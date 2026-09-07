# 第 16 章 · Reactor 完整实现

▶ 对应程序：`ch16_reactor_selftest`（自动化验证）、`ch16_echo_server`、`ch16_chat_server`

▶ 源码：`src/ch16_reactor/reactor/reactor.h` + `reactor.cpp`（约 1100 行，去掉注释约 600 行）

这是全书的技术收口：把第 8~12 章的网络知识、第 13 章的坑、第 14 章的智能指针全部用上，写一个能跑的小型网络库。结构对照 muduo（陈硕）与 libevent 的设计。

## 1. 为什么需要 Reactor —— 三种服务器模型

### 模型 1：一连接一线程

```cpp
while (true) {
    int conn = accept(listen_fd, ...);
    std::thread([conn]{ 处理这个连接直到断开; }).detach();
}
```

- **优点**：代码直白，每个连接的逻辑是同步的，好写好读
- **缺点**：1 万个连接 = 1 万个线程。每个线程默认 1~8 MB 栈，光栈就要几十 GB；内核调度器在上万个线程间切换，上下文切换开销吃掉大部分 CPU

这就是著名的 **C10K 问题**。

### 模型 2：线程池 + 阻塞 IO

连接数不再等于线程数，但一个线程被一个"正在等数据"的连接占住，慢连接会耗尽线程池。仍然扛不住高并发长连接。

### 模型 3：Reactor

核心思路：**别让线程等 IO，让线程等「哪些 IO 已经就绪」。**

```
┌──────────────────────────────────────────────────────┐
│                    EventLoop                         │
│                                                      │
│   ┌──────────┐   1. poller_->poll(timeout)           │
│   │  Poller  │      阻塞在这里，等内核通知             │
│   │ epoll /  │      (一个线程同时监视上万个 fd)       │
│   │ poll /   │                                       │
│   │ select   │   2. 返回「就绪的 Channel 列表」       │
│   └────┬─────┘                                       │
│        ▼                                             │
│   ┌──────────────────────────────────┐               │
│   │ for (Channel* ch : activeList)   │  3. 分发       │
│   │     ch->handleEvent();           │               │
│   └──────────────────────────────────┘               │
│        ▼  回调是**非阻塞**的，处理完立刻返回循环       │
│   onMessage / onConnection / onWriteComplete         │
└──────────────────────────────────────────────────────┘
```

一个线程 + 一个 epoll 就能撑数万连接，因为线程从不空等。

**代价**：所有回调必须非阻塞。回调里做一次同步数据库查询，整个循环就卡住了 —— 这是 Reactor 编程最重要的纪律。

## 2. 类结构

自底向上：

| 类 | 职责 |
|---|---|
| `Buffer` | 应用层收发缓冲区（非阻塞 IO 的必需品） |
| `Poller` | 对 epoll / poll / select 的抽象 |
| `EpollPoller` / `PollPoller` | Linux / 通用实现 |
| `Channel` | 一个 fd + 关心的事件 + 各类回调（**不拥有** fd） |
| `EventLoop` | 事件循环，one loop per thread |
| `EventLoopThread(Pool)` | 从属 Reactor 池（round-robin 分配连接） |
| `TcpConnection` | 一条 TCP 连接的完整生命周期 |
| `Acceptor` | 专门处理监听 fd 的 accept |
| `TcpServer` | 把上面全部组装起来的门面 |

用户只需要和 `TcpServer` 打交道 —— 一个完整的回显服务器就这么点代码：

```cpp
EventLoop loop;
TcpServer server(&loop, netc::make_addr_v4(nullptr, 9000), "Echo");
server.setThreadNum(4);
server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
    conn->send(buf->retrieveAllAsString());
});
server.start();
loop.loop();
```

## 3. 主从 Reactor

```
     ┌────────────────────┐
     │   主 Reactor       │   只负责 accept，不做任何 IO
     │   (mainLoop)       │
     │   Acceptor         │
     └─────────┬──────────┘
               │ 新连接按 round-robin 分给从 Reactor
     ┌─────────┼─────────┬─────────────┐
     ▼         ▼         ▼             ▼
┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
│subLoop 0│ │subLoop 1│ │subLoop 2│ │subLoop 3│   每个跑在独立线程
│ N 个连接│ │ N 个连接│ │ N 个连接│ │ N 个连接│
└─────────┘ └─────────┘ └─────────┘ └─────────┘
```

**关键约束：一个连接只属于一个 EventLoop，永不跨线程迁移。**

于是同一连接的所有回调都在同一线程串行执行 —— 连接内部的状态（Buffer、协议解析进度）**根本不需要加锁**。

这是 Reactor 相比"共享状态 + 加锁"模型最大的工程优势：不是性能，而是**正确性容易保证**。

## 4. 应用层 Buffer —— 非阻塞 IO 的必需品

### 4.1 为什么必须有

**读方向**：非阻塞 `read` 一次可能只返回半个消息。比如协议是"4 字节长度 + N 字节内容"，`read` 返回 3 字节 —— 你连长度都读不完整。必须把这 3 字节**存起来**，等下次可读事件再拼。这就是粘包/半包处理的本质。

**写方向**：非阻塞 `write` 一次可能只写进去一部分（内核发送缓冲区满了）。剩下的必须存起来，然后**注册可写事件**，等内核腾出空间再继续写。

自测实测：发送 2 MB 数据，服务端触发了 **32 次**可读事件，单次 Buffer 峰值 65 KB。一条消息被拆成这么多次 —— 没有 Buffer 就必然出错。

### 4.2 内存布局

```
+-------------------+------------------+------------------+
| prependable       |     readable     |     writable     |
+-------------------+------------------+------------------+
0      <=      readerIndex   <=   writerIndex    <=     size()
```

`prependable` 区（预留 8 字节，刚好放一个 int64 长度字段）的用途：想在消息前面加长度字段时，不用挪动整个消息，直接往前写 4 字节：

```cpp
Buffer out;
out.append(body);                            // 先写载荷
int32_t be = htonl((uint32_t)body.size());
out.prepend(&be, sizeof(be));                // 最后往前面塞长度，零搬移
```

对比朴素做法"先算长度、先写头、再写体"，`prepend` 让你可以**写完才知道多长** —— 序列化嵌套结构时非常有用。

### 4.3 makeSpace 的两种策略

```cpp
void makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        buf_.resize(writerIndex_ + len);         // 策略 A：总空间不够 -> 扩容
    } else {
        // 策略 B：总空间够，只是数据「漂」到后面了 -> 搬回前面
        // 这避免了「明明有空间却反复 resize」导致的内存无限增长
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
    }
}
```

典型触发场景：不断 append 小消息 + retrieve，`readerIndex` 一路往后跑。

### 4.4 readFd —— 最值得学的函数

**问题**：Buffer 该开多大？

- 开小了：一次 read 读不完，多次系统调用（系统调用有成本）
- 开大了：每个连接一个 64KB Buffer，1 万连接就是 640 MB 常驻内存

**解法**：栈上临时缓冲 + **分散读**（scatter read）

准备两块缓冲区交给内核：第 1 块是 Buffer 现有的可写空间（可能很小），第 2 块是栈上 64KB 临时数组。内核先填满第 1 块再填第 2 块，一次系统调用最多读 64KB+。

```cpp
long Buffer::readFd(socket_t fd, int* savedErrno) {
    char   extrabuf[65536];              // 栈上，函数返回就没了
    size_t writable = writableBytes();

    struct iovec vec[2];
    vec[0].iov_base = beginWrite();   vec[0].iov_len = writable;
    vec[1].iov_base = extrabuf;       vec[1].iov_len = sizeof(extrabuf);
    long n = ::readv(fd, vec, (writable < sizeof(extrabuf)) ? 2 : 1);

    if (n <= writable) {
        hasWritten(n);                                    // 全装进 Buffer 了
    } else {
        writerIndex_ = buf_.size();
        append(extrabuf, n - writable);                   // 溢出部分才触发扩容
    }
    return n;
}
```

于是：**连接空闲时 Buffer 一直很小（省内存），突发大流量时一次系统调用就能全部读走（省 CPU）。**

POSIX 用 `readv`，Windows 用 `WSARecv` —— 都是"分散/聚集 IO"的标准接口。

## 5. Channel 与 tie() —— 生命周期的坑

`Channel` 把"一个 fd""它关心哪些事件""事件发生时调什么函数"绑在一起。**它不拥有 fd** —— fd 的关闭由持有者负责（`TcpConnection` 持有连接 fd，`Acceptor` 持有监听 fd）。

### 为什么需要 tie()

考虑这个执行序列：

1. 可读事件触发，`Channel::handleEvent()` 开始执行
2. 回调里发现对端关闭，调用 `TcpConnection::handleClose()`
3. `handleClose` 把 `TcpConnection` 从 `TcpServer` 的 map 里移除
4. `TcpConnection` 的引用计数归零，**对象析构**
5. 但 `Channel` 是 `TcpConnection` 的成员！`Channel` 也被析构了
6. `handleEvent()` 还在执行，继续访问已析构的 `this` → **崩溃**

解法：`handleEvent` 开头把持有者的 `weak_ptr` 提升成 `shared_ptr` 并持有到函数结束：

```cpp
void Channel::handleEvent() {
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (!guard) return;        // 持有者已销毁 —— 这是「迟到」的事件，丢弃
        // guard 在本作用域末尾才释放，所以对象活到回调结束
        ...分发事件...
    }
}
```

绑定发生在连接建立时：

```cpp
void TcpConnection::connectEstablished() {
    channel_->tie(shared_from_this());        // 第 14 章的 enable_shared_from_this
    channel_->enableReading();
}
```

**这就是第 13 章坑 22 和第 14 章 enable_shared_from_this 在真实项目里的样子。** 不是学术练习 —— 少了它，服务器在高并发断连时会随机崩溃，而且极难复现。

## 6. Poller —— 三种多路复用机制

| 机制 | 缺陷 | 复杂度 |
|---|---|---|
| `select` | ① `FD_SETSIZE` 硬上限（Linux 1024，Windows 64）② 每次调用都要把整个 fd 集合从用户态拷到内核态 ③ 返回后要遍历所有 fd | O(总连接数) |
| `poll` | 用数组代替位图，去掉了 `FD_SETSIZE` 限制，但②③依旧 | O(总连接数) |
| `epoll` | 解决了根本问题 | **O(就绪连接数)** |

epoll 的三个关键改进：

1. `epoll_ctl` 注册一次，内核**持久保存**（不再每次拷贝）
2. 内核维护一个**就绪队列**，`epoll_wait` 只返回就绪的 fd
3. `epoll_event.data.ptr` 可以直接存 `Channel*` —— 返回时 O(1) 拿到对象，不需要像 poll 那样再查一次 map

10000 个连接、其中 10 个活跃时：select/poll 要检查 10000 个，epoll 只返回 10 个。**这就是 epoll 能撑 C10K/C100K 的原因。**

（BSD/macOS 的对应物是 kqueue，Windows 是 IOCP —— IOCP 是"完成端口"模型，语义上属于 **Proactor** 而非 Reactor。）

### LT vs ET

**LT（Level Triggered，水平触发，默认）**：只要缓冲区里**还有**数据没读完，就一直通知你。

- 好处：不会漏事件，一次没读完下次还会通知，代码简单
- 坏处：如果注册了可写事件但一直没数据要写，会被反复唤醒（CPU 100%）

**ET（Edge Triggered，边缘触发）**：只在"状态从无到有"的那一刻通知一次。

- 好处：通知次数少
- 坏处：**必须一次把数据读干**（循环 read 直到 EAGAIN），否则剩下的数据永远不会再通知，连接"假死"。因此 ET 下 fd 必须是非阻塞的，否则最后那次 read 会永久阻塞

**本实现用 LT** —— 与 muduo 一致。陈硕的理由：LT 的编程模型简单得多，而 ET 带来的性能提升在实际压测中并不显著。

### LT 模式的铁律

```cpp
void TcpConnection::handleWrite() {
    long n = send(sockfd_, outputBuffer_.peek(), outputBuffer_.readableBytes());
    outputBuffer_.retrieve(n);
    if (outputBuffer_.readableBytes() == 0) {
        channel_->disableWriting();      // ← 全部发完了，立刻取消可写事件！
    }
}
```

**不取消的话，LT 模式会因为"发送缓冲区一直有空间"而无限触发，CPU 直接打满 100%。** 这是 Reactor 编程最经典的坑。

## 7. EventLoop 与跨线程唤醒

### 7.1 one loop per thread

一个 `EventLoop` 对象只能在创建它的线程里 `loop()`。本实现用 `thread_local` 强制这个约束：

```cpp
thread_local EventLoop* t_loopInThisThread = nullptr;

EventLoop::EventLoop() : threadId_(std::this_thread::get_id()) {
    if (t_loopInThisThread) throw std::runtime_error("本线程已有 EventLoop");
    t_loopInThisThread = this;
}
```

### 7.2 唤醒机制 —— self-pipe trick

**问题**：主线程想让 subLoop 立刻处理一个新连接，但 subLoop 此刻正阻塞在 `epoll_wait` 里（可能设置了 10 秒超时）。怎么让它马上醒？

**解法**：给每个 EventLoop 配一个"唤醒 fd"，也注册到自己的 Poller 里。想唤醒它就往这个 fd 写 1 个字节 —— `epoll_wait` 立刻返回。

| 平台 | 实现 |
|---|---|
| Linux | `eventfd(2)` —— 专为此设计，只占一个 fd，8 字节计数器，比 pipe 省一个 fd 也更快 |
| Windows | 没有 eventfd 也没有 socketpair → **自连接**：在 `127.0.0.1:0` 上 bind+listen（端口 0 = 系统分配），查出实际端口，connect 到自己，accept 拿到另一端 |

这个技巧叫 **self-pipe trick**，1990 年代就有了，至今是标准做法。

唤醒 fd 的数据**必须读掉**，否则 LT 模式会一直触发可读事件（死循环打满 CPU）。

### 7.3 runInLoop —— 把跨线程共享变成跨线程投递

```cpp
void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();                       // 已经在目标线程，直接执行，零开销
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    { std::lock_guard lk(mutex_); pendingFunctors_.push_back(std::move(cb)); }
    if (!isInLoopThread() || callingPendingFunctors_) wakeup();
}
```

两种情况需要唤醒：

1. 调用者是其它线程 → 目标线程可能正阻塞在 poll
2. 调用者就是本线程，但正在执行 `doPendingFunctors` → 新任务不会被本轮循环处理（本轮的 swap 已经做完），所以要唤醒让 poll 立刻返回进入下一轮

### 7.4 doPendingFunctors 的两个讲究

```cpp
void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard lk(mutex_);
        functors.swap(pendingFunctors_);      // ← 关键
    }
    for (const Functor& f : functors) f();     // ← 不持锁执行
    callingPendingFunctors_ = false;
}
```

用 `swap` 而不是逐个取：

1. 临界区极短（只交换两个指针），不阻塞其它线程投递任务
2. 执行回调时**不持锁** —— 否则回调里再调 `queueInLoop` 就自己死锁了

### 7.5 循环主体

```cpp
while (!quit_) {
    activeChannels_.clear();
    poller_->poll(kPollTimeoutMs, &activeChannels_);   // 1. 唯一的阻塞点
    for (Channel* ch : activeChannels_) ch->handleEvent();  // 2. 分发
    doPendingFunctors();                                // 3. 执行投递的任务
}
```

第 3 步放在最后而不是最前，是为了让 IO 事件优先 —— IO 有实时性要求。

## 8. TcpConnection

### 8.1 状态机

```
kConnecting ──connectEstablished()──► kConnected
                                          │
                    ┌─────────────────────┼──────────────────┐
                    │                     │                  │
              shutdown()            对端 FIN            出错/强制关闭
                    ▼                     ▼                  ▼
              kDisconnecting ───────► kDisconnected ◄────────┘
              (半关闭：我不再发，
               但还能收)
```

### 8.2 sendInLoop 的三条路径

这是整个库里最需要看懂的函数：

**路径 1：输出缓冲区为空 → 尝试直接写进内核**（最快，跳过 Buffer）

```cpp
if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
    nwrote = send(sockfd_, data, len);
    remaining = len - nwrote;
}
```

**为什么要判断缓冲区为空？** 因为 TCP 必须保序。如果 Buffer 里还有上次没发完的数据，这次的数据必须排在它后面，直接写会导致**数据顺序错乱** —— 这是很难查的 bug。

**路径 2/3：还有剩余 → 存进 outputBuffer_ 并注册可写事件**

```cpp
if (remaining > 0) {
    outputBuffer_.append(data + nwrote, remaining);
    if (!channel_->isWriting()) channel_->enableWriting();
}
```

### 8.3 高水位回调 —— 慢客户端保护

服务端疯狂发数据、客户端故意不读时，数据会堆在服务端的 `outputBuffer_` 里。不管的话，**一个慢客户端就能让服务端 OOM** —— 这是真实事故的常见原因。

```cpp
conn->setHighWaterMarkCallback(
    [](const TcpConnectionPtr& c, size_t bytes) {
        c->forceClose();          // 典型处置：直接踢掉这个慢客户端
    },
    64 * 1024 * 1024);            // 64 MB 高水位线
```

自测验证：设 256 KB 高水位，服务端猛塞 8 MB，回调准确在堆积 256 KB 时触发。

### 8.4 优雅关闭（半关闭）

```cpp
void TcpConnection::shutdownInLoop() {
    if (channel_->isWriting()) return;    // 还有数据没发完，先别关
    ::shutdown(sockfd_, SHUT_WR);          // 只关写端（发 FIN），读端保持打开
}
```

**只关写端的意义**：告诉对端"我说完了"，但仍然能接收对端剩下要说的话。HTTP/1.0 的 `Connection: close` 就依赖这个语义。

注意 `if (channel_->isWriting()) return;` —— 如果还有数据在发，先不关；`handleWrite` 发完后会回到这里。这保证了"先把话说完再挂电话"。

### 8.5 跨线程 send

```cpp
void TcpConnection::send(std::string_view message) {
    if (loop_->isInLoopThread()) {
        sendInLoop(message.data(), message.size());
    } else {
        // 必须把数据**拷贝**一份带过去。不能只传指针 ——
        // 等目标线程执行时，调用方的缓冲区可能已经没了。
        loop_->runInLoop([self = shared_from_this(), str = std::string(message)] {
            self->sendInLoop(str.data(), str.size());
        });
    }
}
```

那个 `std::string(message)` 拷贝不是浪费，是**必需**的 —— 又是一次 `string_view` 悬垂的防御（第 13 章 2.2）。

## 9. Acceptor 与两个生产细节

### 9.1 SO_REUSEADDR 与 SO_REUSEPORT

```cpp
netc::set_reuse_addr(acceptSocket_, true);
```

**SO_REUSEADDR**：服务端重启时立刻可以重新 bind，不用等 TIME_WAIT 结束。没有它，重启会报 `Address already in use`，得等 60 秒。服务端**几乎必设**。

**SO_REUSEPORT**（Linux 3.9+）：多个进程/线程各自 bind 同一端口，内核在它们之间做负载均衡。好处是彻底避免"惊群"—— 每个新连接内核只唤醒一个 accept 者。Nginx 就用这个。

### 9.2 EMFILE 的处理 —— 一个真实的生产陷阱

fd 耗尽时 `accept` 返回 `EMFILE`，但**连接仍然在内核的已完成队列里**。LT 模式下会立刻再次触发可读 → 死循环打满 CPU。

对策：预留一个空闲 fd。

```cpp
idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);     // 构造时占一个

// accept 遇到 EMFILE 时：
::close(idleFd_);                              // 腾出一个位置
idleFd_ = ::accept(acceptSocket_, nullptr, nullptr);
::close(idleFd_);                              // accept 下来立刻关闭
idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);     // 重新占回预留 fd
```

这样至少能把连接"礼貌地拒绝"掉，而不是把 CPU 打满。这个技巧来自 muduo，是很多自研网络库会漏掉的细节。

### 9.3 循环 accept

一次事件可能对应多个已完成的连接，所以要循环 accept 直到 `EWOULDBLOCK`。

### 9.4 backlog

```cpp
::listen(acceptSocket_, SOMAXCONN);
```

backlog = 内核"已完成三次握手但还没被 accept"的队列长度。Linux 上受 `/proc/sys/net/core/somaxconn` 限制（默认 4096）。太小会导致高并发建连时客户端收到 RST 或超时重传。

## 10. 聊天室：跨线程广播的两种解法

广播必须访问"所有连接的列表"，而这些连接分布在**不同的**从属 Reactor 线程上。

### 解法 A：成员列表加锁 + send 自动跨线程投递

```cpp
void broadcast(const std::string& msg) {
    std::set<TcpConnectionPtr> snapshot;
    {
        std::lock_guard lk(mutex_);
        snapshot = members_;          // 拷一份（shared_ptr 拷贝，保证目标存活）
    }                                  // 锁在这里就释放了
    for (const auto& m : snapshot) {
        m->send(msg);                  // 可能跨线程，send 自己会处理
    }
}
```

**注意先拷贝再解锁，然后才发送。** 如果持锁发送，`send` 内部可能触发回调，回调里又访问 `members_` → 死锁。**"不要在持锁时调用外部代码"是并发编程的通用铁律**（第 13 章 5.3）。

### 解法 B：每个 loop 各维护本 loop 的成员列表

```cpp
std::map<EventLoop*, std::set<TcpConnectionPtr>> perLoopMembers;

void broadcastLockFree(const std::string& msg) {
    for (EventLoop* loop : server_.getAllLoops()) {
        loop->runInLoop([loop, msg] {
            // 在 loop 自己的线程里，访问自己的成员集合，无需加锁
            for (const auto& conn : perLoopMembers[loop]) {
                conn->send(msg);      // 同线程，直接写，也没有拷贝开销
            }
        });
    }
}
```

完全无锁，但连接加入/离开时也要投递到对应 loop 处理，代码更绕。

**成员数少（< 几千）时解法 A 完全够用，别过早优化。**

## 11. 自测结果

`ch16_reactor_selftest` 在单进程内启动 Reactor 服务器 + 阻塞式客户端线程，自动跑完 8 组验证：

| 测试 | 验证点 | 结果 |
|---|---|---|
| 1 基本回显 | 连接、发送、接收、关闭的完整流程 | ✅ |
| 2 多连接并发 | 40 个并发连接经 4 个从属 Reactor 分发 | ✅ 40/40 |
| 3 大消息分片 | 2 MB 数据经 32 次可读事件拼接，逐字节一致 | ✅ |
| 4 长度前缀协议 | 3 条消息粘在一个包里发送，正确拆分 | ✅ 3/3 |
| 5 跨线程 send | 主线程推送到从属 Reactor 的连接 | ✅ |
| 6 优雅关闭 | 先收到完整回复再收到 EOF | ✅ |
| 7 高水位回调 | 慢客户端在堆积 256 KB 时被检出 | ✅ |
| 8 吞吐压测 | 8 客户端 × 2000 次往返，无错误 | ✅ ~13 万 QPS（Debug） |

关于那个 QPS：这是"一问一答"模式，主要受**往返延迟**限制，不是服务端的吞吐上限。真实压测要用 pipeline 或更多连接。

测试代码故意用**阻塞式** IO 写客户端 —— 测试代码越简单越好，不要让测试本身成为 bug 来源。

## 12. 独立运行

```bash
# 回显服务器
build\bin\Debug\ch16_echo_server.exe 9001 4
# 另一个终端
nc 127.0.0.1 9001          # Linux/macOS
telnet 127.0.0.1 9001      # Windows

# 聊天室：开三个终端各自连上，互相发消息
build\bin\Debug\ch16_chat_server.exe 9002 3
```

聊天室支持 `/who` 看在线、`/quit` 退出，第一条消息作为昵称。

## 本章要点

1. **Reactor = 「等就绪」而非「等数据」**，一个线程管上万连接
2. **one loop per thread**：连接绑定到固定线程，连接内状态无需加锁 —— 这是正确性优势，不只是性能
3. **应用层 Buffer 是非阻塞 IO 的必需品**（半包 / 粘包 / 写不完），`readv` + 栈上 extrabuf 兼顾省内存与省系统调用
4. **`Channel::tie` + `enable_shared_from_this`** 解决"回调执行中对象被销毁"
5. **写完必须 `disableWriting()`**，否则 LT 模式 CPU 打满
6. 跨线程调用统一走 **`runInLoop` + eventfd/socketpair 唤醒**，把共享状态变成消息投递
7. **高水位回调**是防慢客户端打爆内存的必要手段
8. **epoll 只返回就绪 fd**（O(就绪数)），poll/select 要遍历全部（O(总数)）
9. 生产细节容易漏：`SO_REUSEADDR`、EMFILE 预留 fd、循环 accept、backlog、半关闭
