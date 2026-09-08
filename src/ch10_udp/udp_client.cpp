// =============================================================================
// 第 10 章 —— UDP 客户端 + UDP 特性演示
//
// 用法:
//   ch10_udp_client.exe 127.0.0.1 9999           交互模式
//   ch10_udp_client.exe 127.0.0.1 9999 demo      特性演示（消息边界 / 丢包 / 超时）
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cstring>
#include <cstdlib>

using namespace std::chrono;

// -----------------------------------------------------------------------------
// 给 socket 设置接收超时。
// UDP 没有连接，对端不回你就会永远卡在 recvfrom 上，所以【超时是必须的】。
// -----------------------------------------------------------------------------
bool set_recv_timeout(netc::socket_t s, int ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&tv), sizeof(tv)) == 0;
#else
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

int main(int argc, char** argv) {
    try {
        netc::Startup net_guard;

        std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
        uint16_t    port = (argc > 2) ? static_cast<uint16_t>(std::stoi(argv[2])) : 9999;
        std::string mode = (argc > 3) ? argv[3] : "interactive";

        demo::title("UDP 客户端");
        std::cout << "  目标 " << host << ":" << port << "   模式 " << mode << "\n";

        auto sock = netc::Socket::udp();
        sockaddr_in server = netc::make_addr_v4(host.c_str(), port);
        set_recv_timeout(sock.get(), 1000);      // 1 秒超时

        // 注意：客户端【没有 bind】，内核在第一次 sendto 时自动分配临时端口。
        // 也【没有 connect】—— 虽然 UDP 也可以 connect（见下面说明）。

        char buf[65536];

        if (mode == "demo") {
            // -----------------------------------------------------------------
            demo::title("演示 1：UDP 有消息边界，绝不粘包");
            // -----------------------------------------------------------------
            std::cout << "  连续 sendto 三次: \"AAA\" \"BBB\" \"CCC\"\n";
            for (const char* s : {"AAA", "BBB", "CCC"}) {
                ::sendto(sock.get(), s, 3, 0,
                         reinterpret_cast<sockaddr*>(&server), sizeof(server));
            }
            for (int i = 0; i < 3; ++i) {
                sockaddr_in from{};
                socklen_t   flen = sizeof(from);
                long n = ::recvfrom(sock.get(), buf, static_cast<int>(sizeof(buf)), 0,
                                    reinterpret_cast<sockaddr*>(&from), &flen);
                if (n < 0) { std::cout << "  第 " << i + 1 << " 次接收超时（丢包了）\n"; continue; }
                std::cout << "  第 " << i + 1 << " 次 recvfrom 收到 " << n << " 字节: "
                          << std::string(buf, static_cast<size_t>(n)) << "\n";
            }
            demo::line("");
            demo::line("对比 TCP：TCP 那边三次 send 可能被一次 recv 收成 \"AAABBBCCC\"。");
            demo::line("UDP 永远是「发一个包，收一个包」，一一对应。");
            demo::line("这就是 UDP「保留消息边界」的含义。");

            // -----------------------------------------------------------------
            demo::title("演示 2：超时处理（UDP 没有连接，对端不回你就得自己认栽）");
            // -----------------------------------------------------------------
            {
                auto dead = netc::Socket::udp();
                set_recv_timeout(dead.get(), 500);
                sockaddr_in nowhere = netc::make_addr_v4("127.0.0.1", 59999);  // 假设没人监听
                const char* msg = "hello?";
                ::sendto(dead.get(), msg, 6, 0,
                         reinterpret_cast<sockaddr*>(&nowhere), sizeof(nowhere));

                auto t0 = steady_clock::now();
                sockaddr_in from{}; socklen_t flen = sizeof(from);
                long n = ::recvfrom(dead.get(), buf, static_cast<int>(sizeof(buf)), 0,
                                    reinterpret_cast<sockaddr*>(&from), &flen);
                auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();

                if (n < 0) {
                    std::cout << "  " << ms << " ms 后超时返回（这是预期行为）\n";
                    std::cout << "  错误: " << netc::last_error_string() << "\n";
                }
                demo::line("");
                demo::line("注意：向一个没人监听的 UDP 端口发包，通常不会有任何反馈。");
                demo::line("（有时会收到 ICMP Port Unreachable，Windows 上表现为下次收发报错）");
                demo::line("所以 UDP 应用必须自己做：超时 + 重传 + 序号去重。");
            }

            // -----------------------------------------------------------------
            demo::title("演示 3：吞吐测试");
            // -----------------------------------------------------------------
            {
                const int  kCount = 1000;
                const char payload[64] = "udp-benchmark-payload";
                int sent_ok = 0, recv_ok = 0;

                auto t0 = steady_clock::now();
                for (int i = 0; i < kCount; ++i) {
                    if (::sendto(sock.get(), payload, sizeof(payload), 0,
                                 reinterpret_cast<sockaddr*>(&server), sizeof(server)) > 0) {
                        ++sent_ok;
                    }
                    sockaddr_in from{}; socklen_t flen = sizeof(from);
                    if (::recvfrom(sock.get(), buf, static_cast<int>(sizeof(buf)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen) > 0) {
                        ++recv_ok;
                    }
                }
                auto us = duration_cast<microseconds>(steady_clock::now() - t0).count();

                std::cout << "  发出 " << sent_ok << " 个，收到 " << recv_ok << " 个回复\n";
                std::cout << "  丢失 " << (sent_ok - recv_ok) << " 个 ("
                          << (sent_ok ? 100.0 * (sent_ok - recv_ok) / sent_ok : 0.0) << "%)\n";
                std::cout << "  耗时 " << us / 1000.0 << " ms，平均 "
                          << (recv_ok ? static_cast<double>(us) / recv_ok : 0.0) << " us/次\n";
                demo::line("");
                demo::line("本机回环几乎不丢包；真实网络（尤其跨运营商/无线）丢包率会明显上升。");
            }

            // -----------------------------------------------------------------
            demo::title("UDP 知识要点汇总");
            // -----------------------------------------------------------------
            demo::line("【为什么 UDP 快】");
            demo::line("  没有握手（0 RTT 就能发第一个包）、没有确认重传、没有拥塞控制、");
            demo::line("  头部只有 8 字节（TCP 至少 20）、内核不用维护连接状态。");
            demo::line("");
            demo::line("【UDP 也可以 connect()】");
            demo::line("  connect 一个 UDP socket 不会发任何包，只是在内核里记住默认对端。");
            demo::line("  好处：之后可以直接用 send/recv（不用每次带地址）；");
            demo::line("        能收到 ICMP 错误（比如 Port Unreachable）；性能略好。");
            demo::line("");
            demo::line("【数据报大小怎么定】");
            demo::line("  理论上限 65507 字节，但超过 MTU(1500) 就要 IP 分片，");
            demo::line("  分片中任何一片丢了整个数据报都废掉 -> 丢包率放大。");
            demo::line("  经验值：单个 UDP 负载控制在 1400 字节以内最安全。");
            demo::line("");
            demo::line("【想要可靠的 UDP 怎么做】");
            demo::line("  应用层自己实现：序号 + ACK + 超时重传 + 去重 + 排序 + 流控。");
            demo::line("  这基本就是在重新发明 TCP —— 除非你有特殊需求（比如 QUIC 要");
            demo::line("  避免队头阻塞、要 0-RTT 握手、要在用户态快速迭代），否则用 TCP。");
            demo::line("  成熟方案：KCP（游戏常用）、QUIC/HTTP3、RTP/RTCP（音视频）。");
            demo::line("");
            demo::line("【广播与组播】");
            demo::line("  广播: setsockopt(SO_BROADCAST)，发到 255.255.255.255，只在本网段");
            demo::line("  组播: 加入 224.0.0.0~239.255.255.255 的组，一对多推送");
            demo::line("  TCP 完全不支持这两个 —— 这是 UDP 的独门能力。");

        } else {
            // 交互模式
            std::cout << "\n输入内容按回车发送（输入 quit 退出）:\n";
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.empty()) continue;

                long sent = ::sendto(sock.get(), line.data(), static_cast<int>(line.size()), 0,
                                     reinterpret_cast<sockaddr*>(&server), sizeof(server));
                if (sent < 0) {
                    std::cout << "[客户端] sendto 失败: " << netc::last_error_string() << "\n";
                    break;
                }

                sockaddr_in from{}; socklen_t flen = sizeof(from);
                long n = ::recvfrom(sock.get(), buf, static_cast<int>(sizeof(buf)), 0,
                                    reinterpret_cast<sockaddr*>(&from), &flen);
                if (n < 0) {
                    std::cout << "[客户端] 超时，没收到回复（UDP 不保证送达）\n";
                } else {
                    std::cout << "[回复 来自 " << netc::addr_to_string(from) << "] "
                              << std::string(buf, static_cast<size_t>(n)) << "\n";
                }
                if (line == "quit") break;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
