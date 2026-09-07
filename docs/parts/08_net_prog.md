# 第 9 章 · TCP 编程实战

▶ 对应程序：`ch09_tcp_server` + `ch09_tcp_client`

```bash
# 终端 1
build\bin\ch09_tcp_server.exe 8888 thread

# 终端 2
build\bin\ch09_tcp_client.exe 127.0.0.1 8888          # 交互模式
build\bin\ch09_tcp_client.exe 127.0.0.1 8888 bench    # 粘包演示 + 压测
```

---

## 9.1 服务端的固定套路

```cpp
// ① 创建
socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

// ② 设选项（bind 之前）
int on = 1;
setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

// ③ 绑定
sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_port   = htons(port);
addr.sin_addr.s_addr = htonl(INADDR_ANY);
bind(s, (sockaddr*)&addr, sizeof(addr));

// ④ 监听
listen(s, 128);

// ⑤ 循环 accept
for (;;) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    socket_t c = accept(s, (sockaddr*)&peer, &len);
    handle(c);      // ★ c 是【新的】fd，s 继续监听
}
```

**这五步的顺序不能变**，`setsockopt(SO_REUSEADDR)` 必须在 `bind` 之前。

---

## 9.2 三种并发模型

`ch09_tcp_server` 用第二个参数切换，可以实际对比。

### 模型一：`iterative` 单线程串行

```cpp
while (true) {
    auto conn = accept(listener);
    handle_connection(conn);      // 处理完才回来 accept 下一个
}
```

- ✅ 最简单，没有任何并发问题
- ❌ 一次只能服务一个客户端，第二个要一直等
- **【用在哪】** 只有内部工具、调试用途

### 模型二：`thread` 每连接一个线程

```cpp
while (true) {
    auto conn = accept(listener);
    std::thread([c = std::move(conn)] { handle_connection(c); }).detach();
}
```

- ✅ 写起来简单，每个连接一个线性流程，容易理解和调试
- ❌ 线程栈默认 1~8 MB → **1 万连接要 10~80 GB 内存**
- ❌ 线程切换开销随线程数暴涨
- ❌ 恶意客户端狂建连接就能打垮你

**这就是著名的 C10K 问题**（如何让单机处理 1 万并发连接）。

**【用在哪】** 连接数少（几十到几百）、每个连接计算量大的场景。
比如内部 RPC 服务、管理后台。

### 模型三：`pool` 线程池

```cpp
ThreadPool pool(std::thread::hardware_concurrency());
while (true) {
    auto conn = accept(listener);
    pool.submit([c = std::move(conn)] { handle_connection(c); });
}
```

- ✅ 线程数固定（通常 = CPU 核数或其小倍数），不会无限膨胀
- ❌ **如果某个连接长时间不发数据，会白占一个工作线程**
- 100 个空闲长连接就能把 8 线程的池占满

**【用在哪】** 短连接为主、请求处理快的场景。

### 彻底的解法

**IO 多路复用 + 非阻塞**（第 11 章）。
让一个线程管上万个连接，只有真正有数据的连接才占用 CPU。

生产架构通常是**多 Reactor + 线程池**：
N 个事件循环线程负责 IO，业务计算丢给独立线程池。

---

## 9.3 处理一条连接：三个必须记住的细节

```cpp
void handle_connection(socket_t conn) {
    char buf[4096];
    for (;;) {
        long n = recv(conn, buf, sizeof(buf), 0);

        if (n == 0) {
            // ★① 对端【正常关闭】（发来了 FIN），不是错误
            break;
        }
        if (n < 0) {
            // ★② 出错，但要区分：
            if (errno == EINTR) continue;                  // 被信号打断，重试
            if (errno == EAGAIN) break;                    // 非阻塞下暂时没数据
            break;                                          // 真错误
        }

        // ★③ send 可能只发出去一部分，必须循环
        if (!send_all(conn, buf, n)) break;
    }
    close(conn);
}
```

---

## 9.4 【重点】TCP 粘包与拆包

**这是 TCP 编程最核心的概念。**

`ch09_tcp_client bench` 会实测：客户端连续三次 `send("AAA")` `send("BBB")` `send("CCC")`，
服务端**一次 `recv` 收到 `"AAABBBCCC"` 9 个字节**。

```
  发送端                          网络                    接收端
  send("AAA")  ┐
  send("BBB")  ├─> 内核缓冲区 ─> [AAABBBCCC] ─> recv() 一次全收到  ← 粘包
  send("CCC")  ┘

  或者

  send("很长的一条消息...")  ─> 超过 MSS，被切成两段 ─> recv() 只收到前半截  ← 拆包
```

**为什么会这样**：TCP 是**字节流**协议。它只保证：
- 字节**按序**到达
- 不丢、不重

它**不保证**你的一次 `send` 对应对方的一次 `recv`。
内核会根据 Nagle 算法、缓冲区状态、MSS、网络拥塞情况自行决定怎么打包。

**注意：「粘包」这个词其实是中文社区的俗称，容易让人以为是 bug。
实际上这是 TCP 的设计本意 —— 它就是个字节流管道，没有「包」这个概念。**

### 三种解决方案

**方案 1：固定长度**

```
[==== 64 字节 ====][==== 64 字节 ====]
```
简单，但浪费空间，且长度一旦定死很难改。适合定长的传感器数据。

**方案 2：长度前缀（TLV）—— 最常用**

```
[4字节长度][消息体][4字节长度][消息体]
```

```cpp
// 发送
uint32_t len = htonl(static_cast<uint32_t>(body.size()));   // ★ 转网络字节序
send_all(fd, &len, 4);
send_all(fd, body.data(), body.size());

// 接收
uint32_t netlen;
recv_exactly(fd, &netlen, 4);              // ① 先收满 4 字节
uint32_t len = ntohl(netlen);
if (len > kMaxMessageSize) {               // ★★ 必须校验上限！
    close(fd);                             // 否则恶意客户端发 len=0xFFFFFFFF
    return;                                // 你一 resize 内存就爆了 —— DoS 漏洞
}
std::string body(len, '\0');
recv_exactly(fd, body.data(), len);        // ② 再按长度收满
```

**`recv_exactly` 的实现**

```cpp
bool recv_exactly(socket_t s, void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < n) {
        long r = recv(s, p + got, n - got, 0);
        if (r > 0) { got += r; continue; }
        if (r < 0 && errno == EINTR) continue;
        return false;                       // 对端关闭或出错
    }
    return true;
}
```

**方案 3：分隔符**

```
消息1\n消息2\n消息3\n
```
HTTP 头部用 `\r\n` 分隔行、`\r\n\r\n` 表示头部结束，就是这个方案。
缺点：内容里出现分隔符要转义。

**混合方案**：HTTP 实际上是「分隔符找头部 + 长度前缀（Content-Length）读正文」，
兼顾了可读性和二进制安全 —— 第 12 章会实现。

---

## 9.5 客户端要点

```cpp
// 现代做法：getaddrinfo 解析 + 依次尝试
addrinfo hints{};
hints.ai_family   = AF_UNSPEC;     // IPv4/IPv6 都接受
hints.ai_socktype = SOCK_STREAM;

addrinfo* res;
getaddrinfo(host, port, &hints, &res);
for (addrinfo* p = res; p; p = p->ai_next) {
    int s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (s < 0) continue;
    if (connect(s, p->ai_addr, p->ai_addrlen) == 0) break;   // 连上就用它
    close(s);                                                 // 连不上试下一个
}
freeaddrinfo(res);
```

**客户端不用 `bind`** —— 内核在 `connect` 时自动分配一个临时端口（49152~65535）。
`ch09_tcp_client` 会用 `getsockname` 把这个端口打出来。

**连接超时怎么做**：阻塞 `connect` 的默认超时很长（可能 75 秒以上）。
正确做法是设成非阻塞，`connect` 立刻返回 `EINPROGRESS`，
然后用 `select`/`poll` 等待可写，带自己的超时时间。

---

## 9.6 实测数据（回环地址）

`ch09_tcp_client bench` 在本机跑 1000 次请求-响应往返：

```
  完成 1000 次请求-响应往返
  总耗时 33 ms
  平均往返延迟 (RTT) 33 us
  QPS 约 30000
```

**参考量级**

| 链路 | 典型 RTT |
|------|---------|
| 回环 127.0.0.1 | 20~50 μs |
| 同机房 | 0.1~1 ms |
| 同城 | 1~5 ms |
| 跨城（如北京-上海） | 10~30 ms |
| 跨国（如中美） | 150~300 ms |

**这就是为什么「减少往返次数」比「压缩数据量」更能提升体感。**
一个需要 5 次串行往返的协议，跨国场景下光是等待就要 1.5 秒。
HTTP/2 的多路复用、HTTP/3 的 0-RTT 握手，本质上都在解决这个问题。

---

<div style="page-break-after: always;"></div>

# 第 10 章 · UDP 编程

▶ 对应程序：`ch10_udp_server` + `ch10_udp_client`

```bash
build\bin\ch10_udp_server.exe 9999
build\bin\ch10_udp_client.exe 127.0.0.1 9999 demo
```

## 10.1 和 TCP 的编程差异

```cpp
// TCP                              // UDP
socket(AF_INET, SOCK_STREAM, 0);    socket(AF_INET, SOCK_DGRAM, 0);
bind(...);                          bind(...);          // 服务端仍要 bind
listen(...);                        // ✗ 没有
accept(...);                        // ✗ 没有
connect(...);                       // 可选（见下）
recv(fd, buf, len, 0);              recvfrom(fd, buf, len, 0, &peer, &plen);
send(fd, buf, len, 0);              sendto(fd, buf, len, 0, &peer, plen);
```

**核心差异**：UDP 没有「连接」的概念，所以每次收发都要**带对端地址**。

## 10.2 UDP 有消息边界

`ch10_udp_client demo` 实测：三次 `sendto("AAA")("BBB")("CCC")`，
服务端**三次 `recvfrom` 分别收到 AAA、BBB、CCC**。

```
  TCP:  send("AAA") send("BBB") send("CCC")  ->  recv() 得到 "AAABBBCCC"   ← 粘包
  UDP:  sendto("AAA") sendto("BBB")          ->  recvfrom() 得到 "AAA"     ← 一一对应
                                                 recvfrom() 得到 "BBB"
```

**这是 UDP 相对 TCP 最重要的语义差异。** 不用自己处理消息边界。

**【坑】** 如果 `recvfrom` 的缓冲区比数据报小，**多出来的部分会被直接丢弃**
（不像 TCP 会留在缓冲区等下次读）。所以缓冲区要开够（65536 保险）。

## 10.3 UDP 必须自己处理的事

**① 超时** —— 没有连接，对端不回你就会永远卡在 `recvfrom` 上。

```cpp
// Linux
timeval tv{1, 0};                     // 1 秒
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

// Windows
DWORD ms = 1000;
setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char*)&ms, sizeof(ms));
```

**② 丢包 / 乱序 / 重复** —— 需要可靠性就得自己实现：
序号 + ACK + 超时重传 + 去重 + 排序 + 流控。

**这基本就是在重新发明 TCP。** 除非你有特殊需求，否则用 TCP。

真的需要「可靠 UDP」时，用成熟方案：

| 方案 | 场景 |
|------|------|
| **KCP** | 游戏，牺牲 10~20% 带宽换 30~40% 延迟降低 |
| **QUIC / HTTP3** | Web，0-RTT 握手 + 避免队头阻塞 + 连接迁移 |
| **RTP / RTCP** | 音视频，允许丢帧但要求低延迟 |

**③ 数据报大小** —— 理论上限 65507 字节，但超过 MTU(1500) 就要 IP 分片，
分片中任何一片丢了整个数据报都废掉（丢包率被放大）。
**经验值：单个 UDP 负载控制在 1400 字节以内。**

## 10.4 UDP 也可以 `connect()`

```cpp
connect(udp_fd, &server_addr, sizeof(server_addr));
send(udp_fd, data, len, 0);           // 之后可以直接用 send/recv
```

**它不发任何包**，只是在内核里记住默认对端。好处：

1. 之后可以用 `send`/`recv`，不用每次带地址
2. **能收到 ICMP 错误**（比如 Port Unreachable）—— 不 connect 的话这些错误被丢弃
3. 内核少做一次地址查找，性能略好
4. 内核会过滤掉其它地址发来的包

## 10.5 UDP 的独门能力：广播与组播

```cpp
// 广播（只在本网段）
int on = 1;
setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
sendto(fd, data, len, 0, &broadcast_addr, sizeof(...));   // 目标 255.255.255.255

// 组播（一对多推送）
ip_mreq mreq{};
inet_pton(AF_INET, "239.1.1.1", &mreq.imr_multiaddr);
mreq.imr_interface.s_addr = htonl(INADDR_ANY);
setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
```

**TCP 完全不支持这两个** —— 这是 UDP 无法被替代的场景
（服务发现、局域网设备探测、行情推送、IPTV）。

---

<div style="page-break-after: always;"></div>

# 第 11 章 · IO 多路复用

▶ 对应程序：`ch11_multiplex`

```bash
build\bin\ch11_multiplex.exe explain          # 只看原理讲解
build\bin\ch11_multiplex.exe 8890 select      # 聊天室服务器（所有平台）
build\bin\ch11_multiplex.exe 8890 poll
./build-linux/bin/ch11_multiplex 8890 epoll   # 仅 Linux

# 测试：开 2~3 个终端各跑 telnet 127.0.0.1 8890，随便打字会广播给其他人
```

## 11.1 为什么需要它

「一连接一线程」的问题（C10K）：

- 线程栈默认 1~8 MB → 1 万连接就是 10~80 GB
- 线程切换要保存/恢复寄存器、可能刷 TLB → 开销随线程数暴涨
- **大部分线程其实在「等数据」，什么活都没干**

**多路复用的思路**：一个线程问内核「这 10000 个 fd 里，现在哪些有数据了？」
内核回答后，线程只处理那几个就绪的。**一个线程顶一万个线程用。**

## 11.2 三者对比

| | `select` | `poll` | `epoll` |
|---|---|---|---|
| fd 上限 | `FD_SETSIZE`（通常 1024） | 无 | 无 |
| 数据结构 | 位图 `fd_set` | `pollfd` 数组 | **内核红黑树 + 就绪链表** |
| 每次调用开销 | 全量拷贝进内核 | 全量拷贝进内核 | **只在 `epoll_ctl` 时改一次** |
| 找就绪 fd | 遍历全部 O(n) | 遍历全部 O(n) | **直接返回就绪表 O(1)** |
| 集合会被改写 | ✅ 每次要重设 | ❌ events/revents 分开 | ❌ |
| 触发模式 | 只有 LT | 只有 LT | **LT / ET 都支持** |
| 可移植性 | 全平台 | POSIX + WSAPoll | **仅 Linux** |
| 适合 | 连接少 | 连接中等 | **海量连接** |

**其它平台的对应物**

| 平台 | 技术 | 模型 |
|------|------|------|
| BSD / macOS | `kqueue` | Reactor（和 epoll 类似） |
| Windows | **IOCP** | **Proactor**（完成通知，不是就绪通知） |
| Linux 新方案 | **io_uring** | Proactor，减少系统调用，比 epoll 更进一步 |
| 跨平台封装 | libevent / libuv / Boost.Asio / muduo | 生产项目一般直接用这些 |

## 11.3 三种写法

### select

```cpp
fd_set readfds;
FD_ZERO(&readfds);
FD_SET(listener, &readfds);
int maxfd = listener;
for (auto fd : conns) { FD_SET(fd, &readfds); maxfd = std::max(maxfd, fd); }

timeval tv{1, 0};
int ready = select(maxfd + 1, &readfds, nullptr, nullptr, &tv);
//                 ^^^^^^^^^ Linux 要 maxfd+1，Windows 忽略这个参数

if (FD_ISSET(listener, &readfds)) { /* 新连接 */ }
for (auto fd : conns) {
    if (FD_ISSET(fd, &readfds)) { /* 可读 */ }     // ← O(n) 遍历
}
// ★ fd_set 被内核改写了，下一轮必须全部重新 FD_SET
```

### poll

```cpp
std::vector<pollfd> fds;
fds.push_back({listener, POLLIN, 0});
for (auto fd : conns) fds.push_back({fd, POLLIN, 0});

int ready = poll(fds.data(), fds.size(), 1000);

for (auto& pfd : fds) {
    if (pfd.revents & POLLIN)  { /* 可读 */ }
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) { /* 断开或出错 */ }
}
// events(关心什么) 和 revents(发生了什么) 分开，不用重设
// 但每次还是要把整个数组拷进内核，还是 O(n) 遍历
```

### epoll（Linux）

```cpp
int ep = epoll_create1(0);                              // ① 内核里建事件表

epoll_event ev{};
ev.events  = EPOLLIN;
ev.data.fd = listener;
epoll_ctl(ep, EPOLL_CTL_ADD, listener, &ev);            // ② 只在变化时调用

std::vector<epoll_event> events(1024);
for (;;) {
    int n = epoll_wait(ep, events.data(), events.size(), 1000);   // ③ 取就绪列表
    for (int i = 0; i < n; ++i) {                       // ★ 只遍历【就绪的】
        int fd = events[i].data.fd;
        uint32_t e = events[i].events;
        if (e & EPOLLIN)  { /* 可读 */ }
        if (e & EPOLLOUT) { /* 可写 */ }
        if (e & (EPOLLHUP | EPOLLERR)) { /* 断开 */ }
    }
}
```

**为什么 epoll 快**

```
  select/poll:                        epoll:

  用户态 [10000 个 fd]                用户态 (什么都不传)
      │ 每次全量拷贝                       │
      ▼                                    ▼
  内核态 [遍历 10000 个]              内核态 红黑树(常驻) ──> 就绪链表
      │ 找出就绪的                         │  fd 就绪时由回调
      ▼                                    │  直接挂到就绪链表
  返回，用户再遍历 10000 个找就绪的    返回就绪链表里那 5 个
```

O(n) → O(就绪数)。1 万连接里只有 5 个活跃时，差距是 2000 倍。

## 11.4 【面试】水平触发 LT vs 边缘触发 ET

**水平触发（Level Triggered，默认）**

> 「只要缓冲区里**还有**数据，每次 `epoll_wait` 都会通知你」

- ✅ 你这次没读完，下次还会提醒 → 不容易写错
- ❌ 没读完会反复触发，理论上效率略低

**边缘触发（Edge Triggered，`EPOLLET`）**

> 「只在**状态发生变化**时通知一次」（从「没数据」变成「有数据」）

- ✅ 通知次数少，效率高
- ❌ **必须一次把数据读干净**，否则剩下的数据**永远不会再触发通知** → 连接假死

**用 ET 的三条铁律**

1. fd **必须**是非阻塞的（否则最后一次 `read` 会永久阻塞整个事件循环）
2. **必须循环读到 `EAGAIN` / `EWOULDBLOCK`**
3. 写也一样：写到 `EAGAIN`，然后注册 `EPOLLOUT` 等下次可写

```cpp
// ET 模式下的正确读法
for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0)  { process(buf, n); continue; }        // 继续读
    if (n == 0) { close_conn(); break; }              // 对端关闭
    if (errno == EINTR) continue;
    if (errno == EAGAIN) break;                       // ★ 读干净了，正常退出
    close_conn(); break;                              // 真错误
}
```

**经验：先用 LT 把功能写对，确实成为瓶颈了再考虑 ET。**
Nginx 用 ET，Redis 用 LT —— 两个都是顶级项目。

## 11.5 Reactor 模式

```
        ┌─────────────────────────────────────────────────┐
        │                事件循环 Event Loop              │
        │                                                 │
        │    epoll_wait()  ←── 等待事件                   │
        │         │                                       │
        │         ▼                                       │
        │    Demultiplexer 分发                           │
        │      ╱      │       ╲                           │
        │     ▼       ▼        ▼                          │
        │  Accept   Read     Write                        │
        │  Handler  Handler  Handler                      │
        │     │       │        │                          │
        │     ▼       ▼        ▼                          │
        │  新连接  解析+业务  发送剩余数据                 │
        └─────────────────────────────────────────────────┘
```

### 三种变体

| 模型 | 结构 | 代表 |
|------|------|------|
| **单 Reactor 单线程** | 一个线程做全部事情 | **Redis** |
| **单 Reactor 多线程** | IO 在主线程，业务丢线程池 | 业务较重的服务 |
| **主从 Reactor 多线程** | 主 Reactor 只 accept，分给多个子 Reactor 各跑事件循环 | **Nginx / Netty / muduo** |

```
  主从 Reactor:

     mainReactor (只 accept)
          │  分发新连接
     ┌────┼────┬────┐
     ▼    ▼    ▼    ▼
   sub1  sub2 sub3 sub4      每个子 Reactor 一个线程 + 一个 epoll
    │     │    │    │        各自管一批连接的读写
    └─────┴────┴────┘
            │
       业务线程池（可选，处理耗时计算）
```

**Proactor vs Reactor**

- **Reactor**：「**可以读了**，你自己去读」（就绪通知）
- **Proactor**：「我**已经帮你读好了**，数据在这」（完成通知）

Windows IOCP 和 Linux io_uring 属于 Proactor。

## 11.6 事件驱动编程的实际难点

**① 代码从线性变状态机**

```cpp
// 阻塞式：线性，好读
auto req  = read_request(fd);
auto resp = handle(req);
write_response(fd, resp);

// 事件式：要把「读了一半」「处理中」「没发完」都存成显式状态
struct Conn {
    std::string inbuf;      // 收到但还没解析完的
    std::string outbuf;     // 想发但内核缓冲区满了的
    State state;
};
```

**缓解手段**：协程（C++20 coroutine / Go goroutine）—— 把线性写法还给你，
底层还是事件驱动。

**② 绝对不能在事件循环里做阻塞操作**

一次同步的数据库查询、一次文件读、一次 DNS 解析，就会**卡住所有连接**。
要么异步化，要么丢给线程池。

**③ 写缓冲区管理**

```cpp
// send 可能只发出去一部分
ssize_t n = send(fd, data, len, 0);
if (n < (ssize_t)len) {
    conn.outbuf.append(data + n, len - n);      // 剩下的存起来
    enable_write_event(fd);                      // 注册 EPOLLOUT
}
// 可写事件到来时继续发，发完了要【取消】EPOLLOUT
// ★ 忘了取消的话，LT 模式下会一直触发，CPU 直接跑满 100%
```

**④ 惊群（Thundering Herd）**

多个进程/线程 epoll 同一个监听 fd，来一个连接**全被唤醒**，但只有一个能 accept 成功。

解决：`EPOLLEXCLUSIVE`（Linux 4.5+）或 `SO_REUSEPORT`（每个进程一个独立监听 fd，
内核做负载均衡）。

**⑤ 定时器**

事件循环里要处理超时（空闲连接踢下线、请求超时、重试）。

常见做法：
- **小顶堆 / 时间轮**存所有定时任务，把最近的超时时间作为 `epoll_wait` 的 timeout
- Linux 还可以用 **`timerfd`** 把定时器变成一个 fd，一起 epoll

---

<div style="page-break-after: always;"></div>

# 第 12 章 · 综合实战：迷你 HTTP 服务器

▶ 对应程序：`ch12_http_server`

```bash
build\bin\ch12_http_server.exe 8080
# 浏览器打开 http://127.0.0.1:8080
curl http://127.0.0.1:8080/api/time
curl -X POST -d "hello" http://127.0.0.1:8080/api/echo
curl -H "Authorization: Bearer secret123" http://127.0.0.1:8080/admin/panel
```

不到 400 行 C++，把前面所有东西串起来。

## 12.1 HTTP 协议格式

```
  请求                                   响应
  ─────────────────────────────────────────────────────────────────
  GET /path?query HTTP/1.1\r\n           HTTP/1.1 200 OK\r\n
  Host: example.com\r\n                  Content-Type: text/html\r\n
  Content-Length: 5\r\n                  Content-Length: 13\r\n
  \r\n            ← 空行=头部结束        \r\n
  hello           ← 正文                 <h1>Hello</h1>
```

**HTTP 是怎么解决「粘包」的**：

1. **`\r\n\r\n`** 作为分隔符，标记头部结束 —— 先找到它，才知道头部收全了
2. **`Content-Length`** 告诉你正文有多长 —— 按这个长度收满
3. 也可以用 `Transfer-Encoding: chunked`（分块传输，长度未知时用）

这正是第 9 章讲的「分隔符 + 长度前缀」混合方案。

## 12.2 解析器的关键：可重入

```cpp
// 返回 nullopt = 数据还不完整，请继续收
// 有值 = 解析成功，并【从 buf 里消费掉】这个请求
std::optional<HttpRequest> try_parse(std::string& buf) {
    size_t header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (buf.size() > 16 * 1024) throw std::runtime_error("请求头过大");  // ★ 防 DoS
        return std::nullopt;                     // 头部还没收全
    }

    // ... 解析请求行和头部 ...

    size_t content_length = /* 从头部取 */;
    if (content_length > 8 * 1024 * 1024) throw std::runtime_error("请求体过大"); // ★
    if (buf.size() < header_end + 4 + content_length) {
        return std::nullopt;                     // body 还没收全
    }

    req.body = buf.substr(header_end + 4, content_length);
    buf.erase(0, header_end + 4 + content_length);   // ★ 消费掉
    return req;
}
```

**调用方的循环**

```cpp
for (;;) {
    // 先尽量从已有缓冲里解析（一次 recv 可能收到多个请求 —— HTTP 流水线）
    while (auto req = try_parse(buf)) {
        HttpResponse res;
        router.handle(*req, res);
        send_all(conn, res.serialize());
    }
    // 缓冲里没有完整请求了，再去读
    long n = recv(conn, chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    buf.append(chunk, n);
}
```

**★ 两处长度校验必不可少**，否则恶意客户端发一个超长请求头或
`Content-Length: 4294967295` 就能把你的内存吃光。

## 12.3 架构：中间件 + 路由

```
   请求进来
      │
      ▼
  ┌───────────────────────────────┐
  │ 中间件 1: 加安全响应头        │ ─── next() ──┐
  └───────────────────────────────┘              │
                                                 ▼
  ┌───────────────────────────────┐
  │ 中间件 2: 鉴权                │
  │   /admin/* 需要 token         │ ── 没 token: 直接返回 401，不调 next()
  └───────────────────────────────┘              │
                                                 ▼
  ┌───────────────────────────────┐
  │ 路由分发                      │
  │   GET  /            → 首页    │
  │   GET  /api/time    → JSON    │
  │   POST /api/echo    → 回显    │
  │   都不匹配          → 404     │
  │   路径存在方法不对  → 405     │
  └───────────────────────────────┘
      │
      ▼   响应逐层返回，中间件 1 在 next() 之后统一加响应头（洋葱模型）
   响应出去
```

代码里用到的模式：

| 模式 | 用在哪 |
|------|--------|
| **责任链** | 中间件管道 |
| **策略** | 每个路由的 Handler 是一个 `std::function` |
| **建造者** | `HttpResponse` 的链式 `.set_status().json()` |
| **RAII** | `netc::Socket` 自动 close |

用到的现代 C++：

```cpp
std::optional<HttpRequest>          // 「可能解析不出来」
std::function<void(...)>            // Handler / Middleware
std::string_view                    // 零拷贝解析
auto [k, v] : headers               // 结构化绑定
lambda + 移动捕获                    // 路由注册
std::shared_ptr<netc::Socket>       // 跨线程转移连接所有权
```

## 12.4 HTTP 关键概念

**长连接（keep-alive）**

HTTP/1.1 默认长连接：一条 TCP 连接上可以发多个请求。
这是**最重要的性能优化**——省掉每次的三次握手（1 RTT）和 TIME_WAIT。

```
  短连接: [握手][请求1][响应1][挥手] [握手][请求2][响应2][挥手]
  长连接: [握手][请求1][响应1][请求2][响应2][请求3][响应3][挥手]
```

服务端要在响应里带 `Connection: keep-alive`，
客户端发 `Connection: close` 时才断开。

**常用状态码**

| 码 | 含义 | 什么时候用 |
|----|------|-----------|
| 200 | OK | 成功 |
| 201 | Created | POST 创建成功 |
| 204 | No Content | 成功但无正文（DELETE） |
| 301/302 | 重定向 | 永久 / 临时 |
| 304 | Not Modified | 缓存有效 |
| 400 | Bad Request | 请求格式错误 |
| 401 | Unauthorized | **未认证**（还没登录） |
| 403 | Forbidden | **已认证但无权限** |
| 404 | Not Found | 资源不存在 |
| 405 | Method Not Allowed | 路径对但方法不对 |
| 429 | Too Many Requests | 限流 |
| 500 | Internal Server Error | 服务端出错 |
| 502 | Bad Gateway | 网关拿到上游的坏响应 |
| 503 | Service Unavailable | 服务暂时不可用（过载/维护） |
| 504 | Gateway Timeout | 网关等上游超时 |

**HTTP 版本演进**

| 版本 | 关键改进 | 底层 |
|------|---------|------|
| HTTP/1.0 | 每请求一连接 | TCP |
| HTTP/1.1 | 长连接、管线化、Host 头、分块传输 | TCP |
| HTTP/2 | **二进制分帧**、多路复用（一条连接跑多个流）、头部压缩 HPACK、服务端推送 | TCP |
| HTTP/3 | 基于 **QUIC(UDP)**、0-RTT 握手、**彻底解决队头阻塞**、连接迁移 | UDP |

**队头阻塞（Head-of-Line Blocking）**：
HTTP/2 虽然在应用层做了多路复用，但底层 TCP 一旦丢包，**所有流都要等重传**。
HTTP/3 用 UDP + QUIC，每个流独立，一个流丢包不影响其它流。

## 12.5 从这里往下走

这个迷你服务器**缺什么**（也就是你可以继续练的方向）：

1. **改成事件驱动**：用第 11 章的 epoll 替换线程池，扛住 1 万连接
2. **HTTPS**：接 OpenSSL，理解 TLS 握手
3. **静态文件服务**：`sendfile` 零拷贝、`Range` 断点续传、`ETag` 缓存
4. **`Transfer-Encoding: chunked`**：流式响应
5. **WebSocket**：HTTP 升级握手 + 帧协议
6. **连接超时与限流**：定时器轮、令牌桶
7. **压测**：`wrk -t4 -c100 -d30s http://127.0.0.1:8080/`，看瓶颈在哪

**推荐研读的开源实现**（按难度排序）：

| 项目 | 规模 | 看什么 |
|------|------|--------|
| `cpp-httplib` | 单头文件 | 简洁的 HTTP 实现 |
| **muduo** | 中等 | **陈硕的 Reactor 网络库，中文注释和配套书，最适合学习** |
| `libuv` | 中等 | 跨平台事件循环（Node.js 的底座） |
| Redis | 中等 | 单线程事件循环的极致，`ae.c` 只有几百行 |
| Nginx | 大 | 工业级主从 Reactor + 模块化 |
| Boost.Asio | 大 | 现代 C++ 异步模型 + 协程支持 |

---

<div style="page-break-after: always;"></div>
