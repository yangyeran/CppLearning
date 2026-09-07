# 第 8 章 · 网络基础：什么是网络

▶ 对应程序：`ch08_net_basics`

---

## 8.1 从最基本的问题开始

**问题**：A 电脑上的一个程序，怎么把一段数据交给 B 电脑上的另一个程序？

需要解决四件事：

| 问题 | 解决方案 |
|------|---------|
| 找到 B 这台机器 | **IP 地址** |
| 找到 B 上的那个程序 | **端口号** |
| 数据怎么在物理线路上跑 | 以太网 / WiFi / 光纤 + 逐层封装 |
| 丢了怎么办、乱序怎么办 | **TCP**（或者不管，那就是 **UDP**） |

**「网络协议」就是通信双方约定好的一套规则**，规定了：
数据长什么样（格式）、什么时候发什么（时序）、出错怎么处理（差错控制）。

---

## 8.2 分层模型 —— 为什么要分层

分层的核心思想：**每一层只管自己的事，把下层当成能用的黑盒。**
这样换网卡不用改浏览器，换 WiFi 不用改 TCP。

```
  OSI 七层（理论模型）          TCP/IP 四层（实际用的）      协议举例
 ─────────────────────────────────────────────────────────────────────
  7  应用层    ┐
  6  表示层    ├──────────>    应用层                  HTTP DNS SSH FTP
  5  会话层    ┘                                       ▲
                                                       │ ← socket API 在这里
  4  传输层    ──────────>    传输层                  TCP  UDP
                                                       ▲   端口号
  3  网络层    ──────────>    网络层                  IP  ICMP  ARP
                                                       ▲   IP 地址
  2  数据链路层 ┐                                      │
  1  物理层     ┴──────────>   网络接口层              以太网  WiFi
                                                           MAC 地址
```

**socket 编程发生在应用层和传输层的交界处**：
你调用 `send()`，内核负责 TCP 分段、加 IP 头、交给网卡驱动。

---

## 8.3 封装与解封装

```
  你要发的数据:                                        [ HTTP 报文 ]

  传输层加 TCP 头(20B+):                      [TCP头][ HTTP 报文 ]
                                               ▲
                                               源端口 目的端口 序号 确认号 标志位 窗口

  网络层加 IP 头(20B+):               [IP头][TCP头][ HTTP 报文 ]
                                       ▲
                                       源IP 目的IP TTL 协议号

  链路层加以太网头尾:      [以太网头][IP头][TCP头][ HTTP 报文 ][FCS]
                            ▲
                            源MAC 目的MAC 类型
```

接收端反过来一层层拆开（**解封装**），每层只看自己那个头。

**关键概念：MTU 与 MSS**

- **MTU**（最大传输单元）：链路层一帧最多能装多少字节。以太网通常是 **1500**。
- 超过 MTU 就要 **IP 分片**。分片很糟：任何一片丢了整个包都要重传，效率骤降。
- **MSS**（最大报文段长度）：TCP 一段最多装多少**应用数据**。
  `MSS = MTU - IP头 - TCP头 = 1500 - 20 - 20 = 1460`（典型值）
- TCP 会在握手时协商 MSS，主动按 MSS 切分，**避免 IP 分片**。
- UDP 不管这些，所以 **UDP 负载建议控制在 1400 字节以内**。

---

## 8.4 IP 地址与端口

### IP 地址

- **IPv4**：32 位，写成 4 段十进制，如 `192.168.1.100`
- **IPv6**：128 位，写成 8 组十六进制，如 `2001:db8::1`

| 地址 | 含义 |
|------|------|
| `127.0.0.1` | 回环地址，永远指向本机，数据**不出网卡** |
| `0.0.0.0` | 服务端 bind 时表示「监听所有网卡」 |
| `192.168.x.x` / `10.x.x.x` / `172.16~31.x.x` | 私有地址，只在局域网有效，需 NAT 才能上外网 |
| `255.255.255.255` | 广播地址 |
| `224.0.0.0 ~ 239.255.255.255` | 组播地址 |

### 端口

16 位无符号数，0~65535。

| 范围 | 名称 | 说明 |
|------|------|------|
| 0~1023 | 知名端口 | 需要 root/管理员权限才能绑定 |
| 1024~49151 | 注册端口 | 应用程序申请注册的 |
| 49152~65535 | 动态/临时端口 | 客户端连接时内核自动分配 |

常见知名端口：`20/21` FTP、`22` SSH、`23` Telnet、`25` SMTP、`53` DNS、
`80` HTTP、`443` HTTPS、`3306` MySQL、`6379` Redis、`27017` MongoDB。

### 【面试】四元组

**一条 TCP 连接由四元组唯一标识：`(源IP, 源端口, 目的IP, 目的端口)`**

所以一个服务端在 80 端口能同时服务几万个客户端 —— 它们的源 IP/源端口不同。
理论上，单个服务端口能承载的连接数 = 客户端 IP 数 × 每个客户端的可用端口数，
远超 65535 这个常见误解。

---

## 8.5 字节序（大端 / 小端）—— 网络编程第一个坑

同样一个 32 位整数 `0x12345678`，在内存里怎么摆？

```
  大端 Big Endian     地址:  低 ────────> 高
                      字节:  12  34  56  78      高位字节放低地址（符合人阅读顺序）

  小端 Little Endian  地址:  低 ────────> 高
                      字节:  78  56  34  12      低位字节放低地址

  x86 / ARM 都是【小端】
  网络字节序统一规定为【大端】（也叫 Network Byte Order）
```

`ch08_net_basics` 用 hexdump 实测能看到这个差异。

### 四个转换函数（必须背下来）

```cpp
htons(x)   // host to network short  —— 16 位，用于【端口】
htonl(x)   // host to network long   —— 32 位，用于【IPv4 地址】
ntohs(x)   // network to host short
ntohl(x)   // network to host long
```

### 什么时候必须转换

| 场景 | 要转吗 |
|------|-------|
| 填 `sockaddr_in` 的 `sin_port` / `sin_addr` | ✅ 必须 |
| 自定义二进制协议里的多字节整数（长度前缀、ID 等） | ✅ 必须（约定用网络序） |
| 单个 `char` / 字符串 / 字节数组 | ❌ 不用（没有字节序问题） |
| 文本协议（HTTP、JSON） | ❌ 不用 |

**【坑】忘记转换的典型症状**：本机测试完全正常（两边都是小端，错得一致），
一旦和别的架构机器或标准协议通信就全乱 —— 非常难查。

C++20 起可以用 `std::endian::native` 判断，C++23 有 `std::byteswap` 做翻转。

---

## 8.6 地址结构 sockaddr —— 一个历史包袱

socket API 是 1983 年 BSD 定下来的，那时候 C 语言还没有 `void*`，
所以设计了一个「通用地址」`struct sockaddr`，各协议族再定自己的结构，
传参时强制转换成 `sockaddr*` —— 这就是你到处看到 `(sockaddr*)` 的原因。

```c
struct sockaddr {                 // 通用，16 字节，只用来做参数类型
    sa_family_t sa_family;        // AF_INET / AF_INET6 / AF_UNIX
    char        sa_data[14];
};

struct sockaddr_in {              // IPv4 专用，实际用这个
    sa_family_t    sin_family;    // AF_INET
    uint16_t       sin_port;      // 端口，【网络字节序】
    struct in_addr sin_addr;      // IPv4 地址，【网络字节序】
    char           sin_zero[8];   // 填充，保证和 sockaddr 一样大
};

struct sockaddr_in6 { ... };      // IPv6，28 字节
struct sockaddr_storage { ... };  // 足够大，能装下任意协议族的地址
```

**构造地址（现代写法）**

```cpp
sockaddr_in a{};
a.sin_family = AF_INET;
a.sin_port   = htons(8080);                        // ★ 端口要转
inet_pton(AF_INET, "192.168.1.100", &a.sin_addr);  // 字符串 -> 二进制
// 或监听所有网卡:
a.sin_addr.s_addr = htonl(INADDR_ANY);

// 二进制 -> 字符串
char buf[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
```

**别用 `inet_addr` / `inet_ntoa`**：不支持 IPv6，且 `inet_ntoa` 用静态缓冲区（线程不安全）。

`ch08_net_basics` 会把 `sockaddr_in` 的原始字节打出来，能直接看到
`02 00`（AF_INET）、`1F 90`（8080 的网络序）、`C0 A8 01 64`（192.168.1.100）。

---

## 8.7 DNS 解析

```cpp
addrinfo hints{};
hints.ai_family   = AF_UNSPEC;      // IPv4 和 IPv6 都要
hints.ai_socktype = SOCK_STREAM;    // TCP

addrinfo* res = nullptr;
if (getaddrinfo("example.com", "443", &hints, &res) == 0) {
    for (addrinfo* p = res; p; p = p->ai_next) {
        // 依次尝试连接每个地址
    }
    freeaddrinfo(res);              // ★ 必须释放
}
```

**要点**

1. 返回的是**链表**，一个域名可能有多个 IP，应该**依次尝试**
2. 结果必须 `freeaddrinfo` 释放
3. 它是**阻塞**调用，慢的 DNS 能卡好几秒 —— 高性能服务要用异步 DNS 库（c-ares）
4. 老 API `gethostbyname` 不支持 IPv6 且线程不安全，别用
5. 它还能把服务名翻译成端口：`getaddrinfo(host, "http", ...)` → 80

---

## 8.8 TCP vs UDP

| | TCP | UDP |
|---|---|---|
| 连接 | 面向连接（先三次握手） | 无连接（直接发） |
| 可靠性 | 保证到达、不重复 | 不保证，可能丢/重复 |
| 顺序 | 保证按发送顺序交付 | 不保证 |
| 流量控制 | 有（滑动窗口） | 无 |
| 拥塞控制 | 有（慢启动/拥塞避免/快重传） | 无 |
| **数据边界** | **无！是字节流** | **有！一个包就是一个消息** |
| 头部大小 | 20 字节起 | 8 字节 |
| 速度 | 慢一些 | 快 |
| 一对多 | 不支持 | 支持广播/组播 |
| 典型场景 | HTTP/HTTPS、SSH、数据库、文件传输、邮件 | DNS、直播、游戏、VoIP、SNMP、QUIC 的底座 |

**选型口诀**

> 「数据不能错、不能少、顺序不能乱」→ **TCP**
> 「宁可丢也不能卡，或者要一对多」→ **UDP**（应用层自己做必要的可靠性）

---

## 8.9 TCP 三次握手

```
   客户端                                             服务端
     │                                                  │  (LISTEN)
     │  ①  SYN, seq = x                                 │
     │ ───────────────────────────────────────────────> │
     │  (SYN_SENT)                                      │  (SYN_RCVD)
     │                                                  │
     │  ②  SYN + ACK, seq = y, ack = x+1                │
     │ <─────────────────────────────────────────────── │
     │                                                  │
     │  ③  ACK, ack = y+1                               │
     │ ───────────────────────────────────────────────> │
     │  (ESTABLISHED)                        (ESTABLISHED)
```

**【面试】为什么是三次不是两次？**

两次握手的话，服务端**无法确认「客户端真的收到了我的响应」**。
考虑这个场景：客户端第一个 SYN 因网络拥堵延迟了很久，客户端超时重发了新 SYN
并完成通信、关闭连接。此时那个**旧的 SYN 才姗姗来迟**到达服务端 ——
两次握手下服务端会直接建立连接并一直占着资源，而客户端根本不认这个连接。

第三次 ACK 让**双方都确认了对方的收发能力都正常**。

**【面试】为什么不是四次？**
第二步的 SYN 和 ACK 可以合并成一个包发，没必要拆开。

---

## 8.10 TCP 四次挥手与 TIME_WAIT

```
   主动关闭方                                        被动关闭方
     │  ①  FIN, seq = u                                 │
     │ ───────────────────────────────────────────────> │
     │  (FIN_WAIT_1)                        (CLOSE_WAIT) │
     │                                                   │
     │  ②  ACK, ack = u+1                                │
     │ <─────────────────────────────────────────────── │
     │  (FIN_WAIT_2)                                     │
     │        ← 此时被动方【还能继续发数据】（半关闭）    │
     │                                                   │
     │  ③  FIN, seq = w                                  │
     │ <─────────────────────────────────────────────── │
     │                                        (LAST_ACK) │
     │  ④  ACK, ack = w+1                                │
     │ ───────────────────────────────────────────────> │
     │  (TIME_WAIT)  等 2MSL                     (CLOSED) │
     │  (CLOSED)                                         │
```

**【面试】为什么挥手要四次？**

因为 TCP 是**全双工**的，两个方向要**分别关闭**。
被动方收到 FIN 后可能还有数据没发完，所以 ACK 和自己的 FIN 要分开发。
（如果没数据要发，有些实现会合并成三次。）

**【面试】TIME_WAIT 为什么要等 2MSL（Linux 上通常 60 秒）？**

MSL = Maximum Segment Lifetime，报文在网络中的最大存活时间。

1. **确保最后那个 ACK 能到达对方**。如果 ACK 丢了，对方会重发 FIN，
   自己还在 TIME_WAIT 就能再 ACK 一次。如果直接 CLOSED，就会回一个 RST，对方报错。
2. **让本连接的所有残留报文在网络中自然消亡**，避免污染下一个使用相同四元组的新连接。

**实际影响与缓解**

服务端如果主动关连接（比如 HTTP 短连接），会积累大量 TIME_WAIT，
占满本地端口，表现为「无法建立新连接」。

| 缓解手段 | 说明 |
|---------|------|
| `SO_REUSEADDR` | 让重启的服务能立刻 bind 处于 TIME_WAIT 的端口（**服务端必设**） |
| 用长连接（keep-alive） | 从根本上减少连接建立/关闭次数 —— **最有效** |
| 让客户端主动关 | TIME_WAIT 落在客户端，客户端数量多，压力分散 |
| `net.ipv4.tcp_tw_reuse=1` | Linux 内核参数，允许复用 TIME_WAIT 连接（客户端侧安全） |

**注意**：`tcp_tw_recycle` 在 NAT 环境下会导致连接失败，Linux 4.12 起已被移除，
网上很多老文章还在推荐它，**别用**。

---

## 8.11 TCP 状态机

```
                        ┌──────────┐
             ┌─────────>│  CLOSED  │<──────────────┐
             │          └────┬─────┘               │
             │       被动打开│  │主动打开(connect)  │
             │       (listen)│  │发 SYN             │
             │               ▼  ▼                  │
        ┌────┴─────┐    ┌────────┐  ┌──────────┐   │
        │ LAST_ACK │    │ LISTEN │  │ SYN_SENT │   │
        └────▲─────┘    └───┬────┘  └────┬─────┘   │
             │              │收SYN       │收SYN+ACK │
        收FIN│发FIN     发SYN+ACK        │发ACK     │
             │              ▼            │          │
        ┌────┴──────┐  ┌──────────┐      │          │
        │CLOSE_WAIT │  │ SYN_RCVD │      │          │
        └────▲──────┘  └────┬─────┘      │          │
             │收FIN         │收ACK       │          │
             │              ▼            │          │
             │        ┌─────────────┐<───┘          │
             └────────┤ ESTABLISHED │               │
                      └──────┬──────┘               │
                        发FIN│                      │
                             ▼                      │
                      ┌─────────────┐               │
                      │ FIN_WAIT_1  │               │
                      └──────┬──────┘               │
                        收ACK│                      │
                             ▼                      │
                      ┌─────────────┐               │
                      │ FIN_WAIT_2  │               │
                      └──────┬──────┘               │
                        收FIN│发ACK                 │
                             ▼                      │
                      ┌─────────────┐   等 2MSL     │
                      │  TIME_WAIT  ├───────────────┘
                      └─────────────┘
```

用 `ss -tanp`（Linux）或 `netstat -ano`（Windows）能实时看到这些状态。

---

## 8.12 socket API 全景

```
       服务端                                  客户端
  ═══════════════════════════════════════════════════════════
    socket()          创建 fd
       │
    setsockopt()      设 SO_REUSEADDR 等
       │
    bind()            绑定 IP:端口
       │
    listen()          转为监听态，设置 backlog
       │                                socket()     创建 fd
    accept()  阻塞等待  <─────────────  connect()    发起连接
       │              （三次握手在这里完成）  │
       │  ↑                                   │
       │  └─ 返回【新的 fd】代表这条连接      │
       │     原来的监听 fd 继续监听           │
       │                                      │
    recv()/send()  <──────────────────>  send()/recv()
       │              （在新 fd 上收发）      │
       │                                      │
    close()        <──── 四次挥手 ────>  close()
```

### 每个调用干了什么

**`socket(AF_INET, SOCK_STREAM, 0)`**
创建一个 socket，返回文件描述符。Linux 上「一切皆文件」，所以 socket fd
可以用 `read`/`write`/`close`，也能放进 `select`/`epoll`。Windows 上不行，只能用 `recv`/`send`。

**`bind(fd, addr, len)`**
把 fd 和一个本地 IP:端口关联。客户端一般不用调（内核自动分配临时端口）。

**`listen(fd, backlog)`**
把 fd 从「主动」变成「被动监听」。

`backlog` 是**已完成握手但还没被 accept 取走**的连接队列长度。
（内核里实际有两个队列：半连接队列 SYN queue 和全连接队列 accept queue，
backlog 影响后者。）队列满了新连接会被丢弃 → 客户端表现为连接超时。

**`accept(fd, &addr, &len)`**
从队列取一个已建立的连接，**返回新 fd**。阻塞模式下没连接就一直等。

**`connect(fd, addr, len)`**
客户端发起三次握手。阻塞模式下要等握手完成或超时（默认可能长达 75 秒以上）。

**`send` / `recv`（或 `write` / `read`）**

```cpp
long n = recv(fd, buf, len, 0);
if (n > 0)  { /* 收到 n 字节 */ }
if (n == 0) { /* ★ 对端【正常关闭】了连接，不是错误！ */ }
if (n < 0)  { /* 出错。但要区分 EINTR（被信号打断，应重试）和
                  EAGAIN/EWOULDBLOCK（非阻塞模式下暂时没数据，不是错误） */ }
```

**★ `send` 也可能只发出去一部分**，必须循环：

```cpp
bool send_all(int fd, const char* p, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        long n = send(fd, p + sent, len - sent, 0);
        if (n > 0) { sent += n; continue; }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}
```

这是新手最常见的 bug 之一。

**`close(fd)`**
引用计数减一，到 0 才真正发 FIN。
`shutdown(fd, SHUT_WR)` 可以**只关写方向**（半关闭），用来告诉对端「我发完了，
但我还能收」—— HTTP/1.0 和某些协议会用到。

---

## 8.13 常用 socket 选项

| 选项 | 作用 | 什么时候设 |
|------|------|-----------|
| `SO_REUSEADDR` | 允许 bind 处于 TIME_WAIT 的端口 | **服务端几乎必设**，否则重启报 "Address already in use" |
| `SO_REUSEPORT` | 多个进程绑同一端口，内核做负载均衡（Linux 3.9+） | 多进程 accept，避免惊群 |
| `TCP_NODELAY` | 关闭 Nagle 算法 | 交互式协议（游戏/RPC/SSH）—— 见下文 |
| `SO_KEEPALIVE` | 开启 TCP 保活探测 | 检测对端「假死」。默认 2 小时太长，一般应用层自己做心跳 |
| `SO_RCVBUF` / `SO_SNDBUF` | 收发缓冲区大小 | 高带宽长延迟链路（跨国）需要调大，否则窗口不够跑不满带宽 |
| `SO_RCVTIMEO` / `SO_SNDTIMEO` | 收发超时 | 阻塞模式下防止永久卡死的简易办法 |
| `SO_LINGER` | 控制 close 时如何处理未发送数据 | 慎用，容易造成数据丢失 |

**Nagle 算法**：把小包攒一攒再发（等到收到上一个包的 ACK，或攒够一个 MSS）。
目的是减少「1 字节数据 + 40 字节头」这种极低效的包。
代价是**增加延迟**。交互式应用（每次就发几十字节，要求立即到达）应该
`TCP_NODELAY` 关掉它。

**【坑】Nagle + 延迟 ACK 的经典组合问题**：发送方等 ACK 才发下一个小包，
接收方的延迟 ACK 又要等 40ms 才回 —— 造成 40ms 的莫名延迟。

---

## 8.14 阻塞与非阻塞，五种 IO 模型

**阻塞模式（默认）**
`recv()` 没数据 → 线程挂起，直到有数据或出错。
优点：代码线性好读。缺点：一个线程只能盯一个连接。

**非阻塞模式**
`recv()` 没数据 → 立刻返回 -1，`errno = EAGAIN / EWOULDBLOCK`。
**这不是错误**，只是「现在没有」，必须专门判断。
单纯轮询非阻塞 socket 会烧 CPU，所以要配合 IO 多路复用。

```cpp
// Linux
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);

// Windows
u_long mode = 1;
ioctlsocket(fd, FIONBIO, &mode);
```

### 五种 IO 模型（《UNIX 网络编程》经典分类）

```
1) 阻塞 IO
   应用 ──recv()──> 内核 [等数据 ......][拷贝数据] ──> 返回
        <──────────── 全程阻塞 ────────────────>

2) 非阻塞 IO
   应用 ──recv()──> EAGAIN   (轮询，烧 CPU)
        ──recv()──> EAGAIN
        ──recv()──> 内核 [拷贝数据] ──> 返回

3) IO 多路复用                                    ← 主流
   应用 ──select/epoll()──> 内核 [等任一 fd 就绪] ──> 返回就绪列表
        ──recv()──────────> 内核 [拷贝数据] ──> 返回
   一个线程能盯住成千上万个 fd

4) 信号驱动 IO
   内核数据就绪时发 SIGIO 信号通知。实际很少用。

5) 异步 IO (AIO)
   应用 ──aio_read()──> 立刻返回，继续干别的
        <──通知────── 内核 [等数据][拷贝数据] 全干完了才通知
   Linux io_uring / Windows IOCP
```

**【面试】前 4 种都是「同步 IO」** —— 数据从内核缓冲区拷贝到用户缓冲区
这一步是**你自己在等**。只有第 5 种是内核帮你拷完再通知你。

---

## 8.15 在 Windows 上学 Linux 网络编程

本教程的网络代码按 POSIX 语义写，通过 `src/common/net_compat.h` 兼容 Windows。

| 项目 | Linux (POSIX) | Windows (Winsock2) |
|------|--------------|-------------------|
| 头文件 | `<sys/socket.h>` 等 | `<winsock2.h>` `<ws2tcpip.h>` |
| 初始化 | 不需要 | `WSAStartup` / `WSACleanup` |
| 句柄类型 | `int`（就是 fd） | `SOCKET`（`UINT_PTR`） |
| 无效值 | `-1` | `INVALID_SOCKET` |
| 关闭 | `close()` | `closesocket()` |
| 错误码 | `errno` | `WSAGetLastError()` |
| 非阻塞 | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` |
| 多路复用 | `select`/`poll`/**`epoll`** | `select`/`WSAPoll`/**`IOCP`** |
| 一切皆文件 | ✅ 可用 `read`/`write` | ❌ 只能 `recv`/`send` |

**装 WSL2 跑真正的 Linux**（学 epoll 必须）：

```powershell
wsl --install          # 管理员 PowerShell，然后重启
```

```bash
sudo apt update && sudo apt install -y build-essential gdb cmake
cd /mnt/c/Users/coder/Desktop/Claude_Code/CppLearning
cmake -B build-linux && cmake --build build-linux -j
./build-linux/bin/ch11_multiplex 8890 epoll
```

---

## 8.16 排障工具速查

**连通性**

```bash
ping <host>                  # ICMP，可能被防火墙挡，不通不代表服务不可用
telnet <host> <port>         # 测某个 TCP 端口通不通 —— 最简单有效
nc -zv <host> <port>         # 同上，Linux 常用
curl -v http://host:port/    # 看完整 HTTP 交互
traceroute <host>            # 看路径上每一跳（Windows 是 tracert）
```

**看本机连接**

```bash
# Linux
ss -tlnp                     # 所有监听端口 + 进程（推荐，比 netstat 快）
ss -tanp                     # 所有 TCP 连接及状态
ss -s                        # 汇总统计（各状态连接数）
lsof -i :8080                # 谁占了 8080
netstat -tlnp                # 老版本

# Windows
netstat -ano | findstr :8080
Get-NetTCPConnection -LocalPort 8080
```

**抓包**

```bash
tcpdump -i any -nn port 8080 -w capture.pcap     # 抓包存文件
tcpdump -i any -nn -A port 8080                  # 直接看 ASCII 内容
wireshark capture.pcap                           # 图形化分析
```

Wireshark 里看三次握手、重传、RST、窗口变化最直观，
过滤器写 `tcp.port == 8080` 或 `tcp.flags.syn == 1`。

**看程序行为**

```bash
strace -e trace=network ./prog       # 看程序调了哪些网络系统调用
ltrace ./prog                        # 看库函数调用
```

### 常见现象与原因对照

| 现象 | 原因 |
|------|------|
| `Connection refused` | 目标端口**没人监听**（内核直接回 RST） |
| `Connection timed out` | 包被**丢弃**，没有任何响应（防火墙 DROP / 路由不通 / 对方过载） |
| `Address already in use` | 端口被占，或处于 TIME_WAIT 且没设 `SO_REUSEADDR` |
| `Broken pipe` / `EPIPE` | 往一个对端已关闭的连接上写（默认还会收到 SIGPIPE 信号，要忽略它） |
| `Connection reset by peer` | 对端发了 **RST**：进程崩了 / 强制关闭 / 收到非法包 / backlog 满 |
| `Too many open files` | fd 耗尽。`ulimit -n` 调大，并检查是否有 fd 泄漏 |
| 连接偶发很慢（40ms 左右） | Nagle + 延迟 ACK 的经典组合，设 `TCP_NODELAY` |
| 大量 `TIME_WAIT` | 服务端主动关连接。改长连接，或调内核参数 |
| 大量 `CLOSE_WAIT` | **你的程序收到 FIN 后没有 close()** —— 这是代码 bug，赶紧查 |

**`CLOSE_WAIT` 堆积几乎总是应用层 bug**，值得单独记住：
对端关了，你的 `recv` 返回 0，但你没调 `close()`。

---

<div style="page-break-after: always;"></div>
