// =============================================================================
// 第 19 章 —— 网络练习题参考答案（自测驱动）
//
// 对应练习：附录 B.3 第 9~14 题
//
//   第 9 题  长度前缀协议         LengthPrefixCodec（逐字节喂入验证）
//   第 10 题 心跳与超时           TimerHeap + 真实的空闲连接踢出
//   第 11 题 文件传输             64 位 offset / 断点续传 / MD5 校验
//   第 12 题 简易 RPC             请求 ID 匹配并发响应
//   第 13 题 HTTP 改事件驱动      ☆ 见文末说明（第 16 章已经是答案）
//   第 14 题 WebSocket            握手 + 帧编解码
//
// 第 11、12 题的服务端直接**用第 16 章的 Reactor 库**写 —— 既是答案，
// 也顺便证明那个库真的能用来干活。
//
// 运行： ch19_net_solutions.exe
// =============================================================================

#include "solutions.h"
#include "reactor/reactor.h"
#include "demo.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "    [通过] " : "    [失败] ") << what << "\n";
    if (!ok) ++g_failures;
}

uint16_t nextPort() {
    static uint16_t p = 19500;
    return p++;
}

// =============================================================================
// 第 9 题：长度前缀协议
// =============================================================================
void solve09() {
    demo::section("第 9 题  长度前缀协议（拆包/粘包）");

    // ---- 基本编解码 ----
    {
        sol::LengthPrefixCodec codec;
        std::vector<std::string> got;
        std::string wire = sol::LengthPrefixCodec::encode(R"({"cmd":"ping"})");
        check(wire.size() == 4 + 14, "编码后长度 = 4 + 消息体长度");

        bool ok = codec.feed(wire, [&](std::string&& m) { got.push_back(std::move(m)); });
        check(ok && got.size() == 1 && got[0] == R"({"cmd":"ping"})", "一次喂入完整消息");
    }

    // ---- 核心考点：一次只喂 1 个字节 ----
    {
        sol::LengthPrefixCodec codec;
        std::vector<std::string> got;
        std::string wire = sol::LengthPrefixCodec::encode("hello world");

        for (char c : wire) {
            codec.feed(&c, 1, [&](std::string&& m) { got.push_back(std::move(m)); });
        }
        check(got.size() == 1 && got[0] == "hello world",
              "逐字节喂入（连 4 字节长度头都被拆开）仍能正确组装");
        check(codec.pendingBytes() == 0, "组装完成后无残留");
    }

    // ---- 粘包：3 条消息拼成一个包 ----
    {
        sol::LengthPrefixCodec codec;
        std::vector<std::string> got;
        std::string batch;
        for (const char* m : {"first", "second-longer", "3"}) {
            batch += sol::LengthPrefixCodec::encode(m);
        }
        codec.feed(batch, [&](std::string&& m) { got.push_back(std::move(m)); });
        check(got.size() == 3 && got[0] == "first" && got[1] == "second-longer"
                  && got[2] == "3",
              "一次喂入 3 条粘在一起的消息，全部解出");
    }

    // ---- 混合：半条 + 一条半 + 剩余 ----
    {
        sol::LengthPrefixCodec codec;
        std::vector<std::string> got;
        std::string a = sol::LengthPrefixCodec::encode("AAAA");
        std::string b = sol::LengthPrefixCodec::encode("BBBBBBBB");
        std::string all = a + b;

        std::size_t cut = a.size() + 3;          // 切在 b 的中间
        codec.feed(all.substr(0, cut), [&](std::string&& m) { got.push_back(std::move(m)); });
        check(got.size() == 1 && got[0] == "AAAA", "第一段解出 1 条，第二条残留");
        check(codec.pendingBytes() == 3, "残留 3 字节");

        codec.feed(all.substr(cut), [&](std::string&& m) { got.push_back(std::move(m)); });
        check(got.size() == 2 && got[1] == "BBBBBBBB", "第二段补齐后解出第 2 条");
    }

    // ---- 空消息体 ----
    {
        sol::LengthPrefixCodec codec;
        std::vector<std::string> got;
        codec.feed(sol::LengthPrefixCodec::encode(""),
                   [&](std::string&& m) { got.push_back(std::move(m)); });
        check(got.size() == 1 && got[0].empty(), "长度为 0 的消息也能正确处理");
    }

    // ---- 安全：拒绝超长长度（内存炸弹防护）----
    {
        sol::LengthPrefixCodec codec(1024);      // 上限设成 1 KB
        std::string evil;
        std::uint32_t huge = ::htonl(0xFFFFFFFFu);
        evil.append(reinterpret_cast<const char*>(&huge), 4);

        bool ok = codec.feed(evil, [](std::string&&) {});
        check(!ok, "声明长度 4 GB 时返回 false（不去分配内存）");
        std::cout << "      错误信息: " << codec.lastError() << "\n";
    }

    // ---- 随机分片压测 ----
    {
        sol::LengthPrefixCodec codec;
        std::mt19937 gen(12345);
        std::vector<std::string> expect, got;
        std::string stream;
        for (int i = 0; i < 500; ++i) {
            std::string msg(static_cast<std::size_t>(gen() % 300), static_cast<char>('a' + i % 26));
            expect.push_back(msg);
            stream += sol::LengthPrefixCodec::encode(msg);
        }
        // 以随机大小的块喂入
        std::size_t pos = 0;
        while (pos < stream.size()) {
            std::size_t chunk = 1 + gen() % 64;
            chunk = std::min(chunk, stream.size() - pos);
            codec.feed(stream.data() + pos, chunk,
                       [&](std::string&& m) { got.push_back(std::move(m)); });
            pos += chunk;
        }
        check(got == expect, "500 条消息以随机 1~64 字节分片喂入，全部正确还原");
    }
}

// =============================================================================
// 第 10 题：定时器堆 + 空闲连接踢出
// =============================================================================
void solve10() {
    demo::section("第 10 题  定时器堆与超时踢出");

    // ---- 定时器堆基础 ----
    {
        sol::TimerHeap timers;
        std::vector<int> fired;

        timers.addTimer(60ms, [&] { fired.push_back(3); });
        timers.addTimer(20ms, [&] { fired.push_back(1); });
        timers.addTimer(40ms, [&] { fired.push_back(2); });
        check(timers.size() == 3, "加入 3 个定时器");

        int t = timers.nextTimeoutMs();
        check(t > 0 && t <= 20, "nextTimeoutMs 返回最近的那个（约 20ms，实测 "
                                    + std::to_string(t) + "ms）");

        std::this_thread::sleep_for(80ms);
        std::size_t n = timers.tick();
        check(n == 3, "全部到期后 tick 触发 3 个");
        check((fired == std::vector<int>{1, 2, 3}), "按到期时间顺序触发（小顶堆生效）");
        check(timers.empty(), "触发后堆为空");
        check(timers.nextTimeoutMs(5000) == 5000, "空堆返回默认超时");
    }

    // ---- 取消 ----
    {
        sol::TimerHeap timers;
        std::vector<int> fired;
        timers.addTimer(20ms, [&] { fired.push_back(1); });
        auto id2 = timers.addTimer(30ms, [&] { fired.push_back(2); });
        timers.addTimer(40ms, [&] { fired.push_back(3); });

        timers.cancel(id2);
        std::this_thread::sleep_for(60ms);
        timers.tick();
        check((fired == std::vector<int>{1, 3}), "被取消的定时器不触发");
    }

    // ---- 已过期时返回 0 ----
    {
        sol::TimerHeap timers;
        timers.addTimer(0ms, [] {});
        std::this_thread::sleep_for(5ms);
        check(timers.nextTimeoutMs() == 0,
              "已过期的定时器让 nextTimeoutMs 返回 0（poll 立刻返回，不会漏掉）");
    }

    // ---- 回调里再加定时器（周期性心跳的常见写法）----
    {
        sol::TimerHeap timers;
        int count = 0;
        std::function<void()> reschedule = [&] {
            ++count;
            if (count < 3) timers.addTimer(10ms, reschedule);
        };
        timers.addTimer(10ms, reschedule);
        for (int i = 0; i < 10 && count < 3; ++i) {
            std::this_thread::sleep_for(15ms);
            timers.tick();
        }
        check(count == 3, "回调内部再 addTimer 不会死循环（tick 先取出再调用）");
    }

    // ---- 实战：空闲连接踢出 ----
    //   这是第 10 题的真实场景。用 Reactor 库搭一个服务器，
    //   要求「800ms 没消息就断开」，然后验证：
    //     - 一直发消息的连接不被踢
    //     - 沉默的连接被踢
    {
        uint16_t port = nextPort();
        constexpr auto kIdleTimeout = 800ms;

        reactor::setLogEnabled(false);

        std::atomic<int>  kicked{0};
        std::atomic<bool> running{true};
        reactor::EventLoop* loopPtr = nullptr;
        std::mutex          m;
        std::condition_variable cv;

        std::thread serverThread([&] {
            reactor::EventLoop loop;
            reactor::TcpServer server(&loop, netc::make_addr_v4(nullptr, port), "IdleKicker");

            // 每个连接记录「最后活跃时间」。连接归属单一 loop 线程，
            // 所以这个 map 只被一个线程访问，**不需要加锁**（one loop per thread 的收益）
            auto lastActive = std::make_shared<
                std::map<std::string, sol::TimerHeap::TimePoint>>();
            auto conns = std::make_shared<
                std::map<std::string, reactor::TcpConnectionPtr>>();

            server.setConnectionCallback([=, &kicked](const reactor::TcpConnectionPtr& c) {
                if (c->connected()) {
                    (*lastActive)[c->name()] = sol::TimerHeap::Clock::now();
                    (*conns)[c->name()] = c;
                } else {
                    lastActive->erase(c->name());
                    conns->erase(c->name());
                }
            });
            server.setMessageCallback([=](const reactor::TcpConnectionPtr& c,
                                         reactor::Buffer* buf) {
                buf->retrieveAll();
                (*lastActive)[c->name()] = sol::TimerHeap::Clock::now();   // 刷新活跃时间
                c->send("ack\n");
            });
            server.start();

            { std::lock_guard lk(m); loopPtr = &loop; }
            cv.notify_all();

            // 用一个后台线程模拟「定时扫描」。
            // 生产实现应该把这个逻辑放进 EventLoop 的定时器里
            // （muduo 用 timerfd + TimerQueue），这里为了不改动第 16 章的库
            // 而用 runInLoop 投递 —— 效果相同，且同样保证在 loop 线程执行。
            std::thread scanner([&] {
                while (running.load()) {
                    std::this_thread::sleep_for(100ms);
                    loop.runInLoop([=, &kicked] {
                        auto now = sol::TimerHeap::Clock::now();
                        std::vector<std::string> dead;
                        for (const auto& [name, t] : *lastActive) {
                            if (now - t > kIdleTimeout) dead.push_back(name);
                        }
                        for (const auto& name : dead) {
                            if (auto it = conns->find(name); it != conns->end()) {
                                it->second->send("idle timeout, bye\n");
                                it->second->shutdown();
                                ++kicked;
                            }
                            lastActive->erase(name);
                        }
                    });
                }
            });

            loop.loop();
            running = false;
            scanner.join();
        });

        { std::unique_lock lk(m); cv.wait(lk, [&] { return loopPtr != nullptr; }); }
        std::this_thread::sleep_for(100ms);

        // 客户端 A：持续发消息，不该被踢
        std::atomic<bool> aliveKicked{false};
        std::thread clientA([&] {
            netc::socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            auto addr = netc::make_addr_v4("127.0.0.1", port);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
            for (int i = 0; i < 12; ++i) {          // 12 × 150ms = 1.8s，全程活跃
                netc::send_all(fd, "ping\n");
                char buf[128];
                long n = netc::recv_n(fd, buf, sizeof(buf));
                if (n > 0 && std::string(buf, static_cast<std::size_t>(n)).find("idle") !=
                                 std::string::npos) {
                    aliveKicked = true;
                }
                std::this_thread::sleep_for(150ms);
            }
            netc::close_socket(fd);
        });

        // 客户端 B：连上就沉默，应该被踢
        std::atomic<bool> silentKicked{false};
        std::thread clientB([&] {
            netc::socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            auto addr = netc::make_addr_v4("127.0.0.1", port);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
            netc::send_all(fd, "hello\n");          // 只说一句就沉默
            char buf[256];
            std::string acc;
            while (true) {
                long n = netc::recv_n(fd, buf, sizeof(buf));
                if (n <= 0) break;                   // 被服务端关闭
                acc.append(buf, static_cast<std::size_t>(n));
            }
            if (acc.find("idle timeout") != std::string::npos) silentKicked = true;
            netc::close_socket(fd);
        });

        clientA.join();
        clientB.join();

        if (loopPtr) loopPtr->quit();
        serverThread.join();
        reactor::setLogEnabled(true);

        check(silentKicked.load(), "沉默的连接被踢下线并收到通知");
        check(!aliveKicked.load(), "持续活跃的连接不被踢");
        std::cout << "      共踢出 " << kicked.load() << " 个连接\n";
    }

    std::cout <<
        "      两个要点：\n"
        "        1. 计时必须用 steady_clock。system_clock 会被 NTP 往回调，\n"
        "           一次校时就能让所有定时器失效或提前触发。\n"
        "        2. nextTimeoutMs 的返回值直接喂给 poll/epoll_wait。\n"
        "           已过期要返回 0 而不是负数 —— 负数在 poll 里表示「无限等待」。\n";
}

// =============================================================================
// 摘要算法的标准测试向量
// =============================================================================
void solveDigests() {
    demo::section("摘要算法（第 11、14 题的前置件）");

    // MD5 —— RFC 1321 附录 A.5 的测试向量
    check(sol::Md5::hexOf("") == "d41d8cd98f00b204e9800998ecf8427e", "MD5(\"\")");
    check(sol::Md5::hexOf("a") == "0cc175b9c0f1b6a831c399e269772661", "MD5(\"a\")");
    check(sol::Md5::hexOf("abc") == "900150983cd24fb0d6963f7d28e17f72", "MD5(\"abc\")");
    check(sol::Md5::hexOf("message digest") == "f96b697d7cb7938d525a2f31aaf161d0",
          "MD5(\"message digest\")");
    check(sol::Md5::hexOf("abcdefghijklmnopqrstuvwxyz")
              == "c3fcd3d76192e4007dfb496cca67e13b", "MD5(a-z)");
    check(sol::Md5::hexOf("12345678901234567890123456789012345678901234567890"
                          "123456789012345678901234567890")
              == "57edf4a22be3c955ac49da2e2107b67a", "MD5(80 位数字)");

    // 分块喂入必须与一次性喂入结果相同（流式接口的正确性）
    {
        sol::Md5 m;
        std::string data(100000, 'a');
        for (std::size_t i = 0; i < data.size(); i += 7) {   // 故意用非 64 倍数的块
            m.update(data.data() + i, std::min<std::size_t>(7, data.size() - i));
        }
        check(m.hex() == sol::Md5::hexOf(data), "MD5 分块喂入（7 字节一块）与整体一致");
    }

    // SHA-1 —— RFC 3174
    check(sol::Sha1::hexOf("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA1(\"\")");
    check(sol::Sha1::hexOf("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d",
          "SHA1(\"abc\")");
    check(sol::Sha1::hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
              == "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "SHA1(56 字节向量)");

    // Base64 —— RFC 4648 第 10 节
    check(sol::Base64::encode("") == "", "Base64(\"\")");
    check(sol::Base64::encode("f") == "Zg==", "Base64(\"f\")");
    check(sol::Base64::encode("fo") == "Zm8=", "Base64(\"fo\")");
    check(sol::Base64::encode("foo") == "Zm9v", "Base64(\"foo\")");
    check(sol::Base64::encode("foob") == "Zm9vYg==", "Base64(\"foob\")");
    check(sol::Base64::encode("fooba") == "Zm9vYmE=", "Base64(\"fooba\")");
    check(sol::Base64::encode("foobar") == "Zm9vYmFy", "Base64(\"foobar\")");
    check(sol::Base64::decode("Zm9vYmFy") == "foobar", "Base64 解码往返");

    std::cout <<
        "      注意 MD5 与 SHA-1 的一个易错差异：\n"
        "        MD5  的长度字段是**小端**，SHA-1 是**大端**。\n"
        "        两个算法的分组处理里读入 32 位字的字节序也相反。\n"
        "        照着一个改另一个，这里最容易出错。\n";
}

// =============================================================================
// 第 14 题：WebSocket
// =============================================================================
void solve14() {
    demo::section("第 14 题  WebSocket 握手与帧");

    // ---- 握手：RFC 6455 第 1.3 节的官方示例 ----
    {
        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string accept = sol::websocket::computeAcceptKey(key);
        check(accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
              "Sec-WebSocket-Accept 与 RFC 6455 官方示例一致");
        std::cout << "      key    = " << key << "\n";
        std::cout << "      accept = " << accept << "\n";
    }

    // ---- 完整握手请求 -> 响应 ----
    {
        std::string req =
            "GET /chat HTTP/1.1\r\n"
            "Host: server.example.com\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        auto resp = sol::websocket::makeHandshakeResponse(req);
        check(resp.has_value(), "合法握手请求生成 101 响应");
        check(resp && resp->find("101 Switching Protocols") != std::string::npos,
              "响应包含 101 状态行");
        check(resp && resp->find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
              "响应包含正确的 accept 值");

        // 头名大小写不敏感
        std::string mixedCase =
            "GET / HTTP/1.1\r\nUPGRADE: WebSocket\r\nCONNECTION: keep-alive, Upgrade\r\n"
            "SEC-WEBSOCKET-KEY: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
        check(sol::websocket::makeHandshakeResponse(mixedCase).has_value(),
              "头名大小写混写也能识别（HTTP 头名大小写不敏感）");

        // 缺少必需头 -> 拒绝
        check(!sol::websocket::makeHandshakeResponse(
                  "GET / HTTP/1.1\r\nHost: x\r\n\r\n").has_value(),
              "缺少 Upgrade 头时拒绝握手");
    }

    // ---- 帧编解码往返 ----
    {
        using namespace sol::websocket;

        // 服务端 -> 客户端：不掩码
        Frame out;
        out.opcode  = Opcode::Text;
        out.payload = "Hello";
        std::string wire = encodeFrame(out);
        check(wire.size() == 2 + 5, "短文本帧 = 2 字节头 + 5 字节载荷");
        check(static_cast<std::uint8_t>(wire[0]) == 0x81, "FIN=1 + opcode=Text -> 0x81");

        Frame in;
        std::size_t consumed = 0;
        check(decodeFrame(wire, &in, &consumed) == DecodeResult::Ok, "解码成功");
        check(in.payload == "Hello" && in.fin && !in.masked && in.opcode == Opcode::Text,
              "解码结果正确");
        check(consumed == wire.size(), "消耗字节数正确");

        // 客户端 -> 服务端：必须掩码
        std::uint8_t key[4] = {0x37, 0xfa, 0x21, 0x3d};
        Frame masked;
        masked.payload = "Hello";
        std::string mwire = encodeFrame(masked, key);
        check(mwire.size() == 2 + 4 + 5, "掩码帧多出 4 字节掩码键");
        check((static_cast<std::uint8_t>(mwire[1]) & 0x80) != 0, "MASK 位被置起");
        // RFC 6455 第 5.7 节的官方示例：掩码后应为 7f 9f 4d 51 58
        check(static_cast<std::uint8_t>(mwire[6]) == 0x7f &&
                  static_cast<std::uint8_t>(mwire[7]) == 0x9f &&
                  static_cast<std::uint8_t>(mwire[8]) == 0x4d &&
                  static_cast<std::uint8_t>(mwire[9]) == 0x51 &&
                  static_cast<std::uint8_t>(mwire[10]) == 0x58,
              "掩码后的字节与 RFC 6455 官方示例一致");

        Frame unmasked;
        check(decodeFrame(mwire, &unmasked, &consumed) == DecodeResult::Ok &&
                  unmasked.payload == "Hello" && unmasked.masked,
              "解码时正确去掩码");
    }

    // ---- 三种长度编码路径 ----
    {
        using namespace sol::websocket;
        for (std::size_t len : {std::size_t{0}, std::size_t{125}, std::size_t{126},
                                std::size_t{1000}, std::size_t{65535}, std::size_t{65536},
                                std::size_t{200000}}) {
            Frame f;
            f.opcode  = Opcode::Binary;
            f.payload = std::string(len, 'x');
            std::string wire = encodeFrame(f);

            std::size_t expectHeader = (len <= 125) ? 2 : (len <= 0xFFFF ? 4 : 10);
            bool headerOk = wire.size() == expectHeader + len;

            Frame back;
            std::size_t consumed = 0;
            bool ok = decodeFrame(wire, &back, &consumed) == DecodeResult::Ok
                      && back.payload.size() == len && consumed == wire.size();
            check(headerOk && ok, "长度 " + std::to_string(len) + " 的帧往返正确（头 "
                                      + std::to_string(expectHeader) + " 字节）");
        }
    }

    // ---- 不完整数据必须报 NeedMore 而不是崩溃 ----
    {
        using namespace sol::websocket;
        Frame f;
        f.payload = std::string(70000, 'y');       // 走 8 字节长度路径
        std::string wire = encodeFrame(f);

        Frame back;
        std::size_t consumed = 0;
        for (std::size_t cut : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                                std::size_t{5}, std::size_t{9}, wire.size() - 1}) {
            auto r = decodeFrame(std::string_view(wire).substr(0, cut), &back, &consumed);
            if (r != DecodeResult::NeedMore) {
                check(false, "截断到 " + std::to_string(cut) + " 字节时应返回 NeedMore");
                return;
            }
        }
        check(true, "各种截断长度都正确返回 NeedMore（可重入解码）");

        auto r = decodeFrame(wire, &back, &consumed);
        check(r == DecodeResult::Ok && back.payload.size() == 70000, "数据完整后解码成功");
    }

    // ---- 控制帧与分片 ----
    {
        using namespace sol::websocket;
        Frame ping; ping.opcode = Opcode::Ping; ping.payload = "hb";
        std::string w = encodeFrame(ping);
        Frame back; std::size_t c = 0;
        decodeFrame(w, &back, &c);
        check(back.opcode == Opcode::Ping, "Ping 控制帧");

        Frame frag1; frag1.fin = false; frag1.opcode = Opcode::Text;  frag1.payload = "Hel";
        Frame frag2; frag2.fin = true;  frag2.opcode = Opcode::Continuation; frag2.payload = "lo";
        std::string stream = encodeFrame(frag1) + encodeFrame(frag2);

        std::string assembled;
        std::size_t pos = 0;
        while (pos < stream.size()) {
            Frame f; std::size_t used = 0;
            if (decodeFrame(std::string_view(stream).substr(pos), &f, &used)
                    != DecodeResult::Ok) break;
            assembled += f.payload;
            pos += used;
            if (f.fin) break;
        }
        check(assembled == "Hello", "分片帧（FIN=0 + Continuation）正确拼接");
    }

    std::cout <<
        "      协议里三个最容易忽略的硬规则：\n"
        "        1. 客户端发的帧**必须**掩码，服务端发的**必须不**掩码。\n"
        "           服务端收到未掩码帧要以 1002（协议错误）关闭连接。\n"
        "        2. 掩码不是加密（密钥就在帧里明文传）。它的目的是防止恶意 JS\n"
        "           构造出能欺骗中间代理的字节序列（缓存投毒）。\n"
        "        3. 握手里那个魔数 GUID 同理 —— 不为安全，只为确认对端\n"
        "           真的实现了 WebSocket 而不是某个傻转发的代理。\n";
}

// =============================================================================
// 第 12 题：简易 RPC（服务端用第 16 章的 Reactor 库）
// =============================================================================
class RpcServer {
public:
    using Method = std::function<std::string(const std::string& args)>;

    RpcServer(reactor::EventLoop* loop, uint16_t port, int threads)
        : server_(loop, netc::make_addr_v4(nullptr, port), "RpcServer") {
        server_.setThreadNum(threads);
        server_.setConnectionCallback([this](const reactor::TcpConnectionPtr& c) {
            if (c->connected()) {
                // 每个连接一个独立的解码器，挂在连接的 context 上。
                // 连接归属单一 loop 线程，所以这个 codec 不需要加锁。
                c->setContext(std::make_shared<sol::LengthPrefixCodec>());
            }
        });
        server_.setMessageCallback([this](const reactor::TcpConnectionPtr& c,
                                          reactor::Buffer* buf) {
            auto codec = c->context<sol::LengthPrefixCodec>();
            if (!codec) return;
            std::string data = buf->retrieveAllAsString();
            bool ok = codec->feed(data, [&](std::string&& body) { handleOne(c, body); });
            if (!ok) c->forceClose();          // 协议错误，直接断开
        });
    }

    void registerMethod(std::string name, Method fn) {
        methods_[std::move(name)] = std::move(fn);
    }
    void start() { server_.start(); }

private:
    void handleOne(const reactor::TcpConnectionPtr& conn, const std::string& body) {
        auto req = sol::rpc::decodeRequest(body);
        if (!req) {
            conn->forceClose();
            return;
        }
        sol::rpc::Response resp;
        resp.id = req->id;

        auto it = methods_.find(req->method);
        if (it == methods_.end()) {
            resp.ok = false;
            resp.payload = "未知方法: " + req->method;
        } else {
            try {
                resp.payload = it->second(req->args);
                resp.ok = true;
            } catch (const std::exception& e) {
                resp.ok = false;
                resp.payload = e.what();
            }
        }
        conn->send(sol::rpc::encodeResponse(resp));
    }

    reactor::TcpServer               server_;
    std::map<std::string, Method>    methods_;
};

// 客户端：阻塞式，但支持「并发发出多个请求，靠 ID 匹配响应」
class RpcClient {
public:
    explicit RpcClient(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ == netc::kInvalidSocket) throw std::runtime_error("socket 失败");
        auto addr = netc::make_addr_v4("127.0.0.1", port);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            netc::close_socket(fd_);
            throw std::runtime_error("connect 失败: " + netc::last_error_string());
        }
        netc::set_tcp_nodelay(fd_, true);
    }
    ~RpcClient() { if (fd_ != netc::kInvalidSocket) netc::close_socket(fd_); }

    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    // 只发不等 —— 这才是 RPC 与「一问一答」的区别
    std::uint32_t sendRequest(const std::string& method, const std::string& args) {
        sol::rpc::Request r;
        r.id     = nextId_++;
        r.method = method;
        r.args   = args;
        netc::send_all(fd_, sol::rpc::encodeRequest(r));
        return r.id;
    }

    // 收响应直到把期望的 id 全部收齐。
    // 响应顺序可能与请求顺序不同，所以必须按 id 存进 map。
    std::map<std::uint32_t, sol::rpc::Response> awaitResponses(std::size_t expectCount) {
        std::map<std::uint32_t, sol::rpc::Response> out;
        char buf[4096];
        while (out.size() < expectCount) {
            long n = netc::recv_n(fd_, buf, sizeof(buf));
            if (n <= 0) break;
            codec_.feed(buf, static_cast<std::size_t>(n), [&](std::string&& body) {
                if (auto r = sol::rpc::decodeResponse(body)) out[r->id] = *r;
            });
        }
        return out;
    }

private:
    netc::socket_t          fd_ = netc::kInvalidSocket;
    sol::LengthPrefixCodec  codec_;
    std::uint32_t           nextId_ = 1;
};

void solve12() {
    demo::section("第 12 题  简易 RPC（请求 ID 匹配并发响应）");

    uint16_t port = nextPort();
    reactor::setLogEnabled(false);

    reactor::EventLoop* loopPtr = nullptr;
    std::mutex m;
    std::condition_variable cv;

    std::thread serverThread([&] {
        reactor::EventLoop loop;
        RpcServer server(&loop, port, 2);

        server.registerMethod("echo", [](const std::string& a) { return a; });
        server.registerMethod("upper", [](const std::string& a) {
            std::string s = a;
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        });
        server.registerMethod("add", [](const std::string& a) {
            // args 格式 "3,4"
            auto comma = a.find(',');
            if (comma == std::string::npos) throw std::runtime_error("参数格式应为 a,b");
            long long x = std::stoll(a.substr(0, comma));
            long long y = std::stoll(a.substr(comma + 1));
            return std::to_string(x + y);
        });
        // 故意做一个慢方法：用来证明「响应顺序 ≠ 请求顺序」
        server.registerMethod("slow", [](const std::string& a) {
            std::this_thread::sleep_for(120ms);
            return "slow:" + a;
        });
        server.registerMethod("boom", [](const std::string&) -> std::string {
            throw std::runtime_error("方法内部抛异常");
        });

        server.start();
        { std::lock_guard lk(m); loopPtr = &loop; }
        cv.notify_all();
        loop.loop();
    });

    { std::unique_lock lk(m); cv.wait(lk, [&] { return loopPtr != nullptr; }); }
    std::this_thread::sleep_for(100ms);

    try {
        // ---- 基本调用 ----
        {
            RpcClient c(port);
            auto id = c.sendRequest("echo", "hello");
            auto resps = c.awaitResponses(1);
            check(resps.count(id) == 1 && resps[id].ok && resps[id].payload == "hello",
                  "echo 调用成功");
        }

        // ---- 多方法 ----
        {
            RpcClient c(port);
            auto id1 = c.sendRequest("upper", "abc");
            auto id2 = c.sendRequest("add", "3,4");
            auto resps = c.awaitResponses(2);
            check(resps[id1].payload == "ABC", "upper 返回 ABC");
            check(resps[id2].payload == "7", "add 返回 7");
        }

        // ---- 核心考点：并发请求，慢的先发但后回 ----
        {
            RpcClient c(port);
            // slow 先发（要 120ms），后面 3 个快方法紧随其后
            auto idSlow = c.sendRequest("slow", "first");
            auto idFast1 = c.sendRequest("echo", "f1");
            auto idFast2 = c.sendRequest("echo", "f2");
            auto idFast3 = c.sendRequest("echo", "f3");

            auto resps = c.awaitResponses(4);
            check(resps.size() == 4, "4 个并发请求全部收到响应");
            check(resps[idSlow].payload == "slow:first", "慢请求的响应匹配到正确的 ID");
            check(resps[idFast1].payload == "f1" && resps[idFast2].payload == "f2"
                      && resps[idFast3].payload == "f3",
                  "3 个快请求各自匹配到正确的 ID");
            std::cout << "      这就是请求 ID 的意义：响应可以乱序返回，"
                         "客户端仍能对上号\n";
        }

        // ---- 错误处理 ----
        {
            RpcClient c(port);
            auto id1 = c.sendRequest("nonexistent", "");
            auto id2 = c.sendRequest("boom", "");
            auto resps = c.awaitResponses(2);
            check(!resps[id1].ok && resps[id1].payload.find("未知方法") != std::string::npos,
                  "调用不存在的方法返回错误");
            check(!resps[id2].ok && resps[id2].payload.find("抛异常") != std::string::npos,
                  "方法内抛异常被捕获并转成错误响应");
        }

        // ---- 多客户端并发 ----
        {
            constexpr int kClients = 8;
            constexpr int kCallsEach = 50;
            std::atomic<int> ok{0}, bad{0};
            std::vector<std::thread> threads;
            for (int t = 0; t < kClients; ++t) {
                threads.emplace_back([&, t] {
                    try {
                        RpcClient c(port);
                        std::vector<std::uint32_t> ids;
                        for (int i = 0; i < kCallsEach; ++i) {
                            ids.push_back(c.sendRequest(
                                "add", std::to_string(t) + "," + std::to_string(i)));
                        }
                        auto resps = c.awaitResponses(ids.size());
                        for (int i = 0; i < kCallsEach; ++i) {
                            auto it = resps.find(ids[static_cast<std::size_t>(i)]);
                            if (it != resps.end() && it->second.payload == std::to_string(t + i)) {
                                ++ok;
                            } else {
                                ++bad;
                            }
                        }
                    } catch (const std::exception&) { ++bad; }
                });
            }
            for (auto& th : threads) th.join();
            check(bad.load() == 0 && ok.load() == kClients * kCallsEach,
                  std::to_string(kClients) + " 客户端 × " + std::to_string(kCallsEach)
                      + " 次调用全部正确（" + std::to_string(ok.load()) + "/"
                      + std::to_string(kClients * kCallsEach) + "）");
        }
    } catch (const std::exception& e) {
        check(false, std::string("RPC 测试异常: ") + e.what());
    }

    if (loopPtr) loopPtr->quit();
    serverThread.join();
    reactor::setLogEnabled(true);
}

// =============================================================================
// 第 11 题：文件传输（64 位 offset / 断点续传 / MD5 校验）
// =============================================================================
void solve11() {
    demo::section("第 11 题  文件传输（断点续传 + MD5）");

    // ---- 先单元测试 64 位 offset 的编解码 ----
    // 这一步很关键：>2GB 的 bug 往往不在传输逻辑，而在 offset 溢出。
    {
        for (std::uint64_t off : {std::uint64_t{0},
                                  std::uint64_t{0x7FFFFFFF},        // 2GB - 1
                                  std::uint64_t{0x80000000},        // 恰好 2GB（int32 溢出点）
                                  std::uint64_t{0xFFFFFFFF},        // 4GB - 1（uint32 上限）
                                  std::uint64_t{0x1'0000'0000},     // 4GB
                                  std::uint64_t{0xFF'FFFF'FFFF}}) { // 1TB 量级
            std::string wire = sol::filexfer::encodeGetRequest("big.bin", off);
            // 剥掉 4 字节长度前缀
            std::string body = wire.substr(4);
            auto req = sol::filexfer::decodeGetRequest(body);
            bool ok = req && req->filename == "big.bin" && req->offset == off;
            std::ostringstream ss;
            ss << "offset 0x" << std::hex << off << " 编解码往返正确";
            check(ok, ss.str());
        }
    }

    // ---- 头部编解码 ----
    {
        std::string md5 = std::string(32, 'a');
        std::string wire = sol::filexfer::encodeOkHeader(0x1'2345'6789ull, md5);
        auto h = sol::filexfer::decodeOkHeader(wire.substr(4));
        check(h && h->totalSize == 0x1'2345'6789ull && h->md5hex == md5,
              "响应头（总长 + MD5）编解码正确，总长超过 4GB 也不溢出");
    }

    // ---- 真实传输 + 断点续传 ----
    fs::path tmpDir = fs::temp_directory_path() / "cpplearning_ch19";
    fs::create_directories(tmpDir);
    fs::path srcFile = tmpDir / "source.bin";
    fs::path dstFile = tmpDir / "downloaded.bin";

    // 造一个 3 MB 的测试文件（CI 里不适合真造 2GB，但协议已按 64 位验证过）
    constexpr std::size_t kFileSize = 3 * 1024 * 1024;
    {
        std::ofstream out(srcFile, std::ios::binary);
        std::mt19937 gen(4242);
        std::string chunk(64 * 1024, '\0');
        std::size_t written = 0;
        while (written < kFileSize) {
            for (char& c : chunk) c = static_cast<char>(gen() & 0xFF);
            std::size_t take = std::min(chunk.size(), kFileSize - written);
            out.write(chunk.data(), static_cast<std::streamsize>(take));
            written += take;
        }
    }
    std::string srcMd5;
    {
        std::ifstream in(srcFile, std::ios::binary);
        sol::Md5 m;
        std::string buf(64 * 1024, '\0');
        while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount()) {
            m.update(buf.data(), static_cast<std::size_t>(in.gcount()));
        }
        srcMd5 = m.hex();
    }
    check(fs::file_size(srcFile) == kFileSize, "测试文件已生成（3 MB）");
    std::cout << "      源文件 MD5 = " << srcMd5 << "\n";

    uint16_t port = nextPort();
    reactor::setLogEnabled(false);
    reactor::EventLoop* loopPtr = nullptr;
    std::mutex m;
    std::condition_variable cv;

    std::thread serverThread([&] {
        reactor::EventLoop loop;
        reactor::TcpServer server(&loop, netc::make_addr_v4(nullptr, port), "FileServer");
        server.setThreadNum(1);

        server.setConnectionCallback([](const reactor::TcpConnectionPtr& c) {
            if (c->connected()) c->setContext(std::make_shared<sol::LengthPrefixCodec>());
        });
        server.setMessageCallback([&](const reactor::TcpConnectionPtr& conn,
                                      reactor::Buffer* buf) {
            auto codec = conn->context<sol::LengthPrefixCodec>();
            if (!codec) return;
            std::string data = buf->retrieveAllAsString();
            codec->feed(data, [&](std::string&& body) {
                auto req = sol::filexfer::decodeGetRequest(body);
                if (!req) { conn->forceClose(); return; }

                fs::path path = tmpDir / req->filename;
                std::error_code ec;
                auto total = fs::file_size(path, ec);
                if (ec) {
                    conn->send(sol::LengthPrefixCodec::encode(std::string("ERR\0not found", 13)));
                    conn->shutdown();
                    return;
                }

                conn->send(sol::filexfer::encodeOkHeader(total, srcMd5));

                // 从 offset 开始发送剩余内容。
                // 真实实现应该配合 WriteCompleteCallback 分块发送（避免一次性
                // 把整个大文件读进内存 + 堆爆 outputBuffer）；这里文件不大，
                // 直接一次读完，但用 uint64 的 seek 以验证大文件路径。
                std::ifstream in(path, std::ios::binary);
                in.seekg(static_cast<std::streamoff>(req->offset), std::ios::beg);
                std::string chunk(256 * 1024, '\0');
                while (in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()))
                       || in.gcount()) {
                    conn->send(std::string_view(chunk.data(),
                                                static_cast<std::size_t>(in.gcount())));
                    if (in.gcount() < static_cast<std::streamsize>(chunk.size())) break;
                }
                conn->shutdown();
            });
        });

        server.start();
        { std::lock_guard lk(m); loopPtr = &loop; }
        cv.notify_all();
        loop.loop();
    });

    { std::unique_lock lk(m); cv.wait(lk, [&] { return loopPtr != nullptr; }); }
    std::this_thread::sleep_for(100ms);

    // 阻塞读取恰好 n 字节。TCP 是字节流，一次 recv 不保证读满。
    auto recvExactly = [](netc::socket_t fd, std::size_t n) -> std::string {
        std::string out;
        out.reserve(n);
        char buf[8192];
        while (out.size() < n) {
            long got = netc::recv_n(fd, buf, std::min(sizeof(buf), n - out.size()));
            if (got <= 0) break;
            out.append(buf, static_cast<std::size_t>(got));
        }
        return out;
    };

    // 下载函数：从 offset 开始下载，maxBytes 后主动中断（模拟断线）
    //
    // 【注意这里的协议切换】
    //   响应分两段：先是一条长度前缀的头，之后是**裸字节流**。
    //   所以头必须**精确读取**（4 字节长度 + 恰好那么多字节），
    //   不能用 LengthPrefixCodec —— 它会把头之后的文件字节当成
    //   「下一条消息」缓存起来，导致文件少了一截。
    //   这是「同一连接上协议中途切换」的典型陷阱，HTTP 的
    //   chunked -> raw、WebSocket 的 Upgrade 都有同样的问题。
    auto download = [&](std::uint64_t offset, std::size_t maxBytes,
                        std::uint64_t* outTotal, std::string* outMd5) -> std::size_t {
        netc::socket_t fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        auto addr = netc::make_addr_v4("127.0.0.1", port);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            netc::close_socket(fd);
            return 0;
        }
        netc::send_all(fd, sol::filexfer::encodeGetRequest("source.bin", offset));

        // 第 1 步：精确读 4 字节长度
        std::string lenBytes = recvExactly(fd, 4);
        if (lenBytes.size() != 4) { netc::close_socket(fd); return 0; }
        std::uint32_t be = 0;
        std::memcpy(&be, lenBytes.data(), 4);
        std::uint32_t headerLen = ::ntohl(be);
        if (headerLen == 0 || headerLen > 4096) { netc::close_socket(fd); return 0; }

        // 第 2 步：精确读头部，一字节不多
        std::string header = recvExactly(fd, headerLen);
        if (header.size() != headerLen) { netc::close_socket(fd); return 0; }
        auto h = sol::filexfer::decodeOkHeader(header);
        if (!h) { netc::close_socket(fd); return 0; }     // 可能是 ERR 响应
        *outTotal = h->totalSize;
        *outMd5   = h->md5hex;

        // 第 3 步：之后全部是文件的裸字节
        std::ofstream out(dstFile, offset == 0 ? (std::ios::binary | std::ios::trunc)
                                               : (std::ios::binary | std::ios::app));
        std::size_t bodyWritten = 0;
        char buf[64 * 1024];
        while (bodyWritten < maxBytes) {
            long n = netc::recv_n(fd, buf,
                                  std::min(sizeof(buf), maxBytes - bodyWritten));
            if (n <= 0) break;                            // 传完或断开
            out.write(buf, static_cast<std::streamsize>(n));
            bodyWritten += static_cast<std::size_t>(n);
        }
        out.close();
        netc::close_socket(fd);                           // 提前关 = 模拟断线
        return bodyWritten;
    };

    std::uint64_t total = 0;
    std::string   serverMd5;

    // 第 1 段：只下 1 MB 就"断线"
    std::size_t got1 = download(0, 1024 * 1024, &total, &serverMd5);
    check(total == kFileSize, "服务端报告的总长正确（" + std::to_string(total) + " 字节）");
    check(serverMd5 == srcMd5, "服务端报告的 MD5 与源文件一致");
    check(got1 == 1024 * 1024, "第 1 段下载 1 MB 后主动断开");

    // 第 2 段：从已下载的位置续传
    std::uint64_t resumeFrom = fs::file_size(dstFile);
    std::cout << "      已下载 " << resumeFrom << " 字节，从此处续传\n";
    std::uint64_t t2 = 0; std::string m2;
    std::size_t got2 = download(resumeFrom, kFileSize, &t2, &m2);
    check(got2 > 0, "第 2 段续传收到 " + std::to_string(got2) + " 字节");

    // 校验最终文件
    std::uint64_t finalSize = fs::file_size(dstFile);
    std::string finalMd5;
    {
        std::ifstream in(dstFile, std::ios::binary);
        sol::Md5 md;
        std::string b(64 * 1024, '\0');
        while (in.read(b.data(), static_cast<std::streamsize>(b.size())) || in.gcount()) {
            md.update(b.data(), static_cast<std::size_t>(in.gcount()));
        }
        finalMd5 = md.hex();
    }
    check(finalSize == kFileSize,
          "断点续传后文件大小正确（" + std::to_string(finalSize) + " / "
              + std::to_string(kFileSize) + "）");
    check(finalMd5 == srcMd5, "断点续传后 MD5 与源文件一致（内容完全正确）");
    if (finalMd5 != srcMd5) {
        std::cout << "      源 MD5: " << srcMd5 << "\n      结果 MD5: " << finalMd5 << "\n";
    }

    if (loopPtr) loopPtr->quit();
    serverThread.join();
    reactor::setLogEnabled(true);

    std::error_code ec;
    fs::remove_all(tmpDir, ec);

    std::cout <<
        "      三个大文件传输的必踩坑：\n"
        "        1. offset 必须用 uint64/int64。Windows 上 long 是 32 位，\n"
        "           用它做 offset 在 2GB 处静默溢出。\n"
        "        2. seekg 的参数类型是 std::streamoff，别传 int。\n"
        "        3. 别把整个文件读进内存再 send。应该配合 WriteCompleteCallback\n"
        "           分块发：发完一块才读下一块，这样内存占用恒定。\n"
        "           本例文件小所以简化了，注释里标了正确做法。\n";
}

// =============================================================================
// 第 13 题：把 HTTP 服务器改成事件驱动（说明）
// =============================================================================
void solve13_note() {
    demo::section("第 13 题  HTTP 改事件驱动（☆ 说明）");
    std::cout <<
        "  这题的答案就是**第 16 章**本身 —— 那个 Reactor 库已经完成了\n"
        "  「从线程池 + 阻塞 IO 改成事件驱动」的全部工作。要点回顾：\n"
        "\n"
        "  改造清单：\n"
        "    1. 所有 fd 设为非阻塞          netc::set_nonblocking\n"
        "    2. 每连接一个应用层读缓冲       Buffer + readFd（readv + 栈上 extrabuf）\n"
        "    3. 每连接一个应用层写缓冲       outputBuffer_ + enableWriting()\n"
        "    4. **写完立刻 disableWriting()** 否则 LT 模式 CPU 打满\n"
        "    5. HTTP 解析器必须可重入        不能假设一次能读到完整请求\n"
        "    6. 连接生命周期用 shared_ptr    Channel::tie 防止回调中对象被销毁\n"
        "\n"
        "  其中第 3、4 条是本题的真正难点（题面也这么说）。具体代码见\n"
        "  reactor.cpp 的 TcpConnection::sendInLoop 和 handleWrite —— \n"
        "  尤其注意 sendInLoop 里「输出缓冲区非空时必须排队，不能直接写」\n"
        "  这个保序判断，漏了它会导致数据顺序错乱。\n"
        "\n"
        "  把第 12 章的 HTTP 解析器套到第 16 章的 TcpServer 上就是完整答案：\n"
        "\n"
        "    server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {\n"
        "        auto ctx = conn->context<HttpContext>();\n"
        "        while (ctx->parse(buf)) {                 // 可重入解析\n"
        "            HttpResponse resp = router.handle(ctx->request());\n"
        "            conn->send(resp.serialize());\n"
        "            if (!resp.keepAlive) { conn->shutdown(); break; }\n"
        "            ctx->reset();\n"
        "        }\n"
        "    });\n"
        "\n"
        "  压测对比（题面要求用 wrk）：\n"
        "    wrk -t4 -c1000 -d30s http://127.0.0.1:8080/\n"
        "  线程池版在 c1000 时会因为线程数不足而排队，事件驱动版\n"
        "  连接数不占线程，QPS 通常有数倍差距。注意压测要在 Linux 上做，\n"
        "  Windows 的 WSAPoll 性能和 epoll 不在一个量级。\n";
}

} // namespace

// =============================================================================
int main() {
    netc::Startup guard;

    demo::title("第 19 章  网络练习题参考答案（B.3 第 9~14 题）");
    std::cout <<
        "  纯协议部分在 solutions.h（可单独单元测试）；\n"
        "  需要真实网络的部分直接用第 16 章的 Reactor 库实现。\n";

    try {
        solve09();
        solve10();
        solveDigests();
        solve14();
        solve12();
        solve11();
        solve13_note();
    } catch (const std::exception& e) {
        std::cout << "\n  [异常] " << e.what() << "\n";
        ++g_failures;
    }

    demo::title("小结");
    if (g_failures == 0) {
        std::cout << "  全部自检通过。\n";
    } else {
        std::cout << "  有 " << g_failures << " 项失败。\n";
    }
    std::cout <<
        "\n  这 6 题串起来的知识点：\n"
        "    第 9 题  TCP 是字节流，消息边界必须应用层自己划；解码器必须可重入\n"
        "    第 10 题 定时器用小顶堆 + steady_clock；超时值喂给 poll 时别给负数\n"
        "    第 11 题 大文件三件事：64 位 offset、分块发送、整文件校验\n"
        "    第 12 题 请求 ID 是 RPC 的核心 —— 它让响应可以乱序返回\n"
        "    第 13 题 事件驱动改造的难点在写缓冲管理，不在读\n"
        "    第 14 题 WebSocket 的掩码和魔数 GUID 都不为安全，而为防代理误判\n";

    return g_failures == 0 ? 0 : 1;
}
