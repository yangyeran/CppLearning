// =============================================================================
// 第 16 章 —— Reactor 自测程序
//
// 这个程序在**同一个进程内**启动 Reactor 服务器 + 多个阻塞式客户端线程，
// 自动跑完一整套验证再退出。好处是不用开两个终端就能确认整个库是对的。
//
// 覆盖的测试点：
//   1. 基本回显            —— 连接、发送、接收、关闭的完整流程
//   2. 多连接并发          —— 主从 Reactor 的连接分发
//   3. 大消息分片          —— 应用层 Buffer 处理半包 / 粘包
//   4. 长度前缀协议        —— Buffer 的 prepend / readInt32
//   5. 跨线程 send         —— runInLoop 的任务投递机制
//   6. 高水位回调          —— 慢客户端导致的堆积检测
//   7. 优雅关闭            —— 半关闭（shutdown WR）
//   8. 吞吐压测            —— pipeline 模式下的 QPS
//
// 运行： ch16_reactor_selftest.exe
// =============================================================================

#include "reactor/reactor.h"
#include "demo.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include <stdexcept>

using namespace reactor;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_failures{0};

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "  [通过] " : "  [失败] ") << what << "\n";
    if (!ok) ++g_failures;
}

// -----------------------------------------------------------------------------
// 一个最小的阻塞式同步客户端，专门用来测服务端。
// 故意用阻塞 IO —— 测试代码越简单越好，不要让测试本身成为 bug 来源。
// -----------------------------------------------------------------------------
class SyncClient {
public:
    explicit SyncClient(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ == netc::kInvalidSocket) throw std::runtime_error("客户端 socket 创建失败");
        sockaddr_in addr = netc::make_addr_v4("127.0.0.1", port);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            netc::close_socket(fd_);
            throw std::runtime_error("连接失败: " + netc::last_error_string());
        }
        netc::set_tcp_nodelay(fd_, true);
    }
    ~SyncClient() { close(); }

    SyncClient(const SyncClient&) = delete;
    SyncClient& operator=(const SyncClient&) = delete;

    bool send(std::string_view data) { return netc::send_all(fd_, data); }

    // 阻塞读取恰好 n 字节（TCP 是字节流，一次 recv 不保证读满）
    std::string recvExactly(size_t n) {
        std::string out;
        out.reserve(n);
        char buf[8192];
        while (out.size() < n) {
            long got = netc::recv_n(fd_, buf, (std::min)(sizeof(buf), n - out.size()));
            if (got <= 0) break;
            out.append(buf, static_cast<size_t>(got));
        }
        return out;
    }

    // 读到连接关闭为止
    std::string recvUntilClose() {
        std::string out;
        char buf[8192];
        while (true) {
            long got = netc::recv_n(fd_, buf, sizeof(buf));
            if (got <= 0) break;
            out.append(buf, static_cast<size_t>(got));
        }
        return out;
    }

    void shutdownWrite() {
#ifdef _WIN32
        ::shutdown(fd_, SD_SEND);
#else
        ::shutdown(fd_, SHUT_WR);
#endif
    }

    void close() {
        if (fd_ != netc::kInvalidSocket) { netc::close_socket(fd_); fd_ = netc::kInvalidSocket; }
    }

private:
    netc::socket_t fd_ = netc::kInvalidSocket;
};

// -----------------------------------------------------------------------------
// 测试用的服务器封装：在独立线程里跑一个 EventLoop + TcpServer
// -----------------------------------------------------------------------------
class TestServer {
public:
    TestServer(uint16_t port, int threadNum, MessageCallback onMessage)
        : port_(port), threadNum_(threadNum), onMessage_(std::move(onMessage)) {}

    void start() {
        thread_ = std::thread([this] {
            EventLoop loop;
            TcpServer server(&loop, netc::make_addr_v4(nullptr, port_), "TestServer");
            server.setThreadNum(threadNum_);
            server.setMessageCallback(onMessage_);
            server.setConnectionCallback([this](const TcpConnectionPtr& conn) {
                if (conn->connected()) ++connCount_;
                else                   ++discCount_;
            });
            server.start();

            {
                std::lock_guard<std::mutex> lk(mutex_);
                loop_ = &loop;
            }
            ready_.notify_all();

            loop.loop();

            std::lock_guard<std::mutex> lk(mutex_);
            loop_ = nullptr;
        });

        std::unique_lock<std::mutex> lk(mutex_);
        ready_.wait(lk, [this] { return loop_ != nullptr; });
        // 给 acceptor 一点时间完成 listen（listen 是投递到 loop 里执行的）
        lk.unlock();
        std::this_thread::sleep_for(80ms);
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (loop_) loop_->quit();
        }
        if (thread_.joinable()) thread_.join();
    }

    EventLoop* loop() {
        std::lock_guard<std::mutex> lk(mutex_);
        return loop_;
    }

    int connections()   const { return connCount_.load(); }
    int disconnections() const { return discCount_.load(); }

private:
    uint16_t                port_;
    int                     threadNum_;
    MessageCallback         onMessage_;
    std::thread             thread_;
    std::mutex              mutex_;
    std::condition_variable ready_;
    EventLoop*              loop_ = nullptr;
    std::atomic<int>        connCount_{0};
    std::atomic<int>        discCount_{0};
};

// 端口分配：每个测试用不同端口，避免 TIME_WAIT 干扰
uint16_t nextPort() {
    static uint16_t p = 19100;
    return p++;
}

// =============================================================================
// 测试 1：基本回显
// =============================================================================
void test01_echo() {
    demo::section("测试 1  基本回显");
    uint16_t port = nextPort();

    TestServer server(port, 0, [](const TcpConnectionPtr& conn, Buffer* buf) {
        conn->send(buf->retrieveAllAsString());   // 收到什么就发回什么
    });
    server.start();

    {
        SyncClient c(port);
        check(c.send("hello reactor"), "客户端发送成功");
        std::string echo = c.recvExactly(13);
        check(echo == "hello reactor", "回显内容正确: \"" + echo + "\"");
    }

    std::this_thread::sleep_for(100ms);
    check(server.connections() == 1, "服务端记录到 1 个连接建立");
    check(server.disconnections() == 1, "服务端记录到 1 个连接断开");
    server.stop();
}

// =============================================================================
// 测试 2：多连接并发（验证主从 Reactor 的分发）
// =============================================================================
void test02_concurrent() {
    demo::section("测试 2  多连接并发（4 个从属 Reactor）");
    uint16_t port = nextPort();
    constexpr int kClients = 40;

    TestServer server(port, 4, [](const TcpConnectionPtr& conn, Buffer* buf) {
        conn->send(buf->retrieveAllAsString());
    });
    server.start();

    std::atomic<int> ok{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([port, i, &ok] {
            try {
                SyncClient c(port);
                std::string msg = "client-" + std::to_string(i);
                if (!c.send(msg)) return;
                if (c.recvExactly(msg.size()) == msg) ++ok;
            } catch (const std::exception& e) {
                std::cout << "    客户端 " << i << " 异常: " << e.what() << "\n";
            }
        });
    }
    for (auto& t : clients) t.join();

    check(ok.load() == kClients,
          std::to_string(ok.load()) + "/" + std::to_string(kClients) + " 个并发连接回显正确");

    std::this_thread::sleep_for(150ms);
    check(server.connections() == kClients,
          "服务端记录到 " + std::to_string(server.connections()) + " 个连接");
    server.stop();
}

// =============================================================================
// 测试 3：大消息 —— 验证 Buffer 处理分片
//
// 发 2 MB 数据。TCP 一定会把它切成很多个报文段，服务端会收到多次可读事件。
// 如果没有应用层 Buffer，服务端就会把半个消息当成完整消息处理。
// =============================================================================
void test03_large_message() {
    demo::section("测试 3  大消息分片（2 MB）");
    uint16_t port = nextPort();
    constexpr size_t kSize = 2 * 1024 * 1024;

    std::atomic<size_t> maxBufferSeen{0};
    std::atomic<int>    readEvents{0};

    TestServer server(port, 0, [&](const TcpConnectionPtr& conn, Buffer* buf) {
        ++readEvents;
        size_t cur = buf->readableBytes();
        size_t prev = maxBufferSeen.load();
        while (cur > prev && !maxBufferSeen.compare_exchange_weak(prev, cur)) {}
        conn->send(buf->retrieveAllAsString());
    });
    server.start();

    std::string payload(kSize, 'X');
    for (size_t i = 0; i < kSize; ++i) payload[i] = static_cast<char>('A' + (i % 26));

    {
        SyncClient c(port);
        // 发送要在单独线程做：服务端回显的数据会先填满内核缓冲区，
        // 若不同时收，双方都会阻塞在发送上 —— 这就是经典的死锁场景。
        std::thread sender([&c, &payload] { c.send(payload); });
        std::string echo = c.recvExactly(kSize);
        sender.join();

        check(echo.size() == kSize,
              "收到完整 " + std::to_string(echo.size() / 1024) + " KB");
        check(echo == payload, "2 MB 内容逐字节一致（Buffer 正确处理了分片）");
    }
    std::cout << "    服务端触发了 " << readEvents.load() << " 次可读事件"
              << "，单次 Buffer 峰值 " << maxBufferSeen.load() / 1024 << " KB\n";
    std::cout << "    ^^^ 一条消息被拆成了这么多次事件 —— 这就是必须有 Buffer 的原因\n";
    server.stop();
}

// =============================================================================
// 测试 4：长度前缀协议 —— 解决粘包的标准做法
//
// 协议格式： [4 字节大端长度][载荷]
// 服务端必须循环处理，因为一次可读事件里可能有 0 个、1 个或 N 个完整消息。
// =============================================================================
void test04_length_prefix() {
    demo::section("测试 4  长度前缀协议（粘包处理）");
    uint16_t port = nextPort();

    std::atomic<int> messagesDecoded{0};

    TestServer server(port, 0, [&](const TcpConnectionPtr& conn, Buffer* buf) {
        // 这个 while 循环是长度前缀协议的标准骨架
        while (true) {
            if (buf->readableBytes() < sizeof(int32_t)) break;   // 连长度都不够
            int32_t len = buf->peekInt32();                       // peek 不消费
            if (len < 0 || len > 64 * 1024 * 1024) {              // 防御恶意长度
                conn->forceClose();
                return;
            }
            if (buf->readableBytes() < sizeof(int32_t) + static_cast<size_t>(len)) {
                break;      // 消息还没收全，留在 Buffer 里等下次事件
            }
            buf->retrieve(sizeof(int32_t));                       // 吃掉长度字段
            std::string body = buf->retrieveAsString(static_cast<size_t>(len));
            ++messagesDecoded;

            // 回显时用同样的协议编码。这里正好演示 prepend 的真实用途：
            // 先写载荷，最后再往**前面**塞长度字段 —— 利用预留区，零内存搬移。
            // 对比朴素做法「先算长度、先写头、再写体」，prepend 让你可以
            // 「写完才知道多长」，这在序列化嵌套结构时非常有用。
            Buffer out;
            out.append(body);
            int32_t be = static_cast<int32_t>(::htonl(static_cast<uint32_t>(body.size())));
            out.prepend(&be, sizeof(be));
            conn->send(out.retrieveAllAsString());
        }
    });
    server.start();

    {
        SyncClient c(port);
        // 故意把 3 条消息拼成一个 TCP 包一次发出去 —— 制造「粘包」
        std::vector<std::string> msgs = {"first", "second-message", "3rd"};
        std::string batch;
        for (const auto& m : msgs) {
            uint32_t be = ::htonl(static_cast<uint32_t>(m.size()));
            batch.append(reinterpret_cast<const char*>(&be), 4);
            batch.append(m);
        }
        check(c.send(batch), "把 3 条消息粘在一个包里发送");

        // 逐条接收并解码
        bool allOk = true;
        for (const auto& expect : msgs) {
            std::string hdr = c.recvExactly(4);
            if (hdr.size() != 4) { allOk = false; break; }
            uint32_t be = 0;
            std::memcpy(&be, hdr.data(), 4);
            uint32_t len = ::ntohl(be);
            std::string body = c.recvExactly(len);
            if (body != expect) { allOk = false; break; }
        }
        check(allOk, "3 条消息全部正确拆分并回显");
    }

    std::this_thread::sleep_for(100ms);
    check(messagesDecoded.load() == 3,
          "服务端从粘包中解出 " + std::to_string(messagesDecoded.load()) + " 条完整消息");
    server.stop();
}

// =============================================================================
// 测试 5：跨线程 send（验证 runInLoop 的任务投递）
//
// 主线程直接对一个属于从属 Reactor 的连接调 send()。
// TcpConnection::send 会检测到「当前不在所属 loop 线程」，
// 于是把数据拷一份、包成任务投递过去，并唤醒那个 loop。
// =============================================================================
void test05_cross_thread_send() {
    demo::section("测试 5  跨线程 send");
    uint16_t port = nextPort();

    std::mutex                    connMutex;
    std::vector<TcpConnectionPtr> conns;

    // 服务端收到消息时把连接存进列表（模拟「聊天室成员列表」）。
    // 这个回调在从属 Reactor 线程里执行，而主线程稍后要读这个列表，
    // 所以必须加锁 —— 这正是聊天室广播要面对的同一个问题。
    TestServer server(port, 2, [&](const TcpConnectionPtr& conn, Buffer* buf) {
        buf->retrieveAll();
        std::lock_guard<std::mutex> lk(connMutex);
        conns.push_back(conn);
    });
    server.start();

    {
        SyncClient c(port);
        c.send("register");                       // 触发服务端保存连接
        std::this_thread::sleep_for(120ms);

        TcpConnectionPtr conn;
        {
            std::lock_guard<std::mutex> lk(connMutex);
            check(!conns.empty(), "服务端已保存连接引用");
            if (!conns.empty()) conn = conns.front();
        }

        if (conn) {
            // 从**主线程**推送。连接归某个从属 Reactor 线程所有，
            // 所以这里会走跨线程投递路径。
            check(!conn->getLoop()->isInLoopThread(),
                  "确认当前线程不是该连接所属的 loop 线程");
            conn->send("pushed-from-main-thread");
            std::string got = c.recvExactly(23);
            check(got == "pushed-from-main-thread", "跨线程推送成功: \"" + got + "\"");
        }
    }
    server.stop();
}

// =============================================================================
// 测试 6：优雅关闭（半关闭）
//
// 服务端收到消息后回复，然后 shutdown() 只关写端。
// 客户端应该能读到完整回复，然后读到 EOF。
// =============================================================================
void test06_graceful_shutdown() {
    demo::section("测试 6  优雅关闭（半关闭）");
    uint16_t port = nextPort();

    TestServer server(port, 0, [](const TcpConnectionPtr& conn, Buffer* buf) {
        buf->retrieveAll();
        conn->send("bye-bye");
        conn->shutdown();     // 发完就关写端，但仍可继续接收
    });
    server.start();

    {
        SyncClient c(port);
        c.send("quit");
        std::string all = c.recvUntilClose();
        check(all == "bye-bye",
              "先收到完整回复再收到 EOF（内容 \"" + all + "\"）");
    }
    server.stop();
}

// =============================================================================
// 测试 7：高水位回调（慢客户端保护）
//
// 服务端疯狂发数据，客户端故意不读。数据会堆在服务端的 outputBuffer_ 里。
// 超过高水位线时应该触发回调 —— 生产环境靠这个避免被慢客户端打爆内存。
// =============================================================================
void test07_high_water_mark() {
    demo::section("测试 7  高水位回调（慢客户端保护）");
    uint16_t port = nextPort();

    std::atomic<bool>   hwmFired{false};
    std::atomic<size_t> hwmBytes{0};

    TestServer server(port, 0, [&](const TcpConnectionPtr& conn, Buffer* buf) {
        buf->retrieveAll();
        conn->setHighWaterMarkCallback(
            [&](const TcpConnectionPtr& c, size_t bytes) {
                hwmFired = true;
                hwmBytes = bytes;
                c->forceClose();     // 典型处置：直接踢掉这个慢客户端
            },
            256 * 1024);             // 256 KB 高水位线（真实项目通常几 MB）

        // 猛塞 8 MB。客户端不读，内核缓冲区很快就满，
        // 剩下的全部堆进 outputBuffer_ -> 触发高水位。
        std::string chunk(64 * 1024, 'D');
        for (int i = 0; i < 128; ++i) conn->send(chunk);
    });
    server.start();

    {
        SyncClient c(port);
        c.send("flood-me");
        // 故意不读，只等着
        std::this_thread::sleep_for(600ms);
    }

    check(hwmFired.load(), "高水位回调被触发（堆积 "
                               + std::to_string(hwmBytes.load() / 1024) + " KB）");
    server.stop();
}

// =============================================================================
// 测试 8：吞吐压测
// =============================================================================
void test08_throughput() {
    demo::section("测试 8  吞吐压测");
    uint16_t port = nextPort();
    constexpr int kClients   = 8;
    constexpr int kRoundsEach = 2000;
    const std::string kPayload(256, 'p');

    TestServer server(port, 4, [](const TcpConnectionPtr& conn, Buffer* buf) {
        conn->send(buf->retrieveAllAsString());
    });
    server.start();

    setLogEnabled(false);   // 压测时关掉日志，别让 IO 干扰计时

    std::atomic<long long> totalBytes{0};
    std::atomic<int>       errors{0};

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < kClients; ++i) {
        threads.emplace_back([&] {
            try {
                SyncClient c(port);
                for (int r = 0; r < kRoundsEach; ++r) {
                    if (!c.send(kPayload)) { ++errors; return; }
                    std::string echo = c.recvExactly(kPayload.size());
                    if (echo.size() != kPayload.size()) { ++errors; return; }
                    totalBytes += static_cast<long long>(echo.size()) * 2;
                }
            } catch (const std::exception&) { ++errors; }
        });
    }
    for (auto& t : threads) t.join();
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();

    setLogEnabled(true);

    long long totalReqs = static_cast<long long>(kClients) * kRoundsEach;
    check(errors.load() == 0, "压测过程无错误");
    std::cout << "    " << kClients << " 个客户端 × " << kRoundsEach << " 次往返 = "
              << totalReqs << " 次请求\n";
    std::cout << "    耗时 " << dt << " ms";
    if (dt > 0) {
        std::cout << "，约 " << (totalReqs * 1000 / dt) << " QPS，"
                  << (totalBytes.load() / 1024 / 1024 * 1000 / dt) << " MB/s";
    }
    std::cout << "\n";
    std::cout << "    注意：这是「一问一答」模式，QPS 主要受往返延迟限制，\n"
              << "          不是服务端的吞吐上限。真实压测要用 pipeline 或更多连接。\n";
    server.stop();
}

} // namespace

// =============================================================================
int main() {
    netc::Startup guard;   // Windows 需要 WSAStartup

    demo::title("第 16 章  Reactor 自测");
    std::cout << "  在单进程内启动 Reactor 服务器 + 阻塞式客户端，自动验证全部功能。\n";

    try {
        test01_echo();
        test02_concurrent();
        test03_large_message();
        test04_length_prefix();
        test05_cross_thread_send();
        test06_graceful_shutdown();
        test07_high_water_mark();
        test08_throughput();
    } catch (const std::exception& e) {
        std::cout << "\n  [异常] " << e.what() << "\n";
        ++g_failures;
    }

    demo::title("自测结果");
    if (g_failures.load() == 0) {
        std::cout << "  全部通过。\n";
    } else {
        std::cout << "  有 " << g_failures.load() << " 项失败。\n";
    }

    demo::title("从这个库能学到什么");
    std::cout <<
        "   1. Reactor = 「等就绪」而非「等数据」，一个线程管上万连接\n"
        "   2. one loop per thread：连接绑定到固定线程，连接内状态无需加锁\n"
        "   3. 应用层 Buffer 是非阻塞 IO 的必需品（半包 / 粘包 / 写不完）\n"
        "   4. Channel::tie + enable_shared_from_this 解决「回调执行中对象被销毁」\n"
        "   5. 写完必须 disableWriting()，否则 LT 模式 CPU 打满\n"
        "   6. 跨线程调用统一走 runInLoop + eventfd/socketpair 唤醒\n"
        "   7. 高水位回调是防慢客户端打爆内存的必要手段\n"
        "   8. epoll 只返回就绪 fd（O(就绪数)），poll/select 要遍历全部（O(总数)）\n"
        "\n"
        "  想看单独运行的服务器，跑这两个：\n"
        "    ch16_echo_server.exe 9001        然后 telnet 127.0.0.1 9001\n"
        "    ch16_chat_server.exe 9002        开多个 telnet 互相发消息\n";

    return g_failures.load() == 0 ? 0 : 1;
}
