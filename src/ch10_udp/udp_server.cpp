// =============================================================================
// 第 10 章 —— UDP 服务端
//
// UDP 和 TCP 的编程差异（这是重点）：
//   1) 不需要 listen / accept / connect  —— 没有「连接」这个概念
//   2) 用 recvfrom / sendto，每次都要带对端地址
//   3) 一次 recvfrom 恰好收到一个完整的数据报 —— 有【消息边界】，不会粘包
//   4) 但可能丢包、乱序、重复 —— 需要的话要自己在应用层补
//
// 运行:
//   ch10_udp_server.exe 9999
//   ch10_udp_client.exe 127.0.0.1 9999
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <string>
#include <unordered_map>
#include <atomic>
#include <csignal>
#include <cstring>

static std::atomic<bool> g_running{true};
extern "C" void on_signal(int) { g_running = false; }

int main(int argc, char** argv) {
    try {
        netc::Startup net_guard;
        std::signal(SIGINT, on_signal);

        uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 9999;

        demo::title("UDP 回显服务端");

        // ① socket()：注意 SOCK_DGRAM（数据报），不是 SOCK_STREAM
        auto sock = netc::Socket::udp();
        netc::set_reuse_addr(sock.get(), true);

        // ② bind()：UDP 服务端仍然要 bind，否则客户端不知道往哪发
        sockaddr_in addr = netc::make_addr_v4(nullptr, port);
        if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
                == netc::kSocketError) {
            netc::throw_socket_error("bind()");
        }

        // ③ 没有 listen()，没有 accept()！直接进入收发循环。
        std::cout << "[UDP] 监听 0.0.0.0:" << port << "，按 Ctrl+C 退出\n";
        std::cout << "[UDP] 注意：没有 listen/accept，因为 UDP 没有连接的概念\n\n";

        // 统计每个客户端发了多少包 —— 说明「无连接」下要自己维护会话状态
        std::unordered_map<std::string, int> stats;

        char buf[65536];      // UDP 数据报最大 65507 字节（65535 - 8 UDP头 - 20 IP头）

        while (g_running) {
            sockaddr_in peer{};
            socklen_t   peer_len = sizeof(peer);

            // recvfrom()：收一个数据报，同时拿到发送方地址。
            // 【关键】一次调用恰好返回一个完整数据报。
            //   - 如果数据报比 buf 大，多出来的部分会被【丢弃】（不像 TCP 会留着下次读）
            //   - 返回 0 是合法的（长度为 0 的数据报），不代表连接关闭
            long n = ::recvfrom(sock.get(), buf, static_cast<int>(sizeof(buf)), 0,
                                reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (n < 0) {
                if (netc::interrupted()) continue;
                if (!g_running) break;
                std::cout << "[UDP] recvfrom 出错: " << netc::last_error_string() << "\n";
                continue;
            }

            std::string peer_str = netc::addr_to_string(peer);
            std::string text(buf, static_cast<size_t>(n));
            int count = ++stats[peer_str];

            std::cout << "[收到] " << peer_str << " 第 " << count << " 个包，"
                      << n << " 字节: " << text << "\n";

            // sendto()：回复。必须带上对方地址，因为没有连接可依赖。
            std::string reply = "echo#" + std::to_string(count) + ": " + text;
            long sent = ::sendto(sock.get(), reply.data(), static_cast<int>(reply.size()), 0,
                                 reinterpret_cast<sockaddr*>(&peer), peer_len);
            if (sent < 0) {
                std::cout << "[UDP] sendto 出错: " << netc::last_error_string() << "\n";
            }

            if (text == "quit") {
                std::cout << "[UDP] 收到 quit，退出\n";
                break;
            }
        }

        std::cout << "\n[UDP] 服务端已停止，共服务过 " << stats.size() << " 个客户端\n";

    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
