// =============================================================================
// 第 16 章 —— Reactor 完整实现（实现文件）
//
// 阅读建议：按 Buffer -> Channel -> Poller -> EventLoop -> TcpConnection
//           -> Acceptor -> TcpServer 的顺序读，这也是本文件的排列顺序。
// =============================================================================
#include "reactor.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
// WSAPoll 需要 Vista+。它与 POSIX 的 poll 结构体布局兼容，
// 所以下面的 PollPoller 一份代码就能同时服务两个平台。
#else
#  include <sys/uio.h>      // readv / iovec（分散读）
#  ifdef __linux__
#    include <sys/eventfd.h>
#  endif
#endif

namespace reactor {

// =============================================================================
// 日志（教学用最小实现）
// =============================================================================
namespace {
std::mutex      g_logMutex;
std::atomic<bool> g_logEnabled{true};
}

void setLogEnabled(bool on) { g_logEnabled.store(on); }

void logInfo(const std::string& msg) {
    if (!g_logEnabled.load()) return;
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms
        << " [T" << std::this_thread::get_id() << "] " << msg << '\n';

    std::lock_guard<std::mutex> lk(g_logMutex);
    std::cout << oss.str();
    std::cout.flush();
}

// =============================================================================
// Buffer 实现
// =============================================================================

void Buffer::retrieve(size_t len) {
    assert(len <= readableBytes());
    if (len < readableBytes()) {
        readerIndex_ += len;          // 只移动下标 —— O(1)，不搬内存
    } else {
        retrieveAll();
    }
}

void Buffer::retrieveAll() {
    // 全部取走时把下标重置到起点，让 prependable 区恢复原样。
    // 这就是 Buffer 能长期复用而不无限增长的原因。
    readerIndex_ = kCheapPrepend;
    writerIndex_ = kCheapPrepend;
}

std::string Buffer::retrieveAsString(size_t len) {
    assert(len <= readableBytes());
    std::string result(peek(), len);
    retrieve(len);
    return result;
}

std::string Buffer::retrieveAllAsString() {
    return retrieveAsString(readableBytes());
}

void Buffer::ensureWritable(size_t len) {
    if (writableBytes() < len) makeSpace(len);
    assert(writableBytes() >= len);
}

void Buffer::append(const char* data, size_t len) {
    ensureWritable(len);
    std::copy(data, data + len, beginWrite());
    hasWritten(len);
}

void Buffer::prepend(const void* data, size_t len) {
    assert(len <= prependableBytes());
    readerIndex_ -= len;
    const char* d = static_cast<const char*>(data);
    std::copy(d, d + len, begin() + readerIndex_);
}

// makeSpace 是 Buffer 的空间管理核心，两种策略：
void Buffer::makeSpace(size_t len) {
    if (writableBytes() + prependableBytes() < len + kCheapPrepend) {
        // 策略 A：总空间真的不够 -> 扩容
        buf_.resize(writerIndex_ + len);
    } else {
        // 策略 B：总空间够，只是数据「漂」到后面去了 -> 把数据搬回前面。
        //         这避免了「明明有空间却反复 resize」导致的内存无限增长。
        //         典型场景：不断 append 小消息 + retrieve，readerIndex 一路往后跑。
        assert(kCheapPrepend < readerIndex_);
        size_t readable = readableBytes();
        std::copy(begin() + readerIndex_, begin() + writerIndex_, begin() + kCheapPrepend);
        readerIndex_ = kCheapPrepend;
        writerIndex_ = readerIndex_ + readable;
        assert(readable == readableBytes());
    }
}

const char* Buffer::findCRLF() const {
    static const char kCRLF[] = "\r\n";
    const char* crlf = std::search(peek(), beginWrite(), kCRLF, kCRLF + 2);
    return crlf == beginWrite() ? nullptr : crlf;
}

const char* Buffer::findEOL() const {
    const void* eol = std::memchr(peek(), '\n', readableBytes());
    return static_cast<const char*>(eol);
}

int32_t Buffer::peekInt32() const {
    assert(readableBytes() >= sizeof(int32_t));
    int32_t be32 = 0;
    std::memcpy(&be32, peek(), sizeof(be32));
    return static_cast<int32_t>(::ntohl(static_cast<uint32_t>(be32)));  // 网络序 -> 主机序
}

int32_t Buffer::readInt32() {
    int32_t result = peekInt32();
    retrieve(sizeof(int32_t));
    return result;
}

void Buffer::appendInt32(int32_t x) {
    int32_t be32 = static_cast<int32_t>(::htonl(static_cast<uint32_t>(x)));
    append(reinterpret_cast<const char*>(&be32), sizeof(be32));
}

// -----------------------------------------------------------------------------
// readFd —— Buffer 里最值得学的函数
//
// 【问题】Buffer 该开多大？
//   开小了：一次 read 读不完，要多次系统调用（系统调用是有成本的）
//   开大了：每个连接一个 64KB Buffer，1 万连接就是 640 MB 常驻内存
//
// 【解法】栈上临时缓冲 + 分散读（scatter read）
//   准备两块缓冲区交给内核：
//     第 1 块 = Buffer 里现有的可写空间（可能很小）
//     第 2 块 = 栈上 64KB 的临时数组（不占堆，函数返回就没了）
//   内核会先填满第 1 块，再填第 2 块。一次系统调用最多能读 64KB+。
//   读完后：如果数据没超出第 1 块，什么都不用做；
//           超出了，才把第 2 块的内容 append 到 Buffer（此时才扩容）。
//
//   于是：连接空闲时 Buffer 一直很小（省内存），
//         突发大流量时一次系统调用就能全部读走（省 CPU）。
//
//   POSIX 用 readv，Windows 用 WSARecv —— 都是「分散/聚集 IO」的标准接口。
// -----------------------------------------------------------------------------
long Buffer::readFd(netc::socket_t fd, int* savedErrno) {
    char   extrabuf[65536];
    size_t writable = writableBytes();

#ifdef _WIN32
    WSABUF bufs[2];
    bufs[0].buf = beginWrite();
    bufs[0].len = static_cast<ULONG>(writable);
    bufs[1].buf = extrabuf;
    bufs[1].len = static_cast<ULONG>(sizeof(extrabuf));

    DWORD bytesRecv = 0;
    DWORD flags     = 0;
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    int rc = ::WSARecv(fd, bufs, static_cast<DWORD>(iovcnt), &bytesRecv, &flags,
                       nullptr, nullptr);
    long n = (rc == 0) ? static_cast<long>(bytesRecv) : -1;
#else
    struct iovec vec[2];
    vec[0].iov_base = beginWrite();
    vec[0].iov_len  = writable;
    vec[1].iov_base = extrabuf;
    vec[1].iov_len  = sizeof(extrabuf);

    // 只有当 Buffer 自身空间小于 extrabuf 时才用两块，
    // 否则用一块就够（避免不必要地读入超大数据）
    const int iovcnt = (writable < sizeof(extrabuf)) ? 2 : 1;
    long n = ::readv(fd, vec, iovcnt);
#endif

    if (n < 0) {
        *savedErrno = netc::last_error();
    } else if (static_cast<size_t>(n) <= writable) {
        hasWritten(static_cast<size_t>(n));              // 全装进 Buffer 了
    } else {
        writerIndex_ = buf_.size();                       // Buffer 填满
        append(extrabuf, static_cast<size_t>(n) - writable);  // 溢出部分才触发扩容
    }
    return n;
}

// =============================================================================
// Channel 实现
// =============================================================================

Channel::Channel(EventLoop* loop, netc::socket_t fd) : loop_(loop), fd_(fd) {}

Channel::~Channel() = default;

void Channel::tie(const std::shared_ptr<void>& obj) {
    tie_  = obj;
    tied_ = true;
}

void Channel::update() { loop_->updateChannel(this); }

void Channel::remove() { loop_->removeChannel(this); }

void Channel::handleEvent() {
    // 生命周期守卫：把持有者提升为 shared_ptr 并持有到本函数结束。
    // 这样即使回调内部把连接从 TcpServer 里移除了（引用计数 -1），
    // 对象也不会在本函数执行途中被析构。详见头文件里 tie() 的说明。
    if (tied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (!guard) {
            // 持有者已经销毁 —— 说明这是一个「迟到」的事件，直接丢弃。
            return;
        }
        // guard 在本作用域末尾才释放
        if ((revents_ & kCloseEvent) && !(revents_ & kReadEvent)) {
            if (closeCallback_) closeCallback_();
            return;
        }
        if (revents_ & kErrorEvent) { if (errorCallback_) errorCallback_(); return; }
        if (revents_ & kReadEvent)  { if (readCallback_)  readCallback_();  }
        if (revents_ & kWriteEvent) { if (writeCallback_) writeCallback_(); }
        return;
    }

    // 未绑定守卫的情况（比如 Acceptor 的监听 Channel、EventLoop 的唤醒 Channel）
    if ((revents_ & kCloseEvent) && !(revents_ & kReadEvent)) {
        if (closeCallback_) closeCallback_();
        return;
    }
    if (revents_ & kErrorEvent) { if (errorCallback_) errorCallback_(); return; }
    if (revents_ & kReadEvent)  { if (readCallback_)  readCallback_();  }
    if (revents_ & kWriteEvent) { if (writeCallback_) writeCallback_(); }
}

// =============================================================================
// PollPoller —— 基于 poll(2) / WSAPoll
//
// 优于 select 的地方：没有 FD_SETSIZE 上限，且不需要每次重建位图。
// 仍然不如 epoll 的地方：每次调用都要把整个 pollfd 数组拷进内核，
//                        返回后还要遍历整个数组找就绪项 —— 都是 O(n)。
// =============================================================================
namespace {

#ifdef _WIN32
using PollFd = WSAPOLLFD;
inline int doPoll(PollFd* fds, size_t n, int timeoutMs) {
    return ::WSAPoll(fds, static_cast<ULONG>(n), timeoutMs);
}
#else
using PollFd = struct pollfd;
inline int doPoll(PollFd* fds, size_t n, int timeoutMs) {
    return ::poll(fds, static_cast<nfds_t>(n), timeoutMs);
}
#endif

} // namespace

class PollPoller : public Poller {
public:
    explicit PollPoller(EventLoop* loop) : Poller(loop) {}

    const char* name() const override { return "poll"; }

    void poll(int timeoutMs, ChannelList* activeChannels) override {
        if (pollfds_.empty()) {
            // WSAPoll 在 nfds==0 时直接返回错误，所以空集合时手动睡一下
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs < 0 ? 10 : timeoutMs));
            return;
        }

        int numEvents = doPoll(pollfds_.data(), pollfds_.size(), timeoutMs);
        if (numEvents < 0) {
            if (!netc::interrupted()) {
                logInfo(std::string("PollPoller::poll 出错: ") + netc::last_error_string());
            }
            return;
        }
        if (numEvents == 0) return;   // 超时，无事件

        // O(n) 遍历找就绪项 —— poll 的固有开销
        for (const PollFd& pfd : pollfds_) {
            if (pfd.revents == 0) continue;

            auto it = channels_.find(pfd.fd);
            if (it == channels_.end()) continue;
            Channel* ch = it->second;

            int revents = kNoneEvent;
            if (pfd.revents & (POLLIN | POLLPRI))  revents |= kReadEvent;
            if (pfd.revents & POLLOUT)             revents |= kWriteEvent;
            if (pfd.revents & POLLERR)             revents |= kErrorEvent;
            if (pfd.revents & POLLNVAL)            revents |= kErrorEvent;
            // POLLHUP 表示对端挂断。注意要和 POLLIN 一起判断：
            // 挂断时可能还有数据没读完，必须先让应用层读完再关。
            if (pfd.revents & POLLHUP)             revents |= kCloseEvent;

            ch->setRevents(revents);
            activeChannels->push_back(ch);

            if (--numEvents == 0) break;   // 已找齐，提前退出
        }
    }

    void updateChannel(Channel* channel) override {
        int idx = channel->index();
        if (idx < 0) {
            // 新增
            PollFd pfd{};
            pfd.fd      = channel->fd();
            pfd.events  = toPollEvents(channel->events());
            pfd.revents = 0;
            pollfds_.push_back(pfd);
            channel->setIndex(static_cast<int>(pollfds_.size()) - 1);
            channels_[channel->fd()] = channel;
        } else {
            // 修改
            assert(idx < static_cast<int>(pollfds_.size()));
            PollFd& pfd = pollfds_[static_cast<size_t>(idx)];
            pfd.events  = toPollEvents(channel->events());
            pfd.revents = 0;
        }
    }

    void removeChannel(Channel* channel) override {
        int idx = channel->index();
        if (idx < 0) return;
        assert(idx < static_cast<int>(pollfds_.size()));

        channels_.erase(channel->fd());

        // 用「与末尾交换再 pop_back」做 O(1) 删除。
        // 注意要同步更新被换上来的那个 Channel 的 index_，否则索引就错位了。
        size_t last = pollfds_.size() - 1;
        if (static_cast<size_t>(idx) != last) {
            std::swap(pollfds_[static_cast<size_t>(idx)], pollfds_[last]);
            auto it = channels_.find(pollfds_[static_cast<size_t>(idx)].fd);
            if (it != channels_.end()) it->second->setIndex(idx);
        }
        pollfds_.pop_back();
        channel->setIndex(-1);
    }

private:
    static short toPollEvents(int events) {
        short r = 0;
        if (events & kReadEvent)  r |= POLLIN;
        if (events & kWriteEvent) r |= POLLOUT;
        return r;
    }

    std::vector<PollFd> pollfds_;
};

// =============================================================================
// EpollPoller —— Linux 专属，C10K 的答案
// =============================================================================
#ifdef __linux__
class EpollPoller : public Poller {
public:
    explicit EpollPoller(EventLoop* loop)
        : Poller(loop),
          // EPOLL_CLOEXEC：fork+exec 时自动关闭，避免 fd 泄漏到子进程
          epollfd_(::epoll_create1(EPOLL_CLOEXEC)),
          events_(kInitEventListSize) {
        if (epollfd_ < 0) netc::throw_socket_error("epoll_create1");
    }
    ~EpollPoller() override { ::close(epollfd_); }

    const char* name() const override { return "epoll"; }

    void poll(int timeoutMs, ChannelList* activeChannels) override {
        int numEvents = ::epoll_wait(epollfd_, events_.data(),
                                     static_cast<int>(events_.size()), timeoutMs);
        if (numEvents < 0) {
            if (errno != EINTR) {
                logInfo(std::string("epoll_wait 出错: ") + netc::last_error_string());
            }
            return;
        }
        if (numEvents == 0) return;

        // 关键：只遍历「就绪的」numEvents 个，与总连接数无关 —— 这就是 epoll 的核心优势
        for (int i = 0; i < numEvents; ++i) {
            auto* ch = static_cast<Channel*>(events_[static_cast<size_t>(i)].data.ptr);
            uint32_t ev = events_[static_cast<size_t>(i)].events;

            int revents = kNoneEvent;
            if (ev & (EPOLLIN | EPOLLPRI)) revents |= kReadEvent;
            if (ev & EPOLLOUT)             revents |= kWriteEvent;
            if (ev & EPOLLERR)             revents |= kErrorEvent;
            // EPOLLRDHUP：对端关闭了写端（发来 FIN）。epoll 独有，
            // 有了它就能在「对端半关闭」时立刻知道，不必等 read 返回 0。
            if (ev & EPOLLRDHUP)           revents |= kCloseEvent;

            ch->setRevents(revents);
            activeChannels->push_back(ch);
        }

        // 就绪队列被填满，说明可能还有更多事件 -> 扩容，下次一次取更多
        if (static_cast<size_t>(numEvents) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    }

    void updateChannel(Channel* channel) override {
        const int index = channel->index();
        if (index == kNew || index == kDeleted) {
            channels_[channel->fd()] = channel;
            channel->setIndex(kAdded);
            update(EPOLL_CTL_ADD, channel);
        } else {
            if (channel->isNoneEvent()) {
                // 不关心任何事件了：从 epoll 里摘掉但保留在 channels_ 里，
                // 这样以后重新 enable 时还能找到（避免反复 add/del 的开销）
                update(EPOLL_CTL_DEL, channel);
                channel->setIndex(kDeleted);
            } else {
                update(EPOLL_CTL_MOD, channel);
            }
        }
    }

    void removeChannel(Channel* channel) override {
        netc::socket_t fd = channel->fd();
        int index = channel->index();
        channels_.erase(fd);
        if (index == kAdded) update(EPOLL_CTL_DEL, channel);
        channel->setIndex(kNew);
    }

private:
    static constexpr int kNew = -1, kAdded = 1, kDeleted = 2;
    static constexpr size_t kInitEventListSize = 16;

    void update(int operation, Channel* channel) {
        epoll_event ev{};
        ev.events   = 0;
        if (channel->events() & kReadEvent)  ev.events |= EPOLLIN | EPOLLPRI;
        if (channel->events() & kWriteEvent) ev.events |= EPOLLOUT;
        ev.events |= EPOLLRDHUP;          // 总是关心对端半关闭
        // data.ptr 直接存 Channel*：epoll_wait 返回时就能 O(1) 拿到对象，
        // 不需要像 poll 那样再查一次 map。这也是 epoll 更快的一个细节。
        ev.data.ptr = channel;

        if (::epoll_ctl(epollfd_, operation, channel->fd(), &ev) < 0) {
            logInfo(std::string("epoll_ctl 失败: ") + netc::last_error_string());
        }
    }

    int                      epollfd_;
    std::vector<epoll_event> events_;
};
#endif // __linux__

std::unique_ptr<Poller> Poller::newDefaultPoller(EventLoop* loop) {
#ifdef __linux__
    return std::make_unique<EpollPoller>(loop);
#else
    return std::make_unique<PollPoller>(loop);
#endif
}

// =============================================================================
// EventLoop 实现
// =============================================================================
namespace {

// 每个线程最多一个 EventLoop —— 用 thread_local 强制这个约束
thread_local EventLoop* t_loopInThisThread = nullptr;

constexpr int kPollTimeoutMs = 10000;

} // namespace

EventLoop::EventLoop()
    : threadId_(std::this_thread::get_id()),
      poller_(Poller::newDefaultPoller(this)) {
    if (t_loopInThisThread) {
        throw std::runtime_error("本线程已经有一个 EventLoop 了（违反 one loop per thread）");
    }
    t_loopInThisThread = this;
    createWakeupChannel();
}

EventLoop::~EventLoop() {
    if (wakeupChannel_) {
        wakeupChannel_->disableAll();
        wakeupChannel_->remove();
        wakeupChannel_.reset();
    }
    destroyWakeupPair();
    t_loopInThisThread = nullptr;
}

// -----------------------------------------------------------------------------
// 唤醒机制的搭建
//
// Linux ：eventfd —— 一个 fd + 一个 64 位计数器，专为线程/进程间通知设计。
//                    写入 8 字节使计数器增加；读取则清零并返回。
//                    比 pipe 省一个 fd，也比 pipe 快。
//
// Windows：没有 eventfd，也没有 socketpair。标准做法是「自连接」：
//                    1. 在 127.0.0.1:0 上 bind + listen（端口 0 = 让系统分配）
//                    2. 查出实际端口，然后 connect 到自己
//                    3. accept 拿到另一端
//                    结果就是一对互联的 socket，写一端另一端可读。
// -----------------------------------------------------------------------------
void EventLoop::createWakeupChannel() {
#ifdef __linux__
    wakeupFd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeupFd_ < 0) netc::throw_socket_error("eventfd");
    wakeupSendFd_ = wakeupFd_;                    // eventfd 读写同一个 fd
#else
    // 自连接 socket 对
    netc::socket_t listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == netc::kInvalidSocket) netc::throw_socket_error("socket(唤醒用监听)");

    sockaddr_in addr = netc::make_addr_v4("127.0.0.1", 0);   // 端口 0 = 系统分配
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        netc::close_socket(listener);
        netc::throw_socket_error("bind(唤醒用监听)");
    }
    if (::listen(listener, 1) != 0) {
        netc::close_socket(listener);
        netc::throw_socket_error("listen(唤醒用监听)");
    }

    // 查出系统实际分配的端口
    sockaddr_in actual{};
#ifdef _WIN32
    int alen = sizeof(actual);
#else
    socklen_t alen = sizeof(actual);
#endif
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&actual), &alen) != 0) {
        netc::close_socket(listener);
        netc::throw_socket_error("getsockname(唤醒用监听)");
    }

    netc::socket_t client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == netc::kInvalidSocket) {
        netc::close_socket(listener);
        netc::throw_socket_error("socket(唤醒用客户端)");
    }
    if (::connect(client, reinterpret_cast<sockaddr*>(&actual), sizeof(actual)) != 0) {
        netc::close_socket(listener);
        netc::close_socket(client);
        netc::throw_socket_error("connect(自连接)");
    }
    netc::socket_t server = ::accept(listener, nullptr, nullptr);
    netc::close_socket(listener);                  // 监听 socket 用完即弃
    if (server == netc::kInvalidSocket) {
        netc::close_socket(client);
        netc::throw_socket_error("accept(自连接)");
    }

    netc::set_nonblocking(server, true);
    netc::set_nonblocking(client, true);
    netc::set_tcp_nodelay(server, true);           // 唤醒要求低延迟，关掉 Nagle
    netc::set_tcp_nodelay(client, true);

    wakeupFd_     = server;    // loop 线程读这一端
    wakeupSendFd_ = client;    // 其它线程写这一端
#endif

    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupChannel_->setReadCallback([this] { handleWakeup(); });
    wakeupChannel_->enableReading();
}

void EventLoop::destroyWakeupPair() {
    if (wakeupSendFd_ != netc::kInvalidSocket && wakeupSendFd_ != wakeupFd_) {
        netc::close_socket(wakeupSendFd_);
    }
    if (wakeupFd_ != netc::kInvalidSocket) {
#ifdef __linux__
        ::close(wakeupFd_);
#else
        netc::close_socket(wakeupFd_);
#endif
    }
    wakeupFd_ = wakeupSendFd_ = netc::kInvalidSocket;
}

void EventLoop::wakeup() {
    uint64_t one = 1;
#ifdef __linux__
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) logInfo("wakeup 写入字节数异常");
#else
    char byte = 'w';
    long n = netc::send_n(wakeupSendFd_, &byte, 1);
    // 唤醒失败通常是因为管道已满 —— 但管道满意味着「已经有待处理的唤醒了」，
    // 目的已达到，可以安全忽略。
    (void)n;
    (void)one;
#endif
}

void EventLoop::handleWakeup() {
    // 必须把数据读掉，否则 LT 模式会一直触发可读事件（死循环打满 CPU）
#ifdef __linux__
    uint64_t one = 0;
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    (void)n;
#else
    char buf[256];
    while (true) {
        long n = netc::recv_n(wakeupFd_, buf, sizeof(buf));
        if (n <= 0) break;                       // 读干了（返回 -1 + EWOULDBLOCK）
        if (static_cast<size_t>(n) < sizeof(buf)) break;
    }
#endif
}

void EventLoop::assertInLoopThread() const {
    if (!isInLoopThread()) {
        throw std::runtime_error("此操作必须在 EventLoop 所属线程执行");
    }
}

void EventLoop::loop() {
    assertInLoopThread();
    looping_ = true;
    quit_    = false;
    logInfo("EventLoop 开始运行，Poller = " + std::string(poller_->name()));

    while (!quit_) {
        ++iterations_;
        activeChannels_.clear();

        // 第 1 步：阻塞等待事件（整个循环唯一的阻塞点）
        poller_->poll(kPollTimeoutMs, &activeChannels_);

        // 第 2 步：分发。回调必须是非阻塞的，否则整个 loop 被拖住。
        for (Channel* channel : activeChannels_) {
            channel->handleEvent();
        }

        // 第 3 步：执行其它线程投递过来的任务。
        //   放在最后而不是最前，是为了让 IO 事件优先 —— IO 有实时性要求。
        doPendingFunctors();
    }

    looping_ = false;
    logInfo("EventLoop 停止，共循环 " + std::to_string(iterations_) + " 次");
}

void EventLoop::quit() {
    quit_ = true;
    // 如果是别的线程调 quit()，本 loop 可能正阻塞在 poll 里，
    // 必须唤醒它，否则最多要等 kPollTimeoutMs 才退出。
    if (!isInLoopThread()) wakeup();
}

void EventLoop::runInLoop(Functor cb) {
    if (isInLoopThread()) {
        cb();                       // 已经在目标线程，直接执行，零开销
    } else {
        queueInLoop(std::move(cb));
    }
}

void EventLoop::queueInLoop(Functor cb) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pendingFunctors_.push_back(std::move(cb));
    }
    // 两种情况需要唤醒：
    //   1. 调用者是其它线程 -> 目标线程可能正阻塞在 poll
    //   2. 调用者就是本线程，但正在执行 doPendingFunctors ->
    //      新任务不会被本轮循环处理（本轮的 swap 已经做完了），
    //      所以要唤醒，让 poll 立刻返回进入下一轮。
    if (!isInLoopThread() || callingPendingFunctors_) wakeup();
}

void EventLoop::doPendingFunctors() {
    std::vector<Functor> functors;
    callingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // swap 而不是逐个取：
        //   1. 临界区极短（只交换两个指针），不阻塞其它线程投递任务
        //   2. 执行回调时**不持锁** —— 否则回调里再调 queueInLoop 就自己死锁了
        functors.swap(pendingFunctors_);
    }
    for (const Functor& f : functors) f();
    callingPendingFunctors_ = false;
}

void EventLoop::updateChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    bool isNew = (channel->index() < 0);
    poller_->updateChannel(channel);
    if (isNew && channel->index() >= 0) ++channelCount_;
}

void EventLoop::removeChannel(Channel* channel) {
    assert(channel->ownerLoop() == this);
    assertInLoopThread();
    if (channel->index() >= 0 && channelCount_ > 0) --channelCount_;
    poller_->removeChannel(channel);
}

const char* EventLoop::pollerName() const { return poller_->name(); }

// =============================================================================
// TcpConnection 实现
// =============================================================================

TcpConnection::TcpConnection(EventLoop* loop, std::string name, netc::socket_t sockfd,
                             const sockaddr_in& localAddr, const sockaddr_in& peerAddr)
    : loop_(loop), name_(std::move(name)), sockfd_(sockfd),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      localAddr_(localAddr), peerAddr_(peerAddr) {
    channel_->setReadCallback([this] { handleRead(); });
    channel_->setWriteCallback([this] { handleWrite(); });
    channel_->setCloseCallback([this] { handleClose(); });
    channel_->setErrorCallback([this] { handleError(); });

    netc::set_nonblocking(sockfd_, true);
}

TcpConnection::~TcpConnection() {
    // 到这里 fd 应该已经在 connectDestroyed 里关掉了；兜底再关一次
    if (sockfd_ != netc::kInvalidSocket) {
        netc::close_socket(sockfd_);
        sockfd_ = netc::kInvalidSocket;
    }
}

void TcpConnection::connectEstablished() {
    loop_->assertInLoopThread();
    state_ = kConnected;
    // 绑定生命周期守卫：见头文件 Channel::tie 的说明
    channel_->tie(shared_from_this());
    channel_->enableReading();
    if (connectionCallback_) connectionCallback_(shared_from_this());
}

void TcpConnection::connectDestroyed() {
    loop_->assertInLoopThread();
    if (state_ == kConnected) {
        state_ = kDisconnected;
        channel_->disableAll();
    }
    channel_->remove();
}

void TcpConnection::handleRead() {
    loop_->assertInLoopThread();
    int  savedErrno = 0;
    long n = inputBuffer_.readFd(sockfd_, &savedErrno);

    if (n > 0) {
        // 把「已收到的全部数据」交给应用层。
        // 注意：传的是 Buffer* 而不是 string —— 应用层可能需要多次事件
        //       才能凑齐一个完整消息，Buffer 里的残留数据会保留到下次。
        if (messageCallback_) messageCallback_(shared_from_this(), &inputBuffer_);
    } else if (n == 0) {
        // read 返回 0 = 对端关闭了连接（收到 FIN）。这是唯一可靠的判断方式。
        handleClose();
    } else {
        if (savedErrno ==
#ifdef _WIN32
            WSAEWOULDBLOCK
#else
            EAGAIN
#endif
        ) {
            return;   // LT 模式下的虚假唤醒，无害
        }
        logInfo(name_ + " 读取出错: " + netc::error_string(savedErrno));
        handleError();
    }
}

void TcpConnection::handleWrite() {
    loop_->assertInLoopThread();
    if (!channel_->isWriting()) return;

    long n = netc::send_n(sockfd_, outputBuffer_.peek(), outputBuffer_.readableBytes());
    if (n > 0) {
        outputBuffer_.retrieve(static_cast<size_t>(n));
        if (outputBuffer_.readableBytes() == 0) {
            // 全部发完了 —— 立刻取消可写事件！
            // 不取消的话，LT 模式会因为「发送缓冲区一直有空间」而无限触发，
            // CPU 直接打满 100%。这是 Reactor 编程最经典的坑。
            channel_->disableWriting();
            if (writeCompleteCallback_) {
                loop_->queueInLoop([self = shared_from_this(), cb = writeCompleteCallback_] {
                    cb(self);
                });
            }
            if (state_ == kDisconnecting) {
                shutdownInLoop();   // 之前请求过关闭，等的就是发完这一刻
            }
        }
    } else {
        if (!netc::would_block()) {
            logInfo(name_ + " 写入出错: " + netc::last_error_string());
        }
    }
}

void TcpConnection::handleClose() {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected) return;

    state_ = kDisconnected;
    channel_->disableAll();

    TcpConnectionPtr guard(shared_from_this());   // 保证本函数执行期间对象存活
    if (connectionCallback_) connectionCallback_(guard);   // 通知应用层「断开了」
    if (closeCallback_)      closeCallback_(guard);        // 通知 TcpServer 移除
}

void TcpConnection::handleError() {
    int       err = 0;
#ifdef _WIN32
    int       len = sizeof(err);
    ::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
#else
    socklen_t len = sizeof(err);
    ::getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &err, &len);
#endif
    logInfo(name_ + " SO_ERROR = " + std::to_string(err) + " " + netc::error_string(err));
    handleClose();
}

void TcpConnection::send(std::string_view message) {
    if (state_ != kConnected) return;
    if (loop_->isInLoopThread()) {
        sendInLoop(message.data(), message.size());
    } else {
        // 跨线程发送：必须把数据**拷贝**一份带过去。
        // 不能只传指针 —— 等目标线程执行时，调用方的缓冲区可能已经没了。
        loop_->runInLoop([self = shared_from_this(), str = std::string(message)] {
            self->sendInLoop(str.data(), str.size());
        });
    }
}

void TcpConnection::send(Buffer* buf) {
    if (state_ != kConnected) return;
    if (loop_->isInLoopThread()) {
        sendInLoop(buf->peek(), buf->readableBytes());
        buf->retrieveAll();
    } else {
        loop_->runInLoop([self = shared_from_this(), str = buf->retrieveAllAsString()] {
            self->sendInLoop(str.data(), str.size());
        });
    }
}

// -----------------------------------------------------------------------------
// sendInLoop —— 发送的三条路径，这个函数是整个库里最需要看懂的
// -----------------------------------------------------------------------------
void TcpConnection::sendInLoop(const char* data, size_t len) {
    loop_->assertInLoopThread();
    if (state_ == kDisconnected || len == 0) return;

    long   nwrote    = 0;
    size_t remaining = len;

    // 路径 1：输出缓冲区为空 -> 尝试直接写进内核，跳过 Buffer（最快）
    //   为什么要判断缓冲区为空？因为 TCP 必须保序。
    //   如果 Buffer 里还有上次没发完的数据，这次的数据必须排在它后面，
    //   直接写会导致数据顺序错乱 —— 这是很难查的 bug。
    if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0) {
        nwrote = netc::send_n(sockfd_, data, len);
        if (nwrote >= 0) {
            remaining = len - static_cast<size_t>(nwrote);
            if (remaining == 0 && writeCompleteCallback_) {
                // 一次写完，直接回调（排队执行，避免回调里再调 send 造成递归）
                loop_->queueInLoop([self = shared_from_this(), cb = writeCompleteCallback_] {
                    cb(self);
                });
            }
        } else {
            nwrote = 0;
            if (!netc::would_block()) {
                logInfo(name_ + " sendInLoop 出错: " + netc::last_error_string());
                if (netc::last_error() ==
#ifdef _WIN32
                    WSAECONNRESET
#else
                    EPIPE
#endif
                ) {
                    handleClose();
                    return;
                }
            }
        }
    }

    // 路径 2/3：还有剩余 -> 存进 outputBuffer_，并注册可写事件
    if (remaining > 0) {
        size_t oldLen = outputBuffer_.readableBytes();

        // 高水位检查：对端消费太慢，数据在我们这边堆积。
        // 不管的话，一个慢客户端就能让服务端 OOM。真实事故的常见原因。
        if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_
            && highWaterMarkCallback_) {
            loop_->queueInLoop([self = shared_from_this(), cb = highWaterMarkCallback_,
                                total = oldLen + remaining] {
                cb(self, total);
            });
        }

        outputBuffer_.append(data + nwrote, remaining);
        if (!channel_->isWriting()) {
            channel_->enableWriting();   // 等内核腾出空间再由 handleWrite 续发
        }
    }
}

void TcpConnection::shutdown() {
    if (state_ == kConnected) {
        state_ = kDisconnecting;
        loop_->runInLoop([self = shared_from_this()] { self->shutdownInLoop(); });
    }
}

void TcpConnection::shutdownInLoop() {
    loop_->assertInLoopThread();
    if (channel_->isWriting()) {
        // 还有数据没发完，先别关。handleWrite 发完后会回到这里。
        return;
    }
    // 只关写端（发 FIN），读端保持打开 —— 这叫「半关闭」。
    // 意义：告诉对端「我说完了」，但仍然能接收对端剩下要说的话。
    // HTTP/1.0 的 "Connection: close" 就依赖这个语义。
#ifdef _WIN32
    ::shutdown(sockfd_, SD_SEND);
#else
    ::shutdown(sockfd_, SHUT_WR);
#endif
}

void TcpConnection::forceClose() {
    if (state_ != kDisconnected) {
        state_ = kDisconnecting;
        loop_->queueInLoop([self = shared_from_this()] { self->forceCloseInLoop(); });
    }
}

void TcpConnection::forceCloseInLoop() {
    loop_->assertInLoopThread();
    if (state_ != kDisconnected) handleClose();
}

// =============================================================================
// Acceptor 实现
// =============================================================================

Acceptor::Acceptor(EventLoop* loop, const sockaddr_in& listenAddr, bool reusePort)
    : loop_(loop),
      acceptSocket_(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)),
      acceptChannel_(loop, acceptSocket_),
      idleFd_(netc::kInvalidSocket) {
    if (acceptSocket_ == netc::kInvalidSocket) netc::throw_socket_error("socket(监听)");

    // SO_REUSEADDR：服务端重启时立刻可以重新 bind，不用等 TIME_WAIT 结束。
    // 没有它，重启会报 "Address already in use"，得等 60 秒。
    netc::set_reuse_addr(acceptSocket_, true);

    if (reusePort) {
#ifdef SO_REUSEPORT
        // SO_REUSEPORT（Linux 3.9+）：多个进程/线程各自 bind 同一端口，
        // 内核在它们之间做负载均衡。好处是彻底避免「惊群」——
        // 每个新连接内核只唤醒一个 accept 者。Nginx 就用这个。
        int on = 1;
        ::setsockopt(acceptSocket_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#else
        logInfo("本平台不支持 SO_REUSEPORT，已忽略");
#endif
    }

    if (::bind(acceptSocket_, reinterpret_cast<const sockaddr*>(&listenAddr),
               sizeof(listenAddr)) != 0) {
        netc::close_socket(acceptSocket_);
        netc::throw_socket_error("bind");
    }

    netc::set_nonblocking(acceptSocket_, true);

#ifndef _WIN32
    // 预留一个空闲 fd 用于处理 EMFILE（进程 fd 用尽）：
    //   fd 耗尽时 accept 返回 EMFILE，但连接仍然在内核的已完成队列里。
    //   LT 模式下会立刻再次触发可读 -> 死循环打满 CPU。
    //   对策：关掉这个预留 fd 腾出一个位置，accept 下来立刻关闭，
    //         再重新占回预留 fd。这样至少能把连接「礼貌地拒绝」掉。
    idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
#endif

    acceptChannel_.setReadCallback([this] { handleRead(); });
}

Acceptor::~Acceptor() {
    acceptChannel_.disableAll();
    acceptChannel_.remove();
    netc::close_socket(acceptSocket_);
#ifndef _WIN32
    if (idleFd_ >= 0) ::close(idleFd_);
#endif
}

void Acceptor::listen() {
    loop_->assertInLoopThread();
    listening_ = true;
    // backlog = 内核「已完成三次握手但还没被 accept」的队列长度。
    // SOMAXCONN 在 Linux 上受 /proc/sys/net/core/somaxconn 限制（默认 4096）。
    // 太小会导致高并发建连时客户端收到 RST 或超时重传。
    if (::listen(acceptSocket_, SOMAXCONN) != 0) netc::throw_socket_error("listen");
    acceptChannel_.enableReading();
}

void Acceptor::handleRead() {
    loop_->assertInLoopThread();

    // 循环 accept：一次事件可能对应多个已完成的连接。
    // 不循环的话，LT 模式下虽然还会再通知，但每次只处理一个，效率低。
    while (true) {
        sockaddr_in peerAddr{};
#ifdef _WIN32
        int alen = sizeof(peerAddr);
#else
        socklen_t alen = sizeof(peerAddr);
#endif
        netc::socket_t connfd =
            ::accept(acceptSocket_, reinterpret_cast<sockaddr*>(&peerAddr), &alen);

        if (connfd == netc::kInvalidSocket) {
            if (netc::would_block()) break;        // 队列空了，正常退出
            if (netc::interrupted()) continue;

#ifndef _WIN32
            if (errno == EMFILE) {                  // fd 耗尽，见构造函数的说明
                logInfo("fd 耗尽 (EMFILE)，腾出预留 fd 以礼貌拒绝连接");
                ::close(idleFd_);
                idleFd_ = ::accept(acceptSocket_, nullptr, nullptr);
                ::close(idleFd_);
                idleFd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                continue;
            }
#endif
            logInfo(std::string("accept 出错: ") + netc::last_error_string());
            break;
        }

        if (newConnectionCallback_) {
            newConnectionCallback_(connfd, peerAddr);
        } else {
            netc::close_socket(connfd);   // 没人要这个连接，直接关掉
        }
    }
}

// =============================================================================
// EventLoopThread / EventLoopThreadPool 实现
// =============================================================================

EventLoopThread::EventLoopThread(std::string name) : name_(std::move(name)) {}

EventLoopThread::~EventLoopThread() {
    exiting_ = true;
    if (loop_) {
        loop_->quit();                // 让子线程的 loop 退出
        if (thread_.joinable()) thread_.join();
    }
}

EventLoop* EventLoopThread::startLoop() {
    thread_ = std::thread([this] { threadFunc(); });

    // 必须等到子线程把 EventLoop 创建好才能返回它的指针。
    // EventLoop 只能在自己的线程里构造（构造函数会记录 threadId_），
    // 所以这里用条件变量做一次「握手」。
    std::unique_lock<std::mutex> lk(mutex_);
    cond_.wait(lk, [this] { return loop_ != nullptr; });
    return loop_;
}

void EventLoopThread::threadFunc() {
    EventLoop loop;      // 在本线程栈上构造 —— one loop per thread
    {
        std::lock_guard<std::mutex> lk(mutex_);
        loop_ = &loop;
    }
    cond_.notify_one();

    loop.loop();         // 阻塞在这里直到 quit()

    std::lock_guard<std::mutex> lk(mutex_);
    loop_ = nullptr;     // loop 已经销毁，别再让外部拿到野指针
}

EventLoopThreadPool::EventLoopThreadPool(EventLoop* baseLoop, std::string name)
    : baseLoop_(baseLoop), name_(std::move(name)) {}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start() {
    started_ = true;
    for (int i = 0; i < numThreads_; ++i) {
        auto t = std::make_unique<EventLoopThread>(name_ + std::to_string(i));
        loops_.push_back(t->startLoop());
        threads_.push_back(std::move(t));
    }
    logInfo("线程池启动，从属 Reactor 数量 = " + std::to_string(numThreads_)
            + (numThreads_ == 0 ? "（单线程模式，所有 IO 在主 loop）" : ""));
}

EventLoop* EventLoopThreadPool::getNextLoop() {
    if (loops_.empty()) return baseLoop_;      // 单线程模式
    EventLoop* loop = loops_[next_];
    next_ = (next_ + 1) % loops_.size();       // round-robin
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::getAllLoops() {
    return loops_.empty() ? std::vector<EventLoop*>{baseLoop_} : loops_;
}

// =============================================================================
// TcpServer 实现
// =============================================================================

TcpServer::TcpServer(EventLoop* loop, const sockaddr_in& listenAddr,
                     std::string name, Option option)
    : loop_(loop), name_(std::move(name)), listenAddr_(listenAddr),
      acceptor_(std::make_unique<Acceptor>(loop, listenAddr, option == kReusePort)),
      threadPool_(std::make_shared<EventLoopThreadPool>(loop, name_)) {
    acceptor_->setNewConnectionCallback(
        [this](netc::socket_t fd, const sockaddr_in& addr) { newConnection(fd, addr); });
}

TcpServer::~TcpServer() {
    // 逐个通知连接销毁。注意要在各自所属的 loop 线程里做。
    for (auto& [name, conn] : connections_) {
        TcpConnectionPtr c(conn);
        c->getLoop()->runInLoop([c] { c->connectDestroyed(); });
    }
    connections_.clear();
}

void TcpServer::setThreadNum(int n) {
    assert(n >= 0);
    threadPool_->setThreadNum(n);
}

void TcpServer::start() {
    if (started_.fetch_add(1) == 0) {          // 保证只启动一次
        threadPool_->start();
        loop_->runInLoop([this] { acceptor_->listen(); });
        logInfo(name_ + " 启动，监听 " + netc::addr_to_string(listenAddr_));
    }
}

void TcpServer::newConnection(netc::socket_t sockfd, const sockaddr_in& peerAddr) {
    loop_->assertInLoopThread();

    // 从线程池取一个从属 Reactor —— 这个连接此后一生都归它管
    EventLoop* ioLoop = threadPool_->getNextLoop();

    std::string connName = name_ + "-conn" + std::to_string(nextConnId_++);

    sockaddr_in localAddr{};
#ifdef _WIN32
    int llen = sizeof(localAddr);
#else
    socklen_t llen = sizeof(localAddr);
#endif
    ::getsockname(sockfd, reinterpret_cast<sockaddr*>(&localAddr), &llen);

    auto conn = std::make_shared<TcpConnection>(ioLoop, connName, sockfd, localAddr, peerAddr);
    connections_[connName] = conn;

    conn->setConnectionCallback(connectionCallback_);
    conn->setMessageCallback(messageCallback_);
    conn->setWriteCompleteCallback(writeCompleteCallback_);
    // 连接自己发现要关闭时，回调到这里让 TcpServer 移除它
    conn->setCloseCallback([this](const TcpConnectionPtr& c) { removeConnection(c); });

    // 关键：connectEstablished 必须在 ioLoop 线程执行，
    //       因为它要操作 Channel（注册到 ioLoop 的 Poller）。
    ioLoop->runInLoop([conn] { conn->connectEstablished(); });
}

void TcpServer::removeConnection(const TcpConnectionPtr& conn) {
    // 这个函数可能被任意从属 Reactor 线程调用，
    // 但 connections_ 只能在 loop_（主 Reactor）线程访问 -> 转投过去。
    loop_->runInLoop([this, conn] { removeConnectionInLoop(conn); });
}

void TcpServer::removeConnectionInLoop(const TcpConnectionPtr& conn) {
    loop_->assertInLoopThread();
    connections_.erase(conn->name());

    // 再转回连接自己的 loop 去做真正的清理（注销 Channel）。
    // 用 queueInLoop 而不是 runInLoop：确保 handleEvent 已经彻底返回。
    EventLoop* ioLoop = conn->getLoop();
    ioLoop->queueInLoop([conn] { conn->connectDestroyed(); });
}

} // namespace reactor
