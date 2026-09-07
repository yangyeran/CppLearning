// =============================================================================
// 第 8 章 —— 网络基础：什么是网络，以及 socket 编程的地基
//
// 这一章不涉及真正的连接，只讲清楚概念 + 演示地址/字节序/DNS 这些基础设施。
// 从第 9 章开始才真正收发数据。
//
// 运行： ch08_net_basics.exe
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------------
// 以十六进制打印一段内存 —— 观察字节序的最直接办法
// -----------------------------------------------------------------------------
void hexdump(const void* p, size_t n, std::string_view label) {
    const unsigned char* b = static_cast<const unsigned char*>(p);
    std::cout << "    " << std::left << std::setw(28) << label << ": ";
    for (size_t i = 0; i < n; ++i) {
        std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
                  << static_cast<int>(b[i]) << " ";
    }
    std::cout << std::dec << std::setfill(' ') << "\n";
}

int main() {
    netc::Startup net_guard;      // Windows 上做 WSAStartup；Linux 上什么也不做

    // =========================================================================
    demo::title("8.1 什么是网络 —— 从最基本的问题开始");
    // =========================================================================
    {
        demo::line("问题：A 电脑上的一个程序，怎么把一段数据交给 B 电脑上的另一个程序？");
        demo::line("");
        demo::line("需要解决四件事：");
        demo::line("  1) 找到 B 这台机器          -> IP 地址");
        demo::line("  2) 找到 B 上的那个程序      -> 端口号");
        demo::line("  3) 数据怎么在物理线路上跑   -> 以太网/WiFi/光纤 + 一层层封装");
        demo::line("  4) 丢了怎么办、乱序怎么办   -> TCP（或者不管，那就是 UDP）");
        demo::line("");
        demo::line("「网络协议」就是通信双方约定好的一套规则，规定了：");
        demo::line("  数据长什么样（格式）、什么时候发什么（时序）、出错怎么处理。");
    }

    // =========================================================================
    demo::title("8.2 分层模型 —— 为什么要分层");
    // =========================================================================
    {
        demo::line("分层的核心思想：每一层只管自己的事，把下层当成「能用的黑盒」。");
        demo::line("这样换网卡不用改浏览器，换 WiFi 不用改 TCP。");
        demo::line("");
        demo::line("  OSI 七层（理论模型）        TCP/IP 四层（实际用的）     例子");
        demo::line("  ---------------------------------------------------------------------");
        demo::line("  7 应用层  }");
        demo::line("  6 表示层  }  ---------->    应用层                     HTTP DNS SSH");
        demo::line("  5 会话层  }");
        demo::line("  4 传输层     ---------->    传输层                     TCP UDP");
        demo::line("  3 网络层     ---------->    网络层                     IP ICMP");
        demo::line("  2 数据链路层 }");
        demo::line("  1 物理层     }  ------->    网络接口层                 以太网 WiFi");
        demo::line("");
        demo::line("【socket 编程发生在应用层和传输层的交界处】：");
        demo::line("  你调用 send()，内核负责 TCP 分段、加 IP 头、交给网卡驱动。");
    }

    // =========================================================================
    demo::title("8.3 数据是怎么被一层层包起来的（封装）");
    // =========================================================================
    {
        demo::line("你要发的数据:                                    [ HTTP 报文 ]");
        demo::line("传输层加 TCP 头(20B+):                  [TCP头][ HTTP 报文 ]");
        demo::line("网络层加 IP 头(20B+):            [IP头][TCP头][ HTTP 报文 ]");
        demo::line("链路层加以太网头尾:      [以太网头][IP头][TCP头][ HTTP 报文 ][FCS]");
        demo::line("");
        demo::line("接收端反过来一层层拆开（解封装），每层只看自己的头。");
        demo::line("");
        demo::line("几个关键字段：");
        demo::line("  以太网头: 源MAC / 目的MAC     —— 只在同一个局域网内有意义");
        demo::line("  IP 头   : 源IP / 目的IP / TTL —— 跨网段路由靠它");
        demo::line("  TCP 头  : 源端口 / 目的端口 / 序号 / 确认号 / 标志位 / 窗口");
        demo::line("");
        demo::line("MTU（最大传输单元）：以太网通常 1500 字节。");
        demo::line("超过就要分片，分片会降低效率、丢一片全完，所以 TCP 会主动按 MSS 分段。");
        demo::line("MSS = MTU - IP头 - TCP头 = 1500 - 20 - 20 = 1460 字节（典型值）");
    }

    // =========================================================================
    demo::title("8.4 IP 地址 与 端口");
    // =========================================================================
    {
        demo::line("IPv4: 32 位，写成 4 段十进制  例 192.168.1.100");
        demo::line("IPv6: 128 位，写成 8 组十六进制 例 2001:db8::1");
        demo::line("");
        demo::line("几个特殊地址：");
        demo::line("  127.0.0.1     回环地址，永远指向本机，数据不出网卡");
        demo::line("  0.0.0.0       服务端 bind 时表示「监听本机所有网卡」");
        demo::line("  192.168.x.x   }");
        demo::line("  10.x.x.x      }  私有地址，只在局域网内有效，需要 NAT 才能上外网");
        demo::line("  172.16-31.x.x }");
        demo::line("  255.255.255.255  广播地址");
        demo::line("");
        demo::line("端口：16 位无符号数，0~65535");
        demo::line("  0~1023      知名端口，需要 root/管理员权限才能绑定");
        demo::line("              20/21 FTP  22 SSH  23 Telnet  25 SMTP  53 DNS");
        demo::line("              80 HTTP  443 HTTPS  3306 MySQL  6379 Redis");
        demo::line("  1024~49151  注册端口");
        demo::line("  49152~65535 动态/临时端口，客户端连接时内核自动分配");
        demo::line("");
        demo::line("【四元组】唯一标识一条 TCP 连接：");
        demo::line("  (源IP, 源端口, 目的IP, 目的端口)");
        demo::line("所以一个服务端 80 端口能同时服务几万个客户端 —— 它们源端口不同。");
    }

    // =========================================================================
    demo::title("8.5 字节序（大端 / 小端）—— 网络编程第一个坑");
    // =========================================================================
    {
        std::uint32_t host_value = 0x12345678;

        demo::line("同样一个 32 位整数 0x12345678，在内存里怎么摆？");
        demo::line("  大端 (Big Endian)   : 12 34 56 78   高位字节放低地址（符合人读的顺序）");
        demo::line("  小端 (Little Endian): 78 56 34 12   低位字节放低地址（x86/ARM 都是这个）");
        demo::line("");

        hexdump(&host_value, sizeof(host_value), "主机字节序 0x12345678");

        std::uint32_t net_value = ::htonl(host_value);      // host to network long
        hexdump(&net_value, sizeof(net_value), "网络字节序 htonl 之后");

        std::cout << "\n";
        SHOW(netc::kInvalidSocket == netc::kInvalidSocket);   // 占位，避免未使用告警

        demo::section("检测本机字节序");
        std::uint16_t probe = 0x0102;
        bool little = (*reinterpret_cast<unsigned char*>(&probe) == 0x02);
        std::cout << "    本机是 " << (little ? "小端 (Little Endian)" : "大端 (Big Endian)") << "\n";

        demo::section("四个转换函数（必须背下来）");
        demo::line("  htons(x)  host to network short  —— 16 位，用于【端口】");
        demo::line("  htonl(x)  host to network long   —— 32 位，用于【IPv4 地址】");
        demo::line("  ntohs(x)  network to host short");
        demo::line("  ntohl(x)  network to host long");
        std::cout << "    htons(8080) = 0x" << std::hex << ::htons(8080)
                  << "   ntohs(还原) = " << std::dec << ::ntohs(::htons(8080)) << "\n";

        demo::section("什么时候必须转换");
        demo::line("  1) 填 sockaddr_in 的 sin_port 和 sin_addr  —— 必须转");
        demo::line("  2) 自定义二进制协议里的多字节整数         —— 必须转（约定用网络序）");
        demo::line("  3) 单个 char / 字符串 / 字节数组          —— 不用转（没有字节序问题）");
        demo::line("");
        demo::line("忘记转换的典型症状：本机测试正常（两边都是小端，错得一致），");
        demo::line("和别的架构机器（或者标准协议）通信就全乱 —— 非常难查。");

        demo::section("C++20 起可以用 std::endian 判断");
        demo::line("  if constexpr (std::endian::native == std::endian::little) { ... }");
        demo::line("C++23 起有 std::byteswap 做翻转。");
    }

    // =========================================================================
    demo::title("8.6 地址结构 sockaddr —— 一个历史包袱");
    // =========================================================================
    {
        demo::line("socket API 是 1983 年 BSD 定下来的，那时候还没有 void*，");
        demo::line("所以设计了一个「通用地址」struct sockaddr，各协议族再定自己的结构，");
        demo::line("传参时强制转换成 sockaddr* —— 这就是你到处看到 (sockaddr*) 的原因。");
        demo::line("");
        demo::line("  struct sockaddr {            // 通用，16 字节，只用来做参数类型");
        demo::line("      sa_family_t sa_family;   // AF_INET / AF_INET6 / AF_UNIX");
        demo::line("      char        sa_data[14];");
        demo::line("  };");
        demo::line("");
        demo::line("  struct sockaddr_in {         // IPv4 专用，实际用这个");
        demo::line("      sa_family_t    sin_family;  // AF_INET");
        demo::line("      uint16_t       sin_port;    // 端口，【网络字节序】");
        demo::line("      struct in_addr sin_addr;    // IPv4 地址，【网络字节序】");
        demo::line("      char           sin_zero[8]; // 填充，保证和 sockaddr 一样大");
        demo::line("  };");
        demo::line("");
        demo::line("  struct sockaddr_in6 { ... }  // IPv6，28 字节");
        demo::line("  struct sockaddr_storage      // 足够大，能装下任意协议族的地址");

        demo::section("实际构造一个地址");
        auto addr = netc::make_addr_v4("192.168.1.100", 8080);
        std::cout << "    构造结果: " << netc::addr_to_string(addr) << "\n";
        SHOW(sizeof(sockaddr));
        SHOW(sizeof(sockaddr_in));
        SHOW(sizeof(sockaddr_in6));
        SHOW(sizeof(sockaddr_storage));

        hexdump(&addr, sizeof(addr), "sockaddr_in 的原始字节");
        demo::line("    ^ 可以看到: 02 00 = AF_INET(小端), 1F 90 = 8080(网络序), C0 A8 01 64 = 192.168.1.100");

        demo::section("地址与字符串互转（现代写法）");
        demo::line("  inet_pton(AF_INET, \"1.2.3.4\", &addr.sin_addr)   字符串 -> 二进制");
        demo::line("  inet_ntop(AF_INET, &addr.sin_addr, buf, len)     二进制 -> 字符串");
        demo::line("旧的 inet_addr / inet_ntoa 不支持 IPv6，且 inet_ntoa 用静态缓冲区");
        demo::line("（线程不安全），一律别用。");

        sockaddr_in any = netc::make_addr_v4(nullptr, 9000);
        std::cout << "    监听所有网卡: " << netc::addr_to_string(any) << "\n";
    }

    // =========================================================================
    demo::title("8.7 DNS 解析 —— 域名变 IP");
    // =========================================================================
    {
        demo::line("getaddrinfo 是现代标准做法：同时支持 IPv4/IPv6，线程安全，");
        demo::line("还能顺便把服务名（\"http\"）翻译成端口号。");
        demo::line("");

        auto resolve = [](const char* host, const char* service) {
            addrinfo hints{};
            hints.ai_family   = AF_UNSPEC;      // AF_INET / AF_INET6 / AF_UNSPEC(都要)
            hints.ai_socktype = SOCK_STREAM;    // TCP

            addrinfo* res = nullptr;
            int rc = ::getaddrinfo(host, service, &hints, &res);
            if (rc != 0) {
                std::cout << "    " << host << " -> 解析失败 (可能没联网)\n";
                return;
            }
            std::cout << "    " << host << ":" << service << " ->\n";
            for (addrinfo* p = res; p; p = p->ai_next) {
                char buf[INET6_ADDRSTRLEN]{};
                void* addr_ptr = nullptr;
                int   port = 0;
                if (p->ai_family == AF_INET) {
                    auto* a = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    addr_ptr = &a->sin_addr;  port = ::ntohs(a->sin_port);
                } else if (p->ai_family == AF_INET6) {
                    auto* a = reinterpret_cast<sockaddr_in6*>(p->ai_addr);
                    addr_ptr = &a->sin6_addr; port = ::ntohs(a->sin6_port);
                } else {
                    continue;
                }
                ::inet_ntop(p->ai_family, addr_ptr, buf, sizeof(buf));
                std::cout << "        " << (p->ai_family == AF_INET ? "IPv4" : "IPv6")
                          << "  " << buf << ":" << port << "\n";
            }
            ::freeaddrinfo(res);                // 必须释放！
        };

        resolve("localhost", "http");
        resolve("www.example.com", "443");

        demo::line("");
        demo::line("要点：");
        demo::line("  1) getaddrinfo 返回【链表】，可能有多个地址，应该依次尝试连接");
        demo::line("  2) 结果必须 freeaddrinfo 释放");
        demo::line("  3) 它是【阻塞】调用，慢的 DNS 能卡好几秒 —— 高性能服务要用异步 DNS 库");
        demo::line("  4) 老 API gethostbyname 不支持 IPv6 且线程不安全，别用");
    }

    // =========================================================================
    demo::title("8.8 TCP vs UDP —— 最重要的选型题");
    // =========================================================================
    {
        demo::line("               TCP                          UDP");
        demo::line("  ------------------------------------------------------------------");
        demo::line("  连接         面向连接（先握手）          无连接（直接发）");
        demo::line("  可靠性       保证到达、不重复            不保证，可能丢/重复");
        demo::line("  顺序         保证按发送顺序交付          不保证");
        demo::line("  流量控制     有（滑动窗口）              无");
        demo::line("  拥塞控制     有（慢启动/拥塞避免）       无");
        demo::line("  数据边界     无！是【字节流】            有！一个包就是一个消息");
        demo::line("  头部大小     20 字节起                   8 字节");
        demo::line("  速度         慢一些                      快");
        demo::line("  一对多       不支持                      支持广播/组播");
        demo::line("");
        demo::line("  典型场景     HTTP/HTTPS、SSH、数据库、    DNS、视频直播、游戏、");
        demo::line("               文件传输、邮件               VoIP、SNMP、QUIC 的底座");
        demo::line("");
        demo::line("选型口诀：");
        demo::line("  「数据不能错、不能少、顺序不能乱」-> TCP");
        demo::line("  「宁可丢也不能卡、或者要一对多」  -> UDP（应用层自己做必要的可靠性）");
    }

    // =========================================================================
    demo::title("8.9 TCP 三次握手 与 四次挥手");
    // =========================================================================
    {
        demo::line("【建立连接：三次握手】");
        demo::line("");
        demo::line("   客户端                                        服务端");
        demo::line("     |                                              |");
        demo::line("     |  1. SYN, seq=x                               |");
        demo::line("     | -------------------------------------------> |  (LISTEN)");
        demo::line("     |                                              |");
        demo::line("     |  2. SYN+ACK, seq=y, ack=x+1                  |");
        demo::line("     | <------------------------------------------- |  (SYN_RCVD)");
        demo::line("     |  (SYN_SENT)                                  |");
        demo::line("     |  3. ACK, ack=y+1                             |");
        demo::line("     | -------------------------------------------> |");
        demo::line("     |  (ESTABLISHED)                    (ESTABLISHED)");
        demo::line("");
        demo::line("为什么是三次不是两次？");
        demo::line("  两次的话，服务端无法确认「客户端真的收到了我的响应」。");
        demo::line("  一个延迟很久才到达的旧 SYN 会让服务端白白建立一个连接并一直占资源。");
        demo::line("  第三次 ACK 让双方都确认了「对方的收发能力都正常」。");
        demo::line("");
        demo::line("【断开连接：四次挥手】");
        demo::line("");
        demo::line("   主动方                                        被动方");
        demo::line("     |  1. FIN, seq=u                               |");
        demo::line("     | -------------------------------------------> |");
        demo::line("     |  (FIN_WAIT_1)                    (CLOSE_WAIT)|");
        demo::line("     |  2. ACK, ack=u+1                             |");
        demo::line("     | <------------------------------------------- |");
        demo::line("     |  (FIN_WAIT_2)      <-- 这里被动方还能继续发数据");
        demo::line("     |  3. FIN, seq=w                               |");
        demo::line("     | <------------------------------------------- |");
        demo::line("     |                                   (LAST_ACK) |");
        demo::line("     |  4. ACK, ack=w+1                             |");
        demo::line("     | -------------------------------------------> |");
        demo::line("     |  (TIME_WAIT, 等 2MSL)                (CLOSED)|");
        demo::line("     |  (CLOSED)                                    |");
        demo::line("");
        demo::line("为什么挥手要四次？");
        demo::line("  因为 TCP 是【全双工】的，两个方向要分别关闭。");
        demo::line("  被动方收到 FIN 后可能还有数据没发完，所以 ACK 和自己的 FIN 分开发。");
        demo::line("");
        demo::line("TIME_WAIT 为什么要等 2MSL（Linux 上通常 60 秒）？");
        demo::line("  1) 确保最后那个 ACK 能到达对方；丢了对方会重发 FIN，自己还能再 ACK");
        demo::line("  2) 让本连接的所有残留报文在网络中自然消亡，避免污染下一个同四元组的连接");
        demo::line("");
        demo::line("实际影响：服务端主动关连接会积累大量 TIME_WAIT，占满端口。");
        demo::line("  缓解：SO_REUSEADDR（重启能立刻 bind）、让客户端主动关、开启长连接。");
    }

    // =========================================================================
    demo::title("8.10 socket API 全景 —— 服务端与客户端的调用序列");
    // =========================================================================
    {
        demo::line("        服务端                              客户端");
        demo::line("  ==================================================================");
        demo::line("    socket()      创建 fd");
        demo::line("       |");
        demo::line("    setsockopt()  设 SO_REUSEADDR 等");
        demo::line("       |");
        demo::line("    bind()        绑定 IP:端口");
        demo::line("       |");
        demo::line("    listen()      转为监听态，设置 backlog");
        demo::line("       |                                    socket()   创建 fd");
        demo::line("    accept()      阻塞等待  <-------------  connect()  发起连接");
        demo::line("       |          （三次握手在这里完成）        |");
        demo::line("       |                                        |");
        demo::line("    recv()/send() <-------------------------> send()/recv()");
        demo::line("       |          （在【新的】已连接 fd 上收发）  |");
        demo::line("       |                                        |");
        demo::line("    close()       <-----四次挥手----->      close()");
        demo::line("");
        demo::line("【关键理解】accept() 返回的是一个【新的】fd，代表这一条具体连接。");
        demo::line("原来那个监听 fd 继续监听，不参与数据收发。");

        demo::section("每个调用干了什么");
        demo::line("  socket(AF_INET, SOCK_STREAM, 0)");
        demo::line("      创建一个 socket，返回文件描述符。Linux 上「一切皆文件」，");
        demo::line("      所以 socket fd 可以用 read/write/close，也能放进 select/epoll。");
        demo::line("");
        demo::line("  bind(fd, addr, len)");
        demo::line("      把 fd 和一个本地 IP:端口 关联。客户端一般不用调（内核自动分配）。");
        demo::line("");
        demo::line("  listen(fd, backlog)");
        demo::line("      把 fd 从「主动」变成「被动监听」。");
        demo::line("      backlog 是【已完成握手但还没被 accept 取走】的队列长度。");
        demo::line("      队列满了新连接会被丢弃或拒绝 -> 客户端表现为连接超时。");
        demo::line("");
        demo::line("  accept(fd, &addr, &len)");
        demo::line("      从队列取一个已建立的连接，返回新 fd。阻塞模式下没连接就一直等。");
        demo::line("");
        demo::line("  connect(fd, addr, len)");
        demo::line("      客户端发起三次握手。阻塞模式下要等握手完成或超时。");
        demo::line("");
        demo::line("  send / recv（或 write / read）");
        demo::line("      注意返回值！它们【可能只处理了一部分】，必须循环。");
        demo::line("      recv 返回 0 表示【对端已正常关闭】，返回 -1 才是出错。");
        demo::line("");
        demo::line("  close(fd)");
        demo::line("      引用计数减一，到 0 才真正发 FIN。");
        demo::line("      shutdown(fd, SHUT_WR) 可以只关写方向（半关闭），用来告诉对端「我发完了」。");
    }

    // =========================================================================
    demo::title("8.11 常用 socket 选项");
    // =========================================================================
    {
        auto s = netc::Socket::tcp();
        SHOW(s.valid());

        bool ok1 = netc::set_reuse_addr(s.get(), true);
        bool ok2 = netc::set_tcp_nodelay(s.get(), true);
        bool ok3 = netc::set_nonblocking(s.get(), true);
        SHOW(ok1); SHOW(ok2); SHOW(ok3);

        demo::line("");
        demo::line("  SO_REUSEADDR   允许 bind 处于 TIME_WAIT 的端口。服务端几乎必设，");
        demo::line("                 否则重启会报「Address already in use」。");
        demo::line("  SO_REUSEPORT   （Linux 3.9+）多个进程绑同一端口，内核做负载均衡，");
        demo::line("                 用来做多进程 accept，能避免惊群。");
        demo::line("  TCP_NODELAY    关闭 Nagle 算法。Nagle 会攒小包再发（省带宽但增延迟）。");
        demo::line("                 交互式协议（游戏/RPC/SSH）通常关掉。");
        demo::line("  SO_KEEPALIVE   开启 TCP 保活探测，检测对端「假死」。");
        demo::line("                 默认间隔 2 小时，太长，一般应用层自己做心跳。");
        demo::line("  SO_RCVBUF /    收发缓冲区大小。高带宽长延迟链路（跨国）需要调大，");
        demo::line("  SO_SNDBUF      否则窗口不够，跑不满带宽。");
        demo::line("  SO_LINGER      控制 close 时如何处理未发送数据（慎用）。");
        demo::line("  SO_RCVTIMEO /  收发超时。阻塞模式下防止永久卡死的简易办法。");
        demo::line("  SO_SNDTIMEO");
    }

    // =========================================================================
    demo::title("8.12 阻塞 与 非阻塞");
    // =========================================================================
    {
        demo::line("【阻塞模式】（默认）");
        demo::line("  recv() 没数据 -> 线程挂起，直到有数据或出错");
        demo::line("  优点：代码线性，好写好读");
        demo::line("  缺点：一个线程只能盯一个连接 -> 高并发要开很多线程");
        demo::line("");
        demo::line("【非阻塞模式】");
        demo::line("  recv() 没数据 -> 立刻返回 -1，errno = EAGAIN / EWOULDBLOCK");
        demo::line("  这【不是错误】，只是「现在没有」，必须专门判断。");
        demo::line("  单纯轮询非阻塞 socket 会烧 CPU，所以要配合 IO 多路复用（第 11 章）。");
        demo::line("");
        demo::line("五种 IO 模型（《UNIX 网络编程》经典分类）：");
        demo::line("  1) 阻塞 IO          最简单，一等到底");
        demo::line("  2) 非阻塞 IO        轮询，浪费 CPU");
        demo::line("  3) IO 多路复用      select/poll/epoll，一个线程管很多 fd  <== 主流");
        demo::line("  4) 信号驱动 IO      SIGIO，很少用");
        demo::line("  5) 异步 IO          真正的 AIO（Linux io_uring / Windows IOCP）");
        demo::line("");
        demo::line("注意：前 4 种都是【同步】的 —— 数据从内核拷到用户空间这一步要你自己等。");
        demo::line("     只有第 5 种是内核帮你拷完再通知你。");
    }

    // =========================================================================
    demo::title("8.13 在 Windows 上学 Linux 网络编程");
    // =========================================================================
    {
        demo::line("本教程的网络代码按 POSIX 语义写，通过 src/common/net_compat.h 兼容 Windows。");
        demo::line("");
        demo::line("主要差异：");
        demo::line("  初始化      Linux 不需要    /  Windows 要 WSAStartup + WSACleanup");
        demo::line("  句柄类型    int (就是 fd)   /  SOCKET (UINT_PTR)");
        demo::line("  无效值      -1              /  INVALID_SOCKET");
        demo::line("  关闭        close()         /  closesocket()");
        demo::line("  错误码      errno           /  WSAGetLastError()");
        demo::line("  非阻塞      fcntl(O_NONBLOCK)/ ioctlsocket(FIONBIO)");
        demo::line("  多路复用    select/poll/epoll / select/WSAPoll/IOCP（没有 epoll）");
        demo::line("  read/write  socket 也能用   /  不行，只能 recv/send");
        demo::line("");
        demo::line("要跑真正的 Linux（学 epoll 必须），推荐装 WSL2：");
        demo::line("  1) 管理员 PowerShell 执行:  wsl --install");
        demo::line("  2) 重启，设置 Ubuntu 用户名密码");
        demo::line("  3) sudo apt update && sudo apt install -y build-essential gdb cmake");
        demo::line("  4) 在 WSL 里进入本项目目录（Windows 盘挂载在 /mnt/c/...）");
        demo::line("     cd /mnt/c/Users/coder/Desktop/Claude_Code/CppLearning");
        demo::line("     cmake -B build-linux && cmake --build build-linux -j");
        demo::line("");
        demo::line("Visual Studio 也直接支持 WSL：");
        demo::line("  安装「使用 C++ 的 Linux 开发」工作负载后，CMake 项目可以选");
        demo::line("  「WSL: Ubuntu」作为目标，直接在 Linux 上编译调试。");
    }

    // =========================================================================
    demo::title("8.14 排障工具速查");
    // =========================================================================
    {
        demo::line("  ping <host>              测通不通（ICMP，可能被防火墙挡）");
        demo::line("  telnet <host> <port>     测某个 TCP 端口通不通（最简单有效）");
        demo::line("  nc -zv <host> <port>     同上，Linux 上更常用");
        demo::line("");
        demo::line("  Linux:");
        demo::line("    ss -tlnp               看本机所有监听端口 + 对应进程（推荐）");
        demo::line("    ss -tanp               看所有 TCP 连接和状态");
        demo::line("    netstat -tlnp          老版本用这个");
        demo::line("    lsof -i :8080          谁占了 8080");
        demo::line("    tcpdump -i any -nn port 8080 -w a.pcap    抓包");
        demo::line("    strace -e trace=network ./prog            看程序调了哪些网络系统调用");
        demo::line("");
        demo::line("  Windows:");
        demo::line("    netstat -ano | findstr :8080");
        demo::line("    Get-NetTCPConnection -LocalPort 8080");
        demo::line("");
        demo::line("  Wireshark: 图形化抓包分析，看三次握手/重传/RST 最直观。");
        demo::line("");
        demo::line("  常见现象与原因：");
        demo::line("    Connection refused    目标端口没人监听（或被防火墙 RST）");
        demo::line("    Connection timed out  包被丢弃，没有任何响应（防火墙 DROP / 路由不通）");
        demo::line("    Address already in use bind 的端口被占，或处于 TIME_WAIT 且没设 SO_REUSEADDR");
        demo::line("    Broken pipe / EPIPE   往一个对端已关闭的连接上写");
        demo::line("    Connection reset      对端发了 RST（进程崩了 / 强制关闭 / 收到非法包）");
    }

    std::cout << "\n第 8 章结束。下一章开始写真正能跑的 TCP 服务端和客户端。\n";
    return 0;
}
