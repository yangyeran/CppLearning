// =============================================================================
// 第 9 章 —— TCP 回显服务端（Echo Server）
//
// 这是最经典的网络编程入门程序：客户端发什么，服务端原样发回去。
// 麻雀虽小，五脏俱全 —— socket / bind / listen / accept / recv / send / close 全都有。
//
// 运行:
//   1) 先启动服务端:  ch09_tcp_server.exe 8888
//   2) 另开一个终端:  ch09_tcp_client.exe 127.0.0.1 8888
//   也可以用 telnet 127.0.0.1 8888 或 nc 127.0.0.1 8888 测试
//
// 本文件演示三种并发模型，用命令行第二个参数切换：
//   iterative  单线程循环（一次只能服务一个客户端）
//   thread     每连接一个线程（简单，但连接数一多就扛不住）
//   pool       线程池（生产环境的常见折中）
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <functional>
#include <memory>
#include <cstring>
#include <csignal>

// -----------------------------------------------------------------------------
// 全局停止标志（Ctrl+C 时优雅退出）
// -----------------------------------------------------------------------------
static std::atomic<bool> g_running{true};

extern "C" void on_signal(int) { g_running = false; }

// -----------------------------------------------------------------------------
// 创建并配置一个监听 socket
// 这几步是所有 TCP 服务端都一样的固定套路。
// -----------------------------------------------------------------------------
netc::Socket make_listener(uint16_t port, int backlog = 128) {
    // ① socket()：创建一个 TCP socket
    //    AF_INET     = IPv4
    //    SOCK_STREAM = 字节流（即 TCP）
    auto s = netc::Socket::tcp();

    // ② setsockopt()：设置 SO_REUSEADDR
    //    没有它的话，服务端重启时如果上次的连接还在 TIME_WAIT，
    //    bind 会失败并报 "Address already in use"。
    if (!netc::set_reuse_addr(s.get(), true)) {
        std::cout << "[警告] 设置 SO_REUSEADDR 失败: " << netc::last_error_string() << "\n";
    }

    // ③ bind()：把 socket 绑定到本机的 0.0.0.0:port
    //    0.0.0.0 表示「所有网卡都监听」。只想本机访问就填 127.0.0.1。
    sockaddr_in addr = netc::make_addr_v4(nullptr, port);
    if (::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == netc::kSocketError) {
        netc::throw_socket_error("bind()");
    }

    // ④ listen()：从「主动」socket 变成「被动监听」socket
    //    backlog = 已完成三次握手、等待被 accept 取走的连接队列长度。
    //    队列满了，新来的连接会被丢弃 -> 客户端表现为连接超时。
    if (::listen(s.get(), backlog) == netc::kSocketError) {
        netc::throw_socket_error("listen()");
    }

    std::cout << "[服务端] 正在监听 0.0.0.0:" << port << " (backlog=" << backlog << ")\n";
    return s;
}

// -----------------------------------------------------------------------------
// 处理一条已建立的连接：不停地读，读到什么原样写回去
//
// 这个函数里有网络编程最重要的三个细节：
//   1) recv 返回 0  =  对端【正常关闭】了连接（不是错误！）
//   2) recv 返回 -1 =  出错，但要区分 EINTR（被信号打断，应重试）
//   3) send 可能【只发出去一部分】，必须循环发完
// -----------------------------------------------------------------------------
void handle_connection(netc::Socket conn, const std::string& peer) {
    std::cout << "[连接] " << peer << " 已建立\n";

    // 关闭 Nagle 算法：小包立即发送，降低回显延迟
    netc::set_tcp_nodelay(conn.get(), true);

    char   buf[4096];
    size_t total_bytes = 0;

    for (;;) {
        long n = netc::recv_n(conn.get(), buf, sizeof(buf));

        if (n == 0) {
            // 对端调用了 close() 或 shutdown(SHUT_WR)，发来了 FIN
            std::cout << "[连接] " << peer << " 主动关闭 (recv 返回 0)\n";
            break;
        }
        if (n < 0) {
            if (netc::interrupted()) continue;         // 被信号打断，重试
            std::cout << "[连接] " << peer << " recv 出错: "
                      << netc::last_error_string() << "\n";
            break;
        }

        total_bytes += static_cast<size_t>(n);

        // 打印收到的内容（去掉行尾的 \r\n 好看一点）
        std::string text(buf, static_cast<size_t>(n));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        std::cout << "[收到] " << peer << " (" << n << " 字节): " << text << "\n";

        // 原样回显。注意用 send_all 循环发完，不能只调一次 send。
        if (!netc::send_all(conn.get(), buf, static_cast<size_t>(n))) {
            std::cout << "[连接] " << peer << " send 失败: "
                      << netc::last_error_string() << "\n";
            break;
        }

        // 支持客户端发 "quit" 主动结束
        if (text == "quit" || text == "exit") {
            const char* bye = "Bye!\n";
            netc::send_all(conn.get(), bye, std::strlen(bye));
            std::cout << "[连接] " << peer << " 请求退出\n";
            break;
        }
    }

    std::cout << "[连接] " << peer << " 关闭，累计 " << total_bytes << " 字节\n";
    // conn 是 RAII 对象，离开作用域自动 close()
}

// -----------------------------------------------------------------------------
// 一个极简线程池
// -----------------------------------------------------------------------------
class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lk(m_);
                        cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
        std::cout << "[服务端] 线程池已启动，工作线程数 = " << n << "\n";
    }

    ~ThreadPool() {
        { std::lock_guard lk(m_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }

    void submit(std::function<void()> f) {
        { std::lock_guard lk(m_); tasks_.push(std::move(f)); }
        cv_.notify_one();
    }

private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        m_;
    std::condition_variable           cv_;
    bool                              stop_ = false;
};

// -----------------------------------------------------------------------------
// accept 循环：三种并发模型
// -----------------------------------------------------------------------------
void run_server(uint16_t port, const std::string& mode) {
    auto listener = make_listener(port);

    std::unique_ptr<ThreadPool> pool;
    std::vector<std::thread>    threads;

    if (mode == "pool") {
        unsigned n = std::thread::hardware_concurrency();
        pool = std::make_unique<ThreadPool>(n ? n : 4);
    }

    std::cout << "[服务端] 并发模型 = " << mode << "，按 Ctrl+C 退出\n\n";

    while (g_running) {
        sockaddr_in  peer{};
        socklen_t    len = sizeof(peer);

        // accept()：从「已完成握手」队列里取一条连接。
        // 阻塞模式下这里会一直等，直到有客户端连进来。
        // 返回的是【新的 fd】，代表这一条具体连接；listener 继续监听。
        netc::socket_t c = ::accept(listener.get(),
                                    reinterpret_cast<sockaddr*>(&peer), &len);
        if (c == netc::kInvalidSocket) {
            if (netc::interrupted()) continue;
            if (!g_running) break;
            std::cout << "[服务端] accept 失败: " << netc::last_error_string() << "\n";
            continue;
        }

        netc::Socket conn{c};
        std::string  peer_str = netc::addr_to_string(peer);

        if (mode == "iterative") {
            // 模型一：单线程串行处理
            //   优点：最简单，没有任何并发问题
            //   缺点：一次只能服务一个客户端，第二个客户端要一直等
            handle_connection(std::move(conn), peer_str);

        } else if (mode == "thread") {
            // 模型二：每连接一个线程
            //   优点：写起来简单，一个连接一个线性流程
            //   缺点：线程栈默认 1~8MB，一万个连接就要几十 GB；
            //         线程切换开销大。这就是著名的 C10K 问题。
            threads.emplace_back(
                [c2 = std::move(conn), peer_str]() mutable {
                    handle_connection(std::move(c2), peer_str);
                });
            threads.back().detach();

        } else {
            // 模型三：线程池
            //   线程数固定（通常 = CPU 核数或其倍数），避免无限创建线程。
            //   缺点：如果某个连接长时间不发数据，会白占一个工作线程。
            //   彻底的解法是 IO 多路复用 + 非阻塞（第 11 章）。
            auto shared_conn = std::make_shared<netc::Socket>(std::move(conn));
            pool->submit([shared_conn, peer_str] {
                handle_connection(std::move(*shared_conn), peer_str);
            });
        }
    }

    std::cout << "\n[服务端] 已停止\n";
}

// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    try {
        netc::Startup net_guard;

        uint16_t    port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8888;
        std::string mode = (argc > 2) ? argv[2] : "thread";

        if (mode != "iterative" && mode != "thread" && mode != "pool") {
            std::cout << "用法: " << argv[0] << " [端口] [iterative|thread|pool]\n";
            return 1;
        }

        std::signal(SIGINT, on_signal);
#ifdef SIGTERM
        std::signal(SIGTERM, on_signal);
#endif

        demo::title("TCP 回显服务端");
        std::cout << "  测试方法:\n";
        std::cout << "    ch09_tcp_client.exe 127.0.0.1 " << port << "\n";
        std::cout << "    telnet 127.0.0.1 " << port << "\n";
        std::cout << "    (Linux) nc 127.0.0.1 " << port << "\n\n";

        run_server(port, mode);

    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
