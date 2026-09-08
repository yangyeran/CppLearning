// =============================================================================
// net_compat.h —— 跨平台 Socket 兼容层
//
// 目的：本教程的网络章节按 **Linux/POSIX 语义** 编写（这是行业标准写法），
//       同时通过本头文件让同一份代码在 Windows 上也能编译运行。
//
// Windows 与 Linux 的 socket API 差异（面试常问）：
//   ┌──────────────┬────────────────────┬──────────────────────┐
//   │ 项目         │ Linux (POSIX)      │ Windows (Winsock2)   │
//   ├──────────────┼────────────────────┼──────────────────────┤
//   │ 头文件       │ <sys/socket.h> 等  │ <winsock2.h>         │
//   │ 初始化       │ 不需要             │ WSAStartup / Cleanup │
//   │ 句柄类型     │ int (就是 fd)      │ SOCKET (UINT_PTR)    │
//   │ 无效值       │ -1                 │ INVALID_SOCKET       │
//   │ 关闭         │ close()            │ closesocket()        │
//   │ 错误码       │ errno              │ WSAGetLastError()    │
//   │ 非阻塞       │ fcntl(O_NONBLOCK)  │ ioctlsocket(FIONBIO) │
//   │ 多路复用     │ select/poll/epoll  │ select/WSAPoll/IOCP  │
//   │ 一切皆文件   │ 是，可用 read/write│ 否，只能 recv/send   │
//   └──────────────┴────────────────────┴──────────────────────┘
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <system_error>
#include <string_view>

// -----------------------------------------------------------------------------
// 平台头文件
// -----------------------------------------------------------------------------
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>      // socket / bind / listen / accept / select ...
#  include <ws2tcpip.h>      // getaddrinfo / inet_pton / inet_ntop
#  include <windows.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/types.h>
#  include <sys/socket.h>    // socket / bind / listen / accept / setsockopt
#  include <netinet/in.h>    // sockaddr_in / htons / IPPROTO_TCP
#  include <netinet/tcp.h>   // TCP_NODELAY
#  include <arpa/inet.h>     // inet_pton / inet_ntop
#  include <netdb.h>         // getaddrinfo
#  include <unistd.h>        // close / read / write
#  include <fcntl.h>         // fcntl / O_NONBLOCK
#  include <poll.h>          // poll
#  include <errno.h>
#  include <sys/select.h>
#  ifdef __linux__
#    include <sys/epoll.h>   // epoll（Linux 独有）
#  endif
#endif

namespace netc {

// -----------------------------------------------------------------------------
// 1) 统一的 socket 句柄类型
// -----------------------------------------------------------------------------
#ifdef _WIN32
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline constexpr int      kSocketError   = SOCKET_ERROR;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
inline constexpr int      kSocketError   = -1;
#endif

// -----------------------------------------------------------------------------
// 2) 库初始化 —— RAII 封装
//    Windows 必须 WSAStartup；Linux 什么都不做。
//    在 main() 开头放一个 netc::Startup guard; 即可。
// -----------------------------------------------------------------------------
class Startup {
public:
    Startup() {
#ifdef _WIN32
        WSADATA wsa{};
        int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            throw std::runtime_error("WSAStartup 失败, code=" + std::to_string(rc));
        }
#endif
    }
    ~Startup() {
#ifdef _WIN32
        ::WSACleanup();
#endif
    }
    Startup(const Startup&)            = delete;
    Startup& operator=(const Startup&) = delete;
};

// -----------------------------------------------------------------------------
// 3) 错误处理
// -----------------------------------------------------------------------------
inline int last_error() {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline std::string error_string(int err) {
#ifdef _WIN32
    char* buf = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                         FORMAT_MESSAGE_IGNORE_INSERTS,
                     nullptr, static_cast<DWORD>(err),
                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                     reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string s = buf ? buf : "unknown";
    if (buf) ::LocalFree(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
#else
    return std::strerror(err);
#endif
}

inline std::string last_error_string() { return error_string(last_error()); }

// 出错就抛异常（教学代码里方便，生产代码通常用返回码）
[[noreturn]] inline void throw_socket_error(const char* what) {
    int e = last_error();
    throw std::runtime_error(std::string(what) + " 失败: [" + std::to_string(e) +
                             "] " + error_string(e));
}

// "本次操作会阻塞" —— 非阻塞 IO 里最常见、且不是真错误的错误码
inline bool would_block() {
#ifdef _WIN32
    int e = last_error();
    return e == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

// 被信号打断，应重试
inline bool interrupted() {
#ifdef _WIN32
    return ::WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

// -----------------------------------------------------------------------------
// 4) 关闭 socket
// -----------------------------------------------------------------------------
inline int close_socket(socket_t s) {
#ifdef _WIN32
    return ::closesocket(s);
#else
    return ::close(s);
#endif
}

// -----------------------------------------------------------------------------
// 5) 设置非阻塞
// -----------------------------------------------------------------------------
inline bool set_nonblocking(socket_t s, bool on = true) {
#ifdef _WIN32
    u_long mode = on ? 1u : 0u;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, flags) == 0;
#endif
}

// -----------------------------------------------------------------------------
// 6) 常用 socket 选项
// -----------------------------------------------------------------------------

// SO_REUSEADDR：允许服务端重启时立刻重新 bind 处于 TIME_WAIT 的端口。
// 服务端几乎必设，否则重启会报 "Address already in use"。
inline bool set_reuse_addr(socket_t s, bool on = true) {
#ifdef _WIN32
    BOOL v = on ? TRUE : FALSE;
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
#else
    int v = on ? 1 : 0;
    return ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v)) == 0;
#endif
}

// TCP_NODELAY：关闭 Nagle 算法。
// Nagle 会把小包攒一攒再发（省带宽），但会增加延迟。
// 交互式协议（游戏、RPC、SSH）通常关掉它。
inline bool set_tcp_nodelay(socket_t s, bool on = true) {
#ifdef _WIN32
    BOOL v = on ? TRUE : FALSE;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&v), sizeof(v)) == 0;
#else
    int v = on ? 1 : 0;
    return ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v)) == 0;
#endif
}

// -----------------------------------------------------------------------------
// 7) 收发（统一签名，屏蔽 Windows 的 char* 与 int 长度）
// -----------------------------------------------------------------------------
inline long send_n(socket_t s, const void* buf, size_t len, int flags = 0) {
#ifdef _WIN32
    return ::send(s, static_cast<const char*>(buf), static_cast<int>(len), flags);
#else
    return ::send(s, buf, len, flags);
#endif
}

inline long recv_n(socket_t s, void* buf, size_t len, int flags = 0) {
#ifdef _WIN32
    return ::recv(s, static_cast<char*>(buf), static_cast<int>(len), flags);
#else
    return ::recv(s, buf, len, flags);
#endif
}

// send 只保证「发出去一部分」，必须循环直到全部发完 —— 新手最常见的坑。
inline bool send_all(socket_t s, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        long n = send_n(s, p + sent, len - sent);
        if (n > 0) { sent += static_cast<size_t>(n); continue; }
        if (n < 0 && interrupted()) continue;          // 被信号打断，重试
        return false;                                   // 真错误或对端关闭
    }
    return true;
}

inline bool send_all(socket_t s, std::string_view sv) {
    return send_all(s, sv.data(), sv.size());
}

// -----------------------------------------------------------------------------
// 8) 地址工具
// -----------------------------------------------------------------------------

// 构造 IPv4 地址结构。ip 传 nullptr / "0.0.0.0" 表示监听所有网卡。
inline sockaddr_in make_addr_v4(const char* ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = ::htons(port);           // 主机序 -> 网络序（大端）
    if (ip == nullptr || std::strlen(ip) == 0) {
        a.sin_addr.s_addr = ::htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        throw std::invalid_argument(std::string("非法 IPv4 地址: ") + ip);
    }
    return a;
}

// sockaddr -> "1.2.3.4:5678"
inline std::string addr_to_string(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(::ntohs(a.sin_port));
}

// -----------------------------------------------------------------------------
// 9) RAII socket 句柄（本教程后续统一用它，避免忘记 close）
// -----------------------------------------------------------------------------
class Socket {
public:
    Socket() = default;
    explicit Socket(socket_t s) : fd_(s) {}

    // 创建：type = SOCK_STREAM(TCP) / SOCK_DGRAM(UDP)
    static Socket create(int type, int protocol = 0, int family = AF_INET) {
        socket_t s = ::socket(family, type, protocol);
        if (s == kInvalidSocket) throw_socket_error("socket()");
        return Socket{s};
    }
    static Socket tcp() { return create(SOCK_STREAM, IPPROTO_TCP); }
    static Socket udp() { return create(SOCK_DGRAM,  IPPROTO_UDP); }

    ~Socket() { reset(); }

    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = kInvalidSocket; }
    Socket& operator=(Socket&& o) noexcept {
        if (this != &o) { reset(); fd_ = o.fd_; o.fd_ = kInvalidSocket; }
        return *this;
    }
    Socket(const Socket&)            = delete;
    Socket& operator=(const Socket&) = delete;

    socket_t get()   const noexcept { return fd_; }
    bool     valid() const noexcept { return fd_ != kInvalidSocket; }
    explicit operator bool() const noexcept { return valid(); }

    socket_t release() noexcept { socket_t t = fd_; fd_ = kInvalidSocket; return t; }
    void reset(socket_t s = kInvalidSocket) noexcept {
        if (fd_ != kInvalidSocket) close_socket(fd_);
        fd_ = s;
    }

private:
    socket_t fd_ = kInvalidSocket;
};

} // namespace netc
