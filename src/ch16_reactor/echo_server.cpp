// =============================================================================
// 第 16 章 —— 回显服务器（Reactor 库的最小用例）
//
// 这是用本章 Reactor 库写一个服务器所需的**全部**代码：
// 三行配置 + 一个回调。这就是框架的价值。
//
// 运行：
//     ch16_echo_server.exe [端口=9001] [从属Reactor线程数=4]
//
// 测试：
//     Windows:  telnet 127.0.0.1 9001        （需先启用 telnet 客户端功能）
//               或用 PowerShell 的 Test-NetConnection
//     Linux:    nc 127.0.0.1 9001
//     压测:     ch16_bench_client.exe 127.0.0.1 9001 100 10000
// =============================================================================

#include "reactor/reactor.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>

using namespace reactor;

namespace {
EventLoop* g_loop = nullptr;

// 信号处理：Ctrl+C 时优雅退出，而不是被强杀。
// 注意信号处理函数里只能调用「异步信号安全」的函数，
// 所以这里只设一个原子标志 + 唤醒 loop，绝不做 IO 或加锁。
void onSignal(int) {
    if (g_loop) g_loop->quit();
}
} // namespace

int main(int argc, char* argv[]) {
    netc::Startup guard;

    uint16_t port      = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 9001;
    int      threadNum = (argc > 2) ? std::atoi(argv[2]) : 4;

    std::cout <<
        "============================================================\n"
        "  Reactor 回显服务器\n"
        "============================================================\n"
        "  监听端口       : " << port << "\n"
        "  从属 Reactor 数 : " << threadNum
                              << (threadNum == 0 ? "（单线程模式）" : "") << "\n"
        "  退出           : Ctrl+C\n"
        "\n"
        "  连上来试试：\n"
        "    nc 127.0.0.1 " << port << "        (Linux/macOS)\n"
        "    telnet 127.0.0.1 " << port << "    (Windows)\n"
        "============================================================\n\n";

    try {
        EventLoop loop;
        g_loop = &loop;
        std::signal(SIGINT, onSignal);

        TcpServer server(&loop, netc::make_addr_v4(nullptr, port), "EchoServer");
        server.setThreadNum(threadNum);

        std::atomic<int> liveConns{0};

        // 连接建立 / 断开都会回调这里，用 connected() 区分
        server.setConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->connected()) {
                int n = ++liveConns;
                logInfo("新连接 " + conn->name() + " 来自 " + conn->peerAddr()
                        + "，当前在线 " + std::to_string(n));
                conn->setTcpNoDelay(true);   // 交互式回显，关掉 Nagle 降延迟
                conn->send("欢迎！你发什么我回什么。输入 quit 断开。\n");
            } else {
                int n = --liveConns;
                logInfo("连接关闭 " + conn->name() + "，当前在线 " + std::to_string(n));
            }
        });

        // 核心业务逻辑：整个服务器就这么点代码
        server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
            std::string msg = buf->retrieveAllAsString();

            // 去掉行尾的 \r\n，方便判断命令
            std::string trimmed = msg;
            while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
                trimmed.pop_back();
            }

            if (trimmed == "quit") {
                conn->send("再见。\n");
                conn->shutdown();          // 优雅关闭：发完这句才真正关写端
                return;
            }
            if (trimmed == "stat") {
                conn->send("连接名 " + conn->name() + "\n"
                           "本端   " + conn->localAddr() + "\n"
                           "对端   " + conn->peerAddr() + "\n");
                return;
            }

            logInfo(conn->name() + " 收到 " + std::to_string(msg.size()) + " 字节");
            conn->send(msg);               // 回显
        });

        server.start();
        loop.loop();                        // 阻塞在这里，直到 Ctrl+C

        std::cout << "\n服务器已停止。\n";
    } catch (const std::exception& e) {
        std::cerr << "启动失败: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
