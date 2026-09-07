// =============================================================================
// 第 9 章 —— TCP 回显客户端
//
// 用法:
//   ch09_tcp_client.exe [主机] [端口]
//   ch09_tcp_client.exe 127.0.0.1 8888
//
// 输入一行按回车发送，服务端会原样回显。输入 quit 退出。
// 加第三个参数 bench 会跑一个「粘包演示 + 简单压测」。
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace std::chrono;

// -----------------------------------------------------------------------------
// 用 getaddrinfo 解析主机名并依次尝试连接
// 这是【现代标准做法】：同时支持 IPv4/IPv6，一个域名多个 IP 时会挨个试。
// -----------------------------------------------------------------------------
netc::Socket connect_to(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;      // IPv4 和 IPv6 都可以
    hints.ai_socktype = SOCK_STREAM;    // TCP
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        throw std::runtime_error("getaddrinfo 失败: " + host + ":" + port);
    }
    // RAII 保证 freeaddrinfo 一定被调用
    std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> res_guard(res, &::freeaddrinfo);

    for (addrinfo* p = res; p; p = p->ai_next) {
        netc::socket_t s = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == netc::kInvalidSocket) continue;

        netc::Socket sock{s};

        // connect()：客户端在这里发起三次握手。
        // 注意：客户端不需要 bind()，内核会自动分配一个临时端口（49152~65535）。
        if (::connect(sock.get(), p->ai_addr,
                      static_cast<int>(p->ai_addrlen)) != netc::kSocketError) {
            char buf[INET6_ADDRSTRLEN]{};
            if (p->ai_family == AF_INET) {
                auto* a = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
            } else {
                auto* a = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
                ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
            }
            std::cout << "[客户端] 已连接到 " << buf << ":" << port << "\n";

            // 看看内核给我们分配了哪个本地端口
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (::getsockname(sock.get(), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                std::cout << "[客户端] 本地端点 " << netc::addr_to_string(local)
                          << "（内核自动分配的临时端口）\n";
            }
            return sock;
        }
        // 这个地址连不上，试下一个
        std::cout << "[客户端] 尝试失败，换下一个地址: " << netc::last_error_string() << "\n";
    }

    throw std::runtime_error("所有地址都连接失败: " + host + ":" + port);
}

// -----------------------------------------------------------------------------
// 收满 n 字节（对付 TCP 没有消息边界的问题）
// -----------------------------------------------------------------------------
bool recv_exactly(netc::socket_t s, void* buf, size_t n) {
    char*  p    = static_cast<char*>(buf);
    size_t got  = 0;
    while (got < n) {
        long r = netc::recv_n(s, p + got, n - got);
        if (r > 0) { got += static_cast<size_t>(r); continue; }
        if (r < 0 && netc::interrupted()) continue;
        return false;                       // 对端关闭或出错
    }
    return true;
}

// -----------------------------------------------------------------------------
// 交互模式：一行一行地发
// -----------------------------------------------------------------------------
void interactive(netc::Socket& sock) {
    std::cout << "\n输入内容按回车发送（输入 quit 退出）:\n";

    std::string line;
    char        buf[4096];

    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::string msg = line + "\n";
        if (!netc::send_all(sock.get(), msg)) {
            std::cout << "[客户端] 发送失败: " << netc::last_error_string() << "\n";
            break;
        }

        long n = netc::recv_n(sock.get(), buf, sizeof(buf));
        if (n == 0) { std::cout << "[客户端] 服务端已关闭连接\n"; break; }
        if (n < 0)  { std::cout << "[客户端] recv 出错: " << netc::last_error_string() << "\n"; break; }

        std::cout << "[回显] " << std::string(buf, static_cast<size_t>(n));

        if (line == "quit" || line == "exit") break;
    }
}

// -----------------------------------------------------------------------------
// 演示模式：粘包现象 + 简单压测
// -----------------------------------------------------------------------------
void demo_mode(netc::Socket& sock) {
    demo::title("演示 1：TCP 是【字节流】，没有消息边界（俗称粘包/拆包）");

    std::cout << "  连续 send 三次: \"AAA\" \"BBB\" \"CCC\"\n";
    netc::send_all(sock.get(), "AAA");
    netc::send_all(sock.get(), "BBB");
    netc::send_all(sock.get(), "CCC");

    std::this_thread::sleep_for(milliseconds(200));   // 等服务端把三段都回显回来

    char buf[1024]{};
    long n = netc::recv_n(sock.get(), buf, sizeof(buf));
    if (n > 0) {
        std::cout << "  一次 recv 收到 " << n << " 字节: \""
                  << std::string(buf, static_cast<size_t>(n)) << "\"\n";
    }
    demo::line("");
    demo::line("可能是 AAABBBCCC 一次收全（粘包），也可能分几次收到（拆包），");
    demo::line("完全取决于网络时序、Nagle 算法、内核缓冲区状态 —— 不可预测。");
    demo::line("");
    demo::line("【结论】TCP 只保证「字节流按序、不丢、不重」，");
    demo::line("        它【不保证】你的 send 和对方的 recv 一一对应。");
    demo::line("");
    demo::line("【解决办法】应用层自己定消息边界，三种方案:");
    demo::line("  1) 固定长度        每条消息都是 N 字节。简单但浪费。");
    demo::line("  2) 长度前缀 (TLV)  先发 4 字节长度，再发内容。最常用。");
    demo::line("  3) 分隔符          用 \\n 或 \\r\\n\\r\\n 分隔。HTTP 头就是这么干的。");
    demo::line("     缺点：内容里出现分隔符要转义。");

    // -------------------------------------------------------------------------
    demo::title("演示 2：长度前缀协议（正确的做法）");
    // -------------------------------------------------------------------------
    demo::line("发送格式:  [4 字节长度(网络字节序)][消息体]");
    demo::line("接收步骤:  ① 先收满 4 字节拿到长度  ② 再按长度收满消息体");
    demo::line("");
    demo::line("发送端代码:");
    demo::line("    uint32_t len = htonl(static_cast<uint32_t>(body.size()));");
    demo::line("    send_all(fd, &len, 4);");
    demo::line("    send_all(fd, body.data(), body.size());");
    demo::line("");
    demo::line("接收端代码:");
    demo::line("    uint32_t netlen;");
    demo::line("    recv_exactly(fd, &netlen, 4);");
    demo::line("    uint32_t len = ntohl(netlen);");
    demo::line("    if (len > kMaxMessageSize) { 断开连接; }   // 必须校验，否则是 DoS 漏洞");
    demo::line("    std::string body(len, '\\0');");
    demo::line("    recv_exactly(fd, body.data(), len);");
    demo::line("");
    demo::line("【安全提醒】一定要校验长度上限！");
    demo::line("恶意客户端发一个 len = 0xFFFFFFFF，你直接 resize 就把内存吃光了。");

    // -------------------------------------------------------------------------
    demo::title("演示 3：简单往返延迟压测");
    // -------------------------------------------------------------------------
    const int  kRounds = 1000;
    const char kPing[] = "ping";
    char       rbuf[64];

    auto t0 = steady_clock::now();
    int  ok = 0;
    for (int i = 0; i < kRounds; ++i) {
        if (!netc::send_all(sock.get(), kPing, 4)) break;
        if (!recv_exactly(sock.get(), rbuf, 4)) break;
        ++ok;
    }
    auto dt = steady_clock::now() - t0;
    auto us = duration_cast<microseconds>(dt).count();

    std::cout << "  完成 " << ok << " 次请求-响应往返\n";
    std::cout << "  总耗时 " << us / 1000.0 << " ms\n";
    if (ok > 0) {
        std::cout << "  平均往返延迟 (RTT) " << static_cast<double>(us) / ok << " us\n";
        std::cout << "  QPS 约 " << static_cast<long long>(ok * 1000000.0 / (us ? us : 1)) << "\n";
    }
    demo::line("");
    demo::line("回环地址(127.0.0.1)不走网卡，所以延迟极低（几十微秒）。");
    demo::line("同机房通常 0.1~1 ms，跨城 10~50 ms，跨国 100~300 ms。");
    demo::line("这就是为什么「减少往返次数」比「压缩数据量」更能提升体感。");
}

// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    try {
        netc::Startup net_guard;

        std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
        std::string port = (argc > 2) ? argv[2] : "8888";
        std::string mode = (argc > 3) ? argv[3] : "interactive";

        demo::title("TCP 回显客户端");
        std::cout << "  目标: " << host << ":" << port << "   模式: " << mode << "\n\n";

        auto sock = connect_to(host, port);
        netc::set_tcp_nodelay(sock.get(), true);

        if (mode == "bench" || mode == "demo") {
            demo_mode(sock);
        } else {
            interactive(sock);
        }

        std::cout << "\n[客户端] 关闭连接\n";

    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        std::cerr << "提示：请先启动服务端  ch09_tcp_server.exe 8888\n";
        return 1;
    }
    return 0;
}
