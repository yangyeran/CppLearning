// =============================================================================
// 第 16 章 —— Reactor 完整实现（头文件）
//
// 这是一个可以真正跑起来的小型网络库，结构对照 muduo（陈硕）与 libevent 的设计。
// 代码量约 1100 行，去掉注释约 600 行。
//
// ─────────────────────────────────────────────────────────────────────────────
// 为什么需要 Reactor？—— 三种服务器模型的演进
// ─────────────────────────────────────────────────────────────────────────────
//
// 模型 1：一连接一线程 (thread per connection)
//
//     while (true) {
//         int conn = accept(listen_fd, ...);
//         std::thread([conn]{ 处理这个连接直到断开; }).detach();
//     }
//
//   优点：代码直白，每个连接的逻辑是同步的，好写好读。
//   缺点：1 万个连接 = 1 万个线程。每个线程默认 1~8 MB 栈，光栈就要几十 GB；
//         内核调度器在上万个线程间切换，上下文切换开销吃掉大部分 CPU。
//         这就是著名的 C10K 问题。
//
// 模型 2：线程池 + 阻塞 IO
//
//   连接数不再等于线程数，但一个线程被一个「正在等数据」的连接占住，
//   慢连接会耗尽线程池。仍然扛不住高并发长连接。
//
// 模型 3：Reactor（本章实现）
//
//   核心思路：**别让线程等 IO，让线程等「哪些 IO 已经就绪」**。
//
//     ┌──────────────────────────────────────────────────────┐
//     │                    EventLoop                         │
//     │                                                      │
//     │   ┌──────────┐   1.  poller_->poll(timeout)          │
//     │   │  Poller  │       阻塞在这里，等内核通知            │
//     │   │ epoll /  │       (一个线程同时监视上万个 fd)      │
//     │   │ poll /   │                                       │
//     │   │ select   │   2.  返回「就绪的 Channel 列表」      │
//     │   └────┬─────┘                                       │
//     │        │                                             │
//     │        ▼                                             │
//     │   ┌──────────────────────────────────┐               │
//     │   │ for (Channel* ch : activeList)   │  3. 分发       │
//     │   │     ch->handleEvent();           │               │
//     │   └──────────────────────────────────┘               │
//     │        │                                             │
//     │        ▼  回调是**非阻塞**的，处理完立刻返回循环       │
//     │   onMessage / onConnection / onWriteComplete         │
//     └──────────────────────────────────────────────────────┘
//
//   一个线程 + 一个 epoll 就能撑数万连接，因为线程从不空等。
//   代价：所有回调必须非阻塞。回调里做一次同步数据库查询，整个循环就卡住了。
//
// ─────────────────────────────────────────────────────────────────────────────
// 本库的类结构（自底向上）
// ─────────────────────────────────────────────────────────────────────────────
//
//   Buffer              应用层收发缓冲区（非阻塞 IO 的必需品）
//   Poller              对 epoll / poll / select 的抽象
//     ├─ EpollPoller    Linux
//     └─ SelectPoller   Windows / 通用回退
//   Channel             一个 fd + 关心的事件 + 各类回调（不拥有 fd）
//   EventLoop           事件循环，one loop per thread
//   EventLoopThread     跑一个 EventLoop 的线程
//   EventLoopThreadPool  从属 Reactor 池（round-robin 分配连接）
//   TcpConnection       一条 TCP 连接的完整生命周期
//   Acceptor            专门处理监听 fd 的 accept
//   TcpServer           把上面全部组装起来的门面
//
// ─────────────────────────────────────────────────────────────────────────────
// 主从 Reactor（多线程模型）
// ─────────────────────────────────────────────────────────────────────────────
//
//        ┌────────────────────┐
//        │   主 Reactor       │   只负责 accept，不做任何 IO
//        │   (mainLoop)       │
//        │   Acceptor         │
//        └─────────┬──────────┘
//                  │ 新连接按 round-robin 分给从 Reactor
//        ┌─────────┼─────────┬─────────────┐
//        ▼         ▼         ▼             ▼
//   ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
//   │subLoop 0│ │subLoop 1│ │subLoop 2│ │subLoop 3│   每个跑在独立线程
//   │ N 个连接│ │ N 个连接│ │ N 个连接│ │ N 个连接│
//   └─────────┘ └─────────┘ └─────────┘ └─────────┘
//
//   关键约束：**一个连接只属于一个 EventLoop，永不跨线程迁移。**
//   于是同一连接的所有回调都在同一线程串行执行 —— 连接内部的状态
//   （比如 Buffer、协议解析进度）根本不需要加锁。
//   这是 Reactor 相比「共享状态 + 加锁」模型最大的工程优势。
// =============================================================================
#pragma once

#include "net_compat.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace reactor {

// =============================================================================
// 事件位掩码
//
// 不直接用 POLLIN / EPOLLIN，是为了让上层代码与具体的多路复用机制解耦。
// Poller 的实现负责在这套抽象事件与平台事件之间转换。
// =============================================================================
enum EventFlag : int {
    kNoneEvent  = 0,
    kReadEvent  = 1 << 0,   // 可读（或对端关闭，需要 read 返回 0 才能确认）
    kWriteEvent = 1 << 1,   // 可写（发送缓冲区有空间）
    kErrorEvent = 1 << 2,   // 出错
    kCloseEvent = 1 << 3,   // 对端关闭写端（EPOLLRDHUP，Linux 才有）
};

// =============================================================================
// Buffer —— 应用层缓冲区
//
// 【为什么非阻塞 IO 必须有应用层缓冲区？】
//
// 读方向：
//   非阻塞 read 一次可能只返回半个消息。比如你的协议是「4 字节长度 + N 字节
//   内容」，read 返回 3 字节 —— 你连长度都读不完整。这时必须把这 3 字节
//   **存起来**，等下次可读事件来了再拼。这就是「粘包/半包」的处理本质。
//   没有 Buffer，你就只能阻塞式循环读，那 Reactor 的意义就没了。
//
// 写方向：
//   非阻塞 write 一次可能只写进去一部分（内核发送缓冲区满了）。
//   剩下的必须存起来，然后**注册可写事件**，等内核腾出空间再继续写。
//   写完了要**取消可写事件**（否则 LT 模式会疯狂触发，CPU 打满）。
//
// 【内存布局】
//
//   +-------------------+------------------+------------------+
//   | prependable       |     readable     |     writable     |
//   +-------------------+------------------+------------------+
//   0      <=      readerIndex   <=   writerIndex    <=     size()
//
//   prependable 区（预留区）的用途：想在消息前面加个长度字段时，
//   不用把整个消息挪动，直接往前写 4 字节即可 —— 这就是 prepend()。
//   编码「长度前缀协议」时非常有用。
//
// 【为什么初始留 8 字节 prependable？】
//   刚好放得下一个 int64 长度字段，覆盖绝大多数长度前缀协议。
// =============================================================================
class Buffer {
public:
    static constexpr size_t kCheapPrepend = 8;
    static constexpr size_t kInitialSize  = 1024;

    explicit Buffer(size_t initial = kInitialSize)
        : buf_(kCheapPrepend + initial), readerIndex_(kCheapPrepend),
          writerIndex_(kCheapPrepend) {}

    size_t readableBytes()   const { return writerIndex_ - readerIndex_; }
    size_t writableBytes()   const { return buf_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }

    const char* peek() const { return begin() + readerIndex_; }
    char*       beginWrite()       { return begin() + writerIndex_; }
    const char* beginWrite() const { return begin() + writerIndex_; }

    // 取走数据（只移动下标，不搬内存 —— 这是环形复用的关键）
    void retrieve(size_t len);
    void retrieveAll();
    std::string retrieveAsString(size_t len);
    std::string retrieveAllAsString();

    // 追加数据
    void append(const char* data, size_t len);
    void append(std::string_view sv) { append(sv.data(), sv.size()); }
    void hasWritten(size_t len) { writerIndex_ += len; }
    void ensureWritable(size_t len);

    // 在 readerIndex 之前插入（利用 prependable 区，零搬移）
    void prepend(const void* data, size_t len);

    // 查找 CRLF —— 行协议（HTTP 头、Redis 协议）解析用
    const char* findCRLF() const;
    const char* findEOL() const;

    // 大端序读写（网络字节序），长度前缀协议用
    int32_t  peekInt32() const;
    int32_t  readInt32();
    void     appendInt32(int32_t x);

    // 直接从 fd 读到本缓冲区。返回读取字节数，<0 表示出错。
    // 这个函数是 Buffer 里最巧的地方，实现里有详细说明。
    long readFd(netc::socket_t fd, int* savedErrno);

    size_t internalCapacity() const { return buf_.capacity(); }

private:
    char*       begin()       { return buf_.data(); }
    const char* begin() const { return buf_.data(); }
    void        makeSpace(size_t len);

    std::vector<char> buf_;
    size_t            readerIndex_;
    size_t            writerIndex_;
};

// =============================================================================
// Channel —— fd 与事件回调的绑定
//
// 【职责】把「一个 fd」「它关心哪些事件」「事件发生时调什么函数」三者绑在一起。
//
// 【重要：Channel 不拥有 fd】
//   fd 的关闭由持有者负责（TcpConnection 持有连接 fd，Acceptor 持有监听 fd）。
//   Channel 只是「事件的注册表项」。这样职责单一，也避免了重复关闭。
//
// 【为什么需要 tie()？】
//   考虑这个执行序列：
//     1. 可读事件触发，Channel::handleEvent() 开始执行
//     2. 回调里发现对端关闭，调用 TcpConnection::handleClose()
//     3. handleClose 把 TcpConnection 从 TcpServer 的 map 里移除
//     4. TcpConnection 的引用计数归零，**对象析构**
//     5. 但 Channel 是 TcpConnection 的成员！Channel 也被析构了
//     6. handleEvent() 还在执行，继续访问已析构的 this -> 崩溃
//
//   解法：handleEvent 开头把 TcpConnection 的 weak_ptr 提升成 shared_ptr
//         并持有到函数结束。这样即使别处移除了连接，对象也会活到回调结束。
//         这正是第 13 章坑 22 和第 14 章 enable_shared_from_this 的实战。
// =============================================================================
class EventLoop;

class Channel {
public:
    using EventCallback     = std::function<void()>;
    using ReadEventCallback = std::function<void()>;

    Channel(EventLoop* loop, netc::socket_t fd);
    ~Channel();

    Channel(const Channel&)            = delete;
    Channel& operator=(const Channel&) = delete;

    // 由 EventLoop 调用：分发就绪事件到对应回调
    void handleEvent();

    void setReadCallback(ReadEventCallback cb) { readCallback_  = std::move(cb); }
    void setWriteCallback(EventCallback cb)    { writeCallback_ = std::move(cb); }
    void setCloseCallback(EventCallback cb)    { closeCallback_ = std::move(cb); }
    void setErrorCallback(EventCallback cb)    { errorCallback_ = std::move(cb); }

    // 绑定生命周期守卫，见上面 tie() 的说明
    void tie(const std::shared_ptr<void>& obj);

    netc::socket_t fd()          const { return fd_; }
    int            events()      const { return events_; }
    void           setRevents(int r)   { revents_ = r; }

    // 修改关心的事件 —— 每次都要通知 Poller 更新内核里的注册信息
    void enableReading()  { events_ |= kReadEvent;   update(); }
    void disableReading() { events_ &= ~kReadEvent;  update(); }
    void enableWriting()  { events_ |= kWriteEvent;  update(); }
    void disableWriting() { events_ &= ~kWriteEvent; update(); }
    void disableAll()     { events_ = kNoneEvent;    update(); }

    bool isWriting() const { return (events_ & kWriteEvent) != 0; }
    bool isReading() const { return (events_ & kReadEvent) != 0; }
    bool isNoneEvent() const { return events_ == kNoneEvent; }

    // Poller 用来记录这个 Channel 的注册状态（-1 新建 / 1 已注册 / 2 已删除）
    int  index() const { return index_; }
    void setIndex(int i) { index_ = i; }

    EventLoop* ownerLoop() const { return loop_; }
    void       remove();

private:
    void update();

    EventLoop*     loop_;
    netc::socket_t fd_;
    int            events_  = kNoneEvent;   // 我关心哪些事件
    int            revents_ = kNoneEvent;   // Poller 告诉我实际发生了哪些
    int            index_   = -1;

    std::weak_ptr<void> tie_;               // 生命周期守卫（见上文）
    bool                tied_ = false;

    ReadEventCallback readCallback_;
    EventCallback     writeCallback_;
    EventCallback     closeCallback_;
    EventCallback     errorCallback_;
};

// =============================================================================
// Poller —— 多路复用机制的抽象基类
//
// 【三种机制的本质区别】
//
//   select   1965 年代的接口。用位图传递 fd 集合。
//            致命缺陷 1：FD_SETSIZE 硬上限（Linux 1024，Windows 64）
//            致命缺陷 2：每次调用都要把整个 fd 集合从用户态拷到内核态
//            致命缺陷 3：返回后要遍历所有 fd 才知道谁就绪 -> O(n)
//            唯一优点：所有平台都有
//
//   poll     用数组代替位图，去掉了 FD_SETSIZE 限制。
//            但「每次全量拷贝 + 返回后 O(n) 遍历」两个问题依旧。
//
//   epoll    Linux 2.6 引入，解决了根本问题：
//            - epoll_ctl 注册一次，内核**持久保存**（不再每次拷贝）
//            - 内核维护一个「就绪队列」，epoll_wait 只返回就绪的 fd
//            - 于是复杂度从 O(总连接数) 降到 O(就绪连接数)
//
//            10000 个连接、其中 10 个活跃时：
//              select/poll 要检查 10000 个    epoll 只返回 10 个
//            这就是 epoll 能撑 C10K/C100K 的原因。
//
//            (BSD/macOS 的对应物是 kqueue，Windows 是 IOCP —— IOCP 是
//             「完成端口」模型，语义上属于 Proactor 而非 Reactor。)
//
// 【LT vs ET —— epoll 独有的两种触发模式】
//
//   LT (Level Triggered，水平触发，默认)
//     只要缓冲区里**还有**数据没读完，就一直通知你。
//     好处：不会漏事件，一次没读完下次还会通知，代码简单不易错。
//     坏处：如果你注册了可写事件但一直没数据要写，会被反复唤醒（CPU 100%）。
//           所以「写完就要 disableWriting()」是 LT 模式的铁律。
//
//   ET (Edge Triggered，边缘触发)
//     只在「状态从无到有」的那一刻通知一次。
//     好处：通知次数少，惊群更轻。
//     坏处：**必须一次把数据读干**（循环 read 直到返回 EAGAIN），
//           否则剩下的数据永远不会再通知你，连接就「假死」了。
//           因此 ET 模式下 fd 必须是非阻塞的，否则最后那次 read 会永久阻塞。
//
//   本实现用 LT —— 与 muduo 的选择一致。陈硕的理由：LT 的编程模型简单得多，
//   而 ET 带来的性能提升在实际压测中并不显著。
// =============================================================================
class Poller {
public:
    using ChannelList = std::vector<Channel*>;

    explicit Poller(EventLoop* loop) : ownerLoop_(loop) {}
    virtual ~Poller() = default;

    // 阻塞等待事件。timeoutMs < 0 表示无限等待。
    // 就绪的 Channel 填入 activeChannels。
    virtual void poll(int timeoutMs, ChannelList* activeChannels) = 0;

    // 新增/修改一个 Channel 的关注事件
    virtual void updateChannel(Channel* channel) = 0;
    // 彻底移除一个 Channel
    virtual void removeChannel(Channel* channel) = 0;

    virtual const char* name() const = 0;

    // 工厂：Linux 优先 epoll，其它平台用 select
    static std::unique_ptr<Poller> newDefaultPoller(EventLoop* loop);

protected:
    EventLoop* ownerLoop_;
    // fd -> Channel*。Poller 只借用 Channel，不管它的生命周期。
    std::map<netc::socket_t, Channel*> channels_;
};

// =============================================================================
// EventLoop —— 事件循环，整个库的心脏
//
// 【one loop per thread 原则】
//   一个 EventLoop 对象只能在创建它的那个线程里 loop()。
//   所有对它的操作，要么发生在它自己的线程，要么通过 runInLoop 排队过去。
//   这样绝大部分数据结构都不需要加锁。
//
// 【跨线程唤醒机制 —— 本类最巧的部分】
//
//   问题：主线程想让 subLoop 立刻处理一个新连接，但 subLoop 此刻正阻塞在
//         epoll_wait 里（可能设置了 10 秒超时）。怎么让它马上醒？
//
//   解法：给每个 EventLoop 配一个「唤醒 fd」，也注册到自己的 Poller 里。
//         想唤醒它就往这个 fd 写 1 个字节 —— 于是 epoll_wait 立刻返回。
//
//         Linux ：eventfd(2)，专门为此设计，只占一个 fd，8 字节计数器
//         Windows：没有 eventfd，用「自连接的 loopback socket 对」模拟
//                  （connect 到自己 bind 的临时监听端口，见实现）
//
//   这个技巧叫 "self-pipe trick"，1990 年代就有了，至今是标准做法。
//
// 【任务队列】
//   runInLoop(cb)：如果当前就在本线程 -> 直接执行
//                  否则 -> 加入队列并 wakeup()，由目标线程稍后执行
//   这让「跨线程调用」变成了「跨线程投递任务」，彻底避开了共享状态加锁。
// =============================================================================
class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop();    // 开始事件循环，直到 quit()
    void quit();    // 可从任意线程调用

    // 在本 loop 所属线程执行 cb（本线程则立即执行）
    void runInLoop(Functor cb);
    // 无条件排队，由本 loop 线程稍后执行
    void queueInLoop(Functor cb);

    void wakeup();  // 唤醒阻塞中的 poll

    void updateChannel(Channel* channel);
    void removeChannel(Channel* channel);

    bool isInLoopThread() const { return threadId_ == std::this_thread::get_id(); }
    void assertInLoopThread() const;

    const char* pollerName() const;
    size_t      channelCount() const { return channelCount_; }
    int64_t     iterations()   const { return iterations_; }

private:
    void handleWakeup();          // 读掉唤醒 fd 里的数据
    void doPendingFunctors();     // 执行任务队列
    void createWakeupChannel();
    void destroyWakeupPair();

    std::atomic<bool> looping_{false};
    std::atomic<bool> quit_{false};
    std::atomic<bool> callingPendingFunctors_{false};

    const std::thread::id            threadId_;
    std::unique_ptr<Poller>          poller_;
    Poller::ChannelList              activeChannels_;
    size_t                           channelCount_ = 0;
    int64_t                          iterations_   = 0;

    // 唤醒机制。Linux 上 wakeupFd_ 是 eventfd；Windows 上是 socket 对。
    netc::socket_t                   wakeupFd_    = netc::kInvalidSocket;
    netc::socket_t                   wakeupSendFd_ = netc::kInvalidSocket;
    std::unique_ptr<Channel>         wakeupChannel_;

    std::mutex                       mutex_;
    std::vector<Functor>             pendingFunctors_;   // 被 mutex_ 保护
};

// =============================================================================
// TcpConnection —— 一条 TCP 连接
//
// 【为什么必须继承 enable_shared_from_this？】
//   连接对象的生命周期比看起来复杂得多：
//     - TcpServer 的 map 持有它
//     - 正在执行的回调持有它（通过 Channel::tie）
//     - 用户代码可能把 TcpConnectionPtr 存进自己的容器（比如聊天室的成员列表）
//     - 异步任务队列里可能还排着针对它的任务
//   任何一方都可能是「最后一个持有者」，所以只能用引用计数。
//
// 【连接状态机】
//
//     kConnecting ──connectEstablished()──► kConnected
//                                              │
//                        ┌─────────────────────┼──────────────────┐
//                        │                     │                  │
//                  shutdown()            对端 FIN            出错/强制关闭
//                        │                     │                  │
//                        ▼                     ▼                  ▼
//                  kDisconnecting ───────► kDisconnected ◄────────┘
//                  (半关闭：我不再发，
//                   但还能收)
//
// 【send 的三条路径】（实现里有对应注释）
//   1. 输出缓冲区为空 且 内核缓冲区有空间 -> 直接 write，最快路径，零拷贝到 Buffer
//   2. 只写进去一部分            -> 剩余存入 outputBuffer_ 并 enableWriting()
//   3. 输出缓冲区已有数据        -> 必须追加到队尾，保证顺序（不能插队！）
// =============================================================================
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

using ConnectionCallback    = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback         = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback       = std::function<void(const TcpConnectionPtr&, Buffer*)>;
using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    TcpConnection(EventLoop* loop, std::string name, netc::socket_t sockfd,
                  const sockaddr_in& localAddr, const sockaddr_in& peerAddr);
    ~TcpConnection();

    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop*         getLoop()  const { return loop_; }
    const std::string& name()     const { return name_; }
    std::string        localAddr() const { return netc::addr_to_string(localAddr_); }
    std::string        peerAddr()  const { return netc::addr_to_string(peerAddr_); }
    bool               connected() const { return state_ == kConnected; }

    // 线程安全：可从任意线程调用，内部会转投到所属 loop 线程
    void send(std::string_view message);
    void send(Buffer* buf);
    void shutdown();       // 优雅关闭：只关写端，等对端也关
    void forceClose();     // 强制关闭

    void setTcpNoDelay(bool on) { netc::set_tcp_nodelay(sockfd_, on); }

    void setConnectionCallback(ConnectionCallback cb)   { connectionCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb)         { messageCallback_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { writeCompleteCallback_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb)             { closeCallback_ = std::move(cb); }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb, size_t mark) {
        highWaterMarkCallback_ = std::move(cb);
        highWaterMark_ = mark;
    }

    Buffer* inputBuffer()  { return &inputBuffer_; }
    Buffer* outputBuffer() { return &outputBuffer_; }

    // 由 TcpServer 在连接建立/销毁时调用
    void connectEstablished();
    void connectDestroyed();

    // 允许用户挂任意上下文（比如 HTTP 解析状态、玩家对象）
    void  setContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }
    template <typename T> std::shared_ptr<T> context() const {
        return std::static_pointer_cast<T>(context_);
    }

private:
    enum State { kConnecting, kConnected, kDisconnecting, kDisconnected };

    void handleRead();
    void handleWrite();
    void handleClose();
    void handleError();

    void sendInLoop(const char* data, size_t len);
    void shutdownInLoop();
    void forceCloseInLoop();

    EventLoop*               loop_;
    std::string              name_;
    std::atomic<State>       state_{kConnecting};
    netc::socket_t           sockfd_;
    std::unique_ptr<Channel> channel_;
    sockaddr_in              localAddr_;
    sockaddr_in              peerAddr_;

    Buffer inputBuffer_;    // 收到但还没被应用层消费的数据
    Buffer outputBuffer_;   // 应用层要发但内核还没收下的数据

    ConnectionCallback    connectionCallback_;
    MessageCallback       messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;
    CloseCallback         closeCallback_;
    HighWaterMarkCallback highWaterMarkCallback_;

    // 高水位线：outputBuffer_ 堆积超过这个值就回调通知应用层「对端消费太慢」。
    // 不处理的话，慢客户端会让服务端内存无限增长 —— 这是真实事故的常见原因。
    size_t highWaterMark_ = 64 * 1024 * 1024;

    std::shared_ptr<void> context_;
};

// =============================================================================
// Acceptor —— 只干一件事：accept 新连接
// =============================================================================
class Acceptor {
public:
    using NewConnectionCallback =
        std::function<void(netc::socket_t sockfd, const sockaddr_in& peerAddr)>;

    Acceptor(EventLoop* loop, const sockaddr_in& listenAddr, bool reusePort = false);
    ~Acceptor();

    void setNewConnectionCallback(NewConnectionCallback cb) { newConnectionCallback_ = std::move(cb); }
    void listen();
    bool listening() const { return listening_; }

private:
    void handleRead();

    EventLoop*            loop_;
    netc::socket_t        acceptSocket_;
    Channel               acceptChannel_;
    NewConnectionCallback newConnectionCallback_;
    bool                  listening_ = false;
    // 预留一个空闲 fd，用于处理 EMFILE（文件描述符耗尽），见实现注释
    netc::socket_t        idleFd_;
};

// =============================================================================
// EventLoopThread / EventLoopThreadPool —— 从属 Reactor
// =============================================================================
class EventLoopThread {
public:
    explicit EventLoopThread(std::string name = "");
    ~EventLoopThread();

    EventLoop* startLoop();   // 启动线程并阻塞等到 EventLoop 就绪

private:
    void threadFunc();

    EventLoop*              loop_ = nullptr;
    std::thread             thread_;
    std::string             name_;
    std::mutex              mutex_;
    std::condition_variable cond_;
    bool                    exiting_ = false;
};

class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(EventLoop* baseLoop, std::string name = "pool");
    ~EventLoopThreadPool();

    void setThreadNum(int n) { numThreads_ = n; }
    void start();

    // round-robin 取下一个 loop。线程数为 0 时返回 baseLoop（单线程模式）
    EventLoop*              getNextLoop();
    std::vector<EventLoop*> getAllLoops();

private:
    EventLoop*                                    baseLoop_;
    std::string                                   name_;
    bool                                          started_ = false;
    int                                           numThreads_ = 0;
    size_t                                        next_ = 0;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*>                       loops_;
};

// =============================================================================
// TcpServer —— 门面类，用户只需要和它打交道
//
// 典型用法：
//     EventLoop loop;
//     TcpServer server(&loop, netc::make_addr_v4(nullptr, 9000), "Echo");
//     server.setThreadNum(4);
//     server.setMessageCallback([](const TcpConnectionPtr& conn, Buffer* buf) {
//         conn->send(buf->retrieveAllAsString());
//     });
//     server.start();
//     loop.loop();
// =============================================================================
class TcpServer {
public:
    enum Option { kNoReusePort, kReusePort };

    TcpServer(EventLoop* loop, const sockaddr_in& listenAddr,
              std::string name = "TcpServer", Option option = kNoReusePort);
    ~TcpServer();

    void setThreadNum(int n);
    void start();

    void setConnectionCallback(ConnectionCallback cb) { connectionCallback_ = std::move(cb); }
    void setMessageCallback(MessageCallback cb)       { messageCallback_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { writeCompleteCallback_ = std::move(cb); }

    EventLoop*         getLoop() const { return loop_; }
    const std::string& name()    const { return name_; }
    size_t             connectionCount() const { return connections_.size(); }
    std::vector<EventLoop*> getAllLoops() { return threadPool_->getAllLoops(); }

private:
    void newConnection(netc::socket_t sockfd, const sockaddr_in& peerAddr);
    void removeConnection(const TcpConnectionPtr& conn);
    void removeConnectionInLoop(const TcpConnectionPtr& conn);

    EventLoop*                           loop_;         // 主 Reactor（accept 用）
    std::string                          name_;
    sockaddr_in                          listenAddr_;
    std::unique_ptr<Acceptor>            acceptor_;
    std::shared_ptr<EventLoopThreadPool> threadPool_;   // 从属 Reactor 池

    ConnectionCallback    connectionCallback_;
    MessageCallback       messageCallback_;
    WriteCompleteCallback writeCompleteCallback_;

    std::atomic<int>                              started_{0};
    int                                           nextConnId_ = 1;
    std::map<std::string, TcpConnectionPtr>       connections_;  // 只在 loop_ 线程访问
};

// 小工具：给日志打时间戳前缀（教学代码用，生产环境请用正规日志库）
void logInfo(const std::string& msg);
void setLogEnabled(bool on);

} // namespace reactor
