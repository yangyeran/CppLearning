// =============================================================================
// 第 11 章 —— IO 多路复用：select / poll / epoll
//
// 核心问题：一个线程怎么同时盯住成千上万个连接？
//
// 本章实现一个【聊天室服务器】：任何客户端发的消息广播给其他所有客户端。
// 用【单线程 + 非阻塞 + IO 多路复用】实现，这就是 Nginx / Redis / Node.js 的模型。
//
// 运行:
//   ch11_multiplex.exe 8890 select     所有平台可用
//   ch11_multiplex.exe 8890 poll       所有平台可用（Windows 用 WSAPoll）
//   ch11_multiplex.exe 8890 epoll      仅 Linux
//   ch11_multiplex.exe explain         只打印原理讲解，不启动服务
//
// 测试:
//   开两三个终端，各跑一个 telnet 127.0.0.1 8890 或 nc 127.0.0.1 8890
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <utility>

static std::atomic<bool> g_running{true};
extern "C" void on_signal(int) { g_running = false; }

// -----------------------------------------------------------------------------
// 一个客户端连接的状态
// 单线程模型下，每个连接的「读了一半」「没发完」的状态都要自己保存。
// 这就是事件驱动编程比阻塞式麻烦的地方 —— 代码从「线性」变成「状态机」。
// -----------------------------------------------------------------------------
struct Conn {
    netc::Socket sock;
    std::string  peer;
    std::string  inbuf;      // 收到但还没处理完的数据（处理粘包）
    std::string  outbuf;     // 想发但内核缓冲区满了、还没发出去的数据

    Conn(netc::Socket s, std::string p) : sock(std::move(s)), peer(std::move(p)) {}
};

// -----------------------------------------------------------------------------
// 创建监听 socket（非阻塞）
// -----------------------------------------------------------------------------
netc::Socket make_listener(uint16_t port) {
    auto s = netc::Socket::tcp();
    netc::set_reuse_addr(s.get(), true);

    sockaddr_in addr = netc::make_addr_v4(nullptr, port);
    if (::bind(s.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == netc::kSocketError)
        netc::throw_socket_error("bind()");
    if (::listen(s.get(), 128) == netc::kSocketError)
        netc::throw_socket_error("listen()");

    // 【关键】监听 socket 也要设成非阻塞。
    // 否则在极端情况下（客户端在 accept 前 RST 掉连接），accept 会意外阻塞住整个事件循环。
    netc::set_nonblocking(s.get(), true);

    std::cout << "[服务器] 监听 0.0.0.0:" << port << "\n";
    return s;
}

// -----------------------------------------------------------------------------
// 业务逻辑：把某个连接发来的整行消息广播给其他人
// 返回 false 表示这个连接应该被关掉
// -----------------------------------------------------------------------------
bool on_readable(Conn& c, std::map<netc::socket_t, Conn>& conns) {
    char buf[4096];

    for (;;) {
        long n = netc::recv_n(c.sock.get(), buf, sizeof(buf));

        if (n > 0) {
            c.inbuf.append(buf, static_cast<size_t>(n));
            // 非阻塞模式下要一直读到 EWOULDBLOCK，否则（边缘触发时）会漏事件
            continue;
        }
        if (n == 0) {
            std::cout << "[断开] " << c.peer << " 正常关闭\n";
            return false;
        }
        // n < 0
        if (netc::would_block()) break;             // 读干净了，这是正常情况
        if (netc::interrupted())  continue;
        std::cout << "[断开] " << c.peer << " 读错误: " << netc::last_error_string() << "\n";
        return false;
    }

    // 按 '\n' 切分出完整的行 —— 这就是「分隔符」式的消息边界处理
    size_t pos;
    while ((pos = c.inbuf.find('\n')) != std::string::npos) {
        std::string line = c.inbuf.substr(0, pos);
        c.inbuf.erase(0, pos + 1);
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::cout << "[消息] " << c.peer << ": " << line << "\n";

        if (line == "quit") return false;

        std::string msg = "[" + c.peer + "] " + line + "\n";
        for (auto& [fd, other] : conns) {
            if (fd == c.sock.get()) continue;               // 不发给自己
            // 简化处理：直接尝试发送。
            // 生产代码应该：先试着发，发不完的塞进 other.outbuf，
            // 并给那个 fd 注册 WRITE 事件，等可写了再继续发。
            netc::send_all(other.sock.get(), msg);
        }
    }

    // 防止恶意客户端发一个超长不带换行的行把内存吃光
    if (c.inbuf.size() > 1 << 20) {
        std::cout << "[断开] " << c.peer << " 单行过长，判定为异常\n";
        return false;
    }
    return true;
}

// #############################################################################
// 方案一：select
// #############################################################################
void run_select(uint16_t port) {
    demo::title("方案一：select");
    demo::line("原理：把关心的 fd 放进三个位图(读/写/异常)，交给内核；");
    demo::line("      内核遍历所有 fd，有就绪的就【修改位图】后返回。");
    demo::line("");
    demo::line("缺点：");
    demo::line("  1) FD_SETSIZE 限制（Linux 默认 1024）—— 连接数硬上限");
    demo::line("  2) 每次调用都要把整个 fd 集合从用户态拷到内核态");
    demo::line("  3) 返回后要【遍历所有 fd】才知道谁就绪 —— O(n)");
    demo::line("  4) fd_set 会被内核改写，每次循环都得重新 FD_SET 一遍");
    demo::line("优点：所有平台都有（Windows 也支持），可移植性最好。\n");

    auto listener = make_listener(port);
    std::map<netc::socket_t, Conn> conns;

    while (g_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listener.get(), &readfds);

        netc::socket_t maxfd = listener.get();
        for (const auto& [fd, c] : conns) {
            FD_SET(fd, &readfds);
            if (fd > maxfd) maxfd = fd;
        }

        timeval tv{1, 0};        // 1 秒超时，好让 Ctrl+C 能生效

        // select 的第一个参数在 Linux 上是 maxfd+1，Windows 上被忽略
        int ready = ::select(static_cast<int>(maxfd) + 1, &readfds, nullptr, nullptr, &tv);

        if (ready < 0) {
            if (netc::interrupted()) continue;
            std::cout << "[错误] select: " << netc::last_error_string() << "\n";
            break;
        }
        if (ready == 0) continue;                        // 超时，没事发生

        // 新连接
        if (FD_ISSET(listener.get(), &readfds)) {
            for (;;) {
                sockaddr_in peer{};
                socklen_t   len = sizeof(peer);
                netc::socket_t c = ::accept(listener.get(),
                                            reinterpret_cast<sockaddr*>(&peer), &len);
                if (c == netc::kInvalidSocket) break;     // EWOULDBLOCK，接完了
                netc::set_nonblocking(c, true);
                std::string ps = netc::addr_to_string(peer);
                std::cout << "[接入] " << ps << "（当前 " << conns.size() + 1 << " 个连接）\n";
                conns.emplace(c, Conn{netc::Socket{c}, ps});
            }
        }

        // 遍历所有连接，检查谁可读 —— 这就是 select 的 O(n) 开销
        std::vector<netc::socket_t> to_close;
        for (auto& [fd, c] : conns) {
            if (!FD_ISSET(fd, &readfds)) continue;
            if (!on_readable(c, conns)) to_close.push_back(fd);
        }
        for (auto fd : to_close) conns.erase(fd);
    }

    std::cout << "\n[服务器] select 循环结束\n";
}

// #############################################################################
// 方案二：poll
// #############################################################################
void run_poll(uint16_t port) {
    demo::title("方案二：poll");
    demo::line("原理：和 select 一样是「每次传全量」，但用 pollfd 数组代替位图。");
    demo::line("");
    demo::line("相比 select 的改进：");
    demo::line("  1) 没有 FD_SETSIZE 限制，数组多大都行");
    demo::line("  2) events(关心什么) 和 revents(发生了什么) 分开存，");
    demo::line("     不用每次重新设置整个集合");
    demo::line("仍然存在的问题：");
    demo::line("  1) 每次调用还是要把整个数组拷进内核");
    demo::line("  2) 返回后还是要遍历整个数组找就绪的 —— 依然 O(n)");
    demo::line("Windows 上对应 WSAPoll（Vista 起）。\n");

    auto listener = make_listener(port);
    std::map<netc::socket_t, Conn> conns;

    while (g_running) {
#ifdef _WIN32
        std::vector<WSAPOLLFD> fds;
#else
        std::vector<pollfd> fds;
#endif
        fds.push_back({listener.get(), POLLIN, 0});
        for (const auto& [fd, c] : conns) fds.push_back({fd, POLLIN, 0});

#ifdef _WIN32
        int ready = ::WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), 1000);
#else
        int ready = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 1000);
#endif
        if (ready < 0) {
            if (netc::interrupted()) continue;
            std::cout << "[错误] poll: " << netc::last_error_string() << "\n";
            break;
        }
        if (ready == 0) continue;

        // fds[0] 是监听 socket
        if (fds[0].revents & POLLIN) {
            for (;;) {
                sockaddr_in peer{};
                socklen_t   len = sizeof(peer);
                netc::socket_t c = ::accept(listener.get(),
                                            reinterpret_cast<sockaddr*>(&peer), &len);
                if (c == netc::kInvalidSocket) break;
                netc::set_nonblocking(c, true);
                std::string ps = netc::addr_to_string(peer);
                std::cout << "[接入] " << ps << "（当前 " << conns.size() + 1 << " 个连接）\n";
                conns.emplace(c, Conn{netc::Socket{c}, ps});
            }
        }

        std::vector<netc::socket_t> to_close;
        for (size_t i = 1; i < fds.size(); ++i) {
            auto& pfd = fds[i];
            if (pfd.revents == 0) continue;

            auto it = conns.find(pfd.fd);
            if (it == conns.end()) continue;

            // POLLHUP: 对端挂断  POLLERR: 出错  POLLNVAL: fd 非法
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                std::cout << "[断开] " << it->second.peer << " (POLLHUP/ERR)\n";
                to_close.push_back(pfd.fd);
                continue;
            }
            if (pfd.revents & POLLIN) {
                if (!on_readable(it->second, conns)) to_close.push_back(pfd.fd);
            }
        }
        for (auto fd : to_close) conns.erase(fd);
    }

    std::cout << "\n[服务器] poll 循环结束\n";
}

// #############################################################################
// 方案三：epoll（Linux 独有）
// #############################################################################
#ifdef __linux__
void run_epoll(uint16_t port) {
    demo::title("方案三：epoll（Linux）");
    demo::line("原理：三个系统调用分工明确");
    demo::line("  epoll_create1()  在【内核里】建一个事件表");
    demo::line("  epoll_ctl()      增删改要关心的 fd（只在变化时调用一次）");
    demo::line("  epoll_wait()     取【已就绪】的 fd 列表");
    demo::line("");
    demo::line("为什么快：");
    demo::line("  1) fd 集合常驻内核，不用每次来回拷贝");
    demo::line("  2) 内核用红黑树管理 fd，用就绪链表收集事件");
    demo::line("  3) epoll_wait 直接返回【就绪的那几个】，复杂度 O(就绪数) 而不是 O(总数)");
    demo::line("  这就是 10 万连接下 epoll 碾压 select/poll 的根本原因。\n");

    auto listener = make_listener(port);

    int ep = ::epoll_create1(0);
    if (ep < 0) netc::throw_socket_error("epoll_create1()");

    auto ep_add = [&](int fd, uint32_t events) {
        epoll_event ev{};
        ev.events  = events;
        ev.data.fd = fd;
        return ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0;
    };

    ep_add(listener.get(), EPOLLIN);

    std::map<netc::socket_t, Conn> conns;
    std::vector<epoll_event>       events(1024);

    while (g_running) {
        int n = ::epoll_wait(ep, events.data(), static_cast<int>(events.size()), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cout << "[错误] epoll_wait: " << netc::last_error_string() << "\n";
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == listener.get()) {
                for (;;) {
                    sockaddr_in peer{};
                    socklen_t   len = sizeof(peer);
                    int c = ::accept(listener.get(),
                                     reinterpret_cast<sockaddr*>(&peer), &len);
                    if (c < 0) break;
                    netc::set_nonblocking(c, true);
                    std::string ps = netc::addr_to_string(peer);
                    std::cout << "[接入] " << ps << "（当前 " << conns.size() + 1 << " 个连接）\n";
                    conns.emplace(c, Conn{netc::Socket{c}, ps});
                    // 这里用【水平触发 LT】，最不容易写错。
                    // 想用边缘触发就改成 EPOLLIN | EPOLLET，
                    // 但那样必须一次把数据读到 EAGAIN 为止，否则会永久丢事件。
                    ep_add(c, EPOLLIN);
                }
                continue;
            }

            auto it = conns.find(fd);
            if (it == conns.end()) continue;

            if (ev & (EPOLLHUP | EPOLLERR)) {
                std::cout << "[断开] " << it->second.peer << " (EPOLLHUP/ERR)\n";
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                conns.erase(it);
                continue;
            }
            if (ev & EPOLLIN) {
                if (!on_readable(it->second, conns)) {
                    ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                    conns.erase(it);
                }
            }
        }
    }

    ::close(ep);
    std::cout << "\n[服务器] epoll 循环结束\n";
}
#endif

// #############################################################################
// 原理讲解（不启动服务，纯输出）
// #############################################################################
void explain() {
    demo::title("11.1 为什么需要 IO 多路复用");
    demo::line("阻塞式「一连接一线程」的问题（著名的 C10K 问题）：");
    demo::line("  线程栈默认 1~8 MB  ->  1 万连接就是 10~80 GB 内存");
    demo::line("  线程切换要保存恢复寄存器、刷 TLB  ->  上下文切换开销随线程数暴涨");
    demo::line("  大部分线程其实在「等数据」，什么活都没干");
    demo::line("");
    demo::line("多路复用的思路：");
    demo::line("  一个线程问内核：「这 10000 个 fd 里，现在哪些有数据了？」");
    demo::line("  内核回答后，线程只处理那几个就绪的 —— 一个线程顶一万个线程用。");

    demo::title("11.2 三者对比");
    demo::line("                 select          poll            epoll");
    demo::line("  ----------------------------------------------------------------");
    demo::line("  fd 上限        FD_SETSIZE      无              无");
    demo::line("                 (通常 1024)");
    demo::line("  数据结构       位图 fd_set     pollfd 数组     内核红黑树 + 就绪链表");
    demo::line("  每次调用开销   全量拷贝        全量拷贝        只在 epoll_ctl 时改一次");
    demo::line("  查找就绪 fd    遍历全部 O(n)   遍历全部 O(n)   直接返回就绪表 O(1)");
    demo::line("  触发模式       只有 LT         只有 LT         LT / ET 都支持");
    demo::line("  可移植性       全平台          POSIX + WSAPoll 仅 Linux");
    demo::line("  适合场景       连接少          连接中等        海量连接");
    demo::line("");
    demo::line("其它平台的对应物：");
    demo::line("  BSD / macOS : kqueue     （思路和 epoll 类似）");
    demo::line("  Windows     : IOCP       （真·异步，模型不同：完成通知而非就绪通知）");
    demo::line("  Linux 新方案: io_uring   （真·异步，比 epoll 更进一步，减少系统调用）");
    demo::line("  跨平台封装  : libevent / libuv / Boost.Asio  （生产项目一般直接用这些）");

    demo::title("11.3 水平触发 LT vs 边缘触发 ET（面试高频）");
    demo::line("水平触发 (Level Triggered, 默认)：");
    demo::line("  「只要缓冲区里【还有】数据，每次 epoll_wait 都会通知你」");
    demo::line("  好处：你这次没读完，下次还会提醒 -> 不容易写错");
    demo::line("  坏处：没读完就会反复触发，理论上效率略低");
    demo::line("");
    demo::line("边缘触发 (Edge Triggered, EPOLLET)：");
    demo::line("  「只在【状态发生变化】时通知一次」（从没数据变成有数据）");
    demo::line("  好处：通知次数少，效率高");
    demo::line("  坏处：必须【一次把数据读干净】（循环 read 直到返回 EAGAIN），");
    demo::line("        否则剩下的数据永远不会再触发通知 -> 连接假死，极难排查");
    demo::line("");
    demo::line("  用 ET 的三条铁律：");
    demo::line("    1) fd 必须是非阻塞的（否则最后一次 read 会永久阻塞）");
    demo::line("    2) 必须循环读到 EAGAIN / EWOULDBLOCK");
    demo::line("    3) 写也一样：写到 EAGAIN，然后注册 EPOLLOUT 等下次可写");
    demo::line("");
    demo::line("  经验：先用 LT 把功能写对，确实成为瓶颈了再考虑 ET。");

    demo::title("11.4 Reactor 模式 —— 事件驱动服务器的标准架构");
    demo::line("");
    demo::line("        ┌─────────────────────────────────────────────┐");
    demo::line("        │              事件循环 (Event Loop)          │");
    demo::line("        │                                             │");
    demo::line("        │   epoll_wait()  <--- 等待事件               │");
    demo::line("        │        |                                    │");
    demo::line("        │        v                                    │");
    demo::line("        │   Demultiplexer 分发                        │");
    demo::line("        │     /       |        \\                     │");
    demo::line("        │    v        v         v                     │");
    demo::line("        │ AcceptHandler ReadHandler WriteHandler      │");
    demo::line("        │    |          |           |                 │");
    demo::line("        │    v          v           v                 │");
    demo::line("        │  新连接    解析+业务    发送剩余数据         │");
    demo::line("        └─────────────────────────────────────────────┘");
    demo::line("");
    demo::line("单 Reactor 单线程   : Redis 的模型。简单，无锁，但只用一个核。");
    demo::line("单 Reactor 多线程   : IO 在主线程，业务丢给线程池。适合业务较重的场景。");
    demo::line("主从 Reactor 多线程 : 主 Reactor 只 accept，分给多个子 Reactor 各自跑");
    demo::line("                      事件循环。Nginx / Netty / muduo 都是这个模型。");
    demo::line("");
    demo::line("Proactor 模式：Reactor 是「可以读了，你自己去读」；");
    demo::line("               Proactor 是「我已经帮你读好了，数据在这」。");
    demo::line("               Windows IOCP 和 Linux io_uring 属于 Proactor。");

    demo::title("11.5 事件驱动编程的实际难点");
    demo::line("1) 代码从线性变状态机");
    demo::line("   阻塞式:  auto req = read_request(); auto resp = handle(req); write(resp);");
    demo::line("   事件式:  要把「读了一半」「处理中」「没发完」都存成显式状态。");
    demo::line("   缓解手段：协程（C++20 coroutine / Go goroutine）把线性写法还给你。");
    demo::line("");
    demo::line("2) 绝对不能在事件循环里做阻塞操作");
    demo::line("   一次同步的数据库查询、一次文件读、一次 DNS 解析，");
    demo::line("   就会卡住【所有】连接。要么异步化，要么丢给线程池。");
    demo::line("");
    demo::line("3) 写缓冲区管理");
    demo::line("   send 可能只发出去一部分（内核发送缓冲区满了）。");
    demo::line("   剩下的必须存进 outbuf，注册 EPOLLOUT，等可写事件再继续发。");
    demo::line("   发完了要记得【取消】EPOLLOUT，否则会一直触发（LT 模式下 CPU 跑满）。");
    demo::line("");
    demo::line("4) 惊群问题");
    demo::line("   多个进程/线程 epoll 同一个监听 fd，来一个连接全被唤醒，只有一个能 accept。");
    demo::line("   解决：EPOLLEXCLUSIVE（Linux 4.5+）或 SO_REUSEPORT（每个进程一个监听 fd）。");
    demo::line("");
    demo::line("5) 定时器");
    demo::line("   事件循环里要处理超时（连接空闲踢下线、重试）。");
    demo::line("   常见做法：小顶堆 / 时间轮，把最近的超时时间作为 epoll_wait 的 timeout。");
    demo::line("   Linux 还可以用 timerfd 把定时器变成一个 fd 一起 epoll。");
}

// -----------------------------------------------------------------------------
int main(int argc, char** argv) {
    try {
        netc::Startup net_guard;
        std::signal(SIGINT, on_signal);

        if (argc > 1 && std::string(argv[1]) == "explain") {
            explain();
            return 0;
        }

        uint16_t    port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8890;
        std::string mode = (argc > 2) ? argv[2] : "select";

        demo::title("IO 多路复用聊天室服务器");
        std::cout << "  端口 " << port << "   模式 " << mode << "\n";
        std::cout << "  测试：开多个终端，各执行  telnet 127.0.0.1 " << port << "\n";
        std::cout << "        然后随便打字，会广播给其他终端。输入 quit 退出。\n";
        std::cout << "  查看原理讲解： ch11_multiplex.exe explain\n";

        if (mode == "select") {
            run_select(port);
        } else if (mode == "poll") {
            run_poll(port);
        } else if (mode == "epoll") {
#ifdef __linux__
            run_epoll(port);
#else
            std::cout << "\n[提示] epoll 是 Linux 独有的，当前平台不支持。\n";
            std::cout << "       在 WSL / Linux 上重新编译即可运行本模式。\n";
            std::cout << "       Windows 的对应技术是 IOCP，模型不同（Proactor）。\n";
            std::cout << "       现在退回 poll 模式演示：\n";
            run_poll(port);
#endif
        } else {
            std::cout << "未知模式，可选: select | poll | epoll | explain\n";
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
