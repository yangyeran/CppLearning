// =============================================================================
// 第 16 章 —— 聊天室服务器（跨线程广播）
//
// 比回显服务器难在哪？
//   回显只需要「谁发的回给谁」，全程在同一个 loop 线程里，不需要共享状态。
//   广播必须访问「所有连接的列表」，而这些连接分布在**不同的**从属 Reactor
//   线程上。这就引出 Reactor 编程里最核心的一个问题：
//
//       如何安全地操作「不属于当前线程」的连接？
//
// 两种解法：
//
//   解法 A（本文件采用）：成员列表加锁 + conn->send() 自动跨线程投递
//     - 成员列表用 mutex 保护（因为多个从属 Reactor 线程都要读写它）
//     - 广播时对每个连接调 send()。send 内部检测到跨线程，
//       会把数据拷一份、包成任务投给对方的 loop —— 我们不需要自己处理。
//     - 优点：直观。缺点：广播时持锁遍历，成员多时是瓶颈。
//
//   解法 B（生产环境常用）：每个 loop 各自维护本 loop 的成员列表
//     - 广播时对每个 loop 调 runInLoop，让它自己遍历自己的成员
//     - 完全无锁，但代码复杂度更高
//     本文件在末尾给出了解法 B 的示意代码。
//
// 协议：行协议（以 \n 分隔），方便直接用 telnet / nc 测试。
//
// 运行：
//     ch16_chat_server.exe [端口=9002] [线程数=3]
// 测试：
//     开三个终端，各自 nc 127.0.0.1 9002，然后互相发消息
// =============================================================================

#include "reactor/reactor.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>

using namespace reactor;

namespace {
EventLoop* g_loop = nullptr;
void onSignal(int) { if (g_loop) g_loop->quit(); }
} // namespace

// =============================================================================
// 聊天室
// =============================================================================
class ChatRoom {
public:
    explicit ChatRoom(EventLoop* loop, uint16_t port, int threadNum)
        : server_(loop, netc::make_addr_v4(nullptr, port), "ChatServer") {
        server_.setThreadNum(threadNum);
        server_.setConnectionCallback(
            [this](const TcpConnectionPtr& conn) { onConnection(conn); });
        server_.setMessageCallback(
            [this](const TcpConnectionPtr& conn, Buffer* buf) { onMessage(conn, buf); });
    }

    void start() { server_.start(); }

private:
    // 每个用户的会话状态。挂在 TcpConnection 的 context 里，
    // 这样就不用再维护一个 conn -> 状态 的映射。
    struct Session {
        std::string nickname;
        bool        named = false;
    };

    void onConnection(const TcpConnectionPtr& conn) {
        if (conn->connected()) {
            conn->setContext(std::make_shared<Session>());
            {
                std::lock_guard<std::mutex> lk(mutex_);
                members_.insert(conn);
            }
            conn->send("=== 欢迎来到聊天室 ===\n"
                       "先输入你的昵称：\n");
            logInfo("新连接 " + conn->peerAddr() + "，在线人数 " + std::to_string(size()));
        } else {
            std::string nick;
            if (auto s = conn->context<Session>()) nick = s->nickname;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                members_.erase(conn);
            }
            if (!nick.empty()) {
                broadcast("*** " + nick + " 离开了聊天室 ***\n", nullptr);
            }
            logInfo("连接关闭 " + conn->peerAddr() + "，在线人数 " + std::to_string(size()));
        }
    }

    void onMessage(const TcpConnectionPtr& conn, Buffer* buf) {
        // 行协议解析：循环取出所有完整的行。
        // 一次可读事件可能包含 0 行（半行）、1 行或多行 —— 必须循环。
        while (const char* crlf = buf->findEOL()) {
            std::string line = buf->retrieveAsString(
                static_cast<size_t>(crlf - buf->peek() + 1));
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (!line.empty()) handleLine(conn, line);
        }
        // 防御：一直不发换行符的客户端会让 Buffer 无限增长
        if (buf->readableBytes() > 64 * 1024) {
            conn->send("单行过长，连接关闭。\n");
            conn->shutdown();
        }
    }

    void handleLine(const TcpConnectionPtr& conn, const std::string& line) {
        auto session = conn->context<Session>();
        if (!session) return;

        // 第一条消息作为昵称
        if (!session->named) {
            session->nickname = line.substr(0, 32);
            session->named    = true;
            conn->send("昵称设为 " + session->nickname + "。命令：/who  /quit\n");
            broadcast("*** " + session->nickname + " 加入了聊天室 ***\n", conn.get());
            return;
        }

        if (line == "/quit") {
            conn->send("再见。\n");
            conn->shutdown();
            return;
        }
        if (line == "/who") {
            std::string list = "当前在线（" + std::to_string(size()) + " 人）：\n";
            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (const auto& m : members_) {
                    if (auto s = m->context<Session>(); s && s->named) {
                        list += "  " + s->nickname + "  " + m->peerAddr() + "\n";
                    }
                }
            }
            conn->send(list);
            return;
        }

        broadcast("[" + session->nickname + "] " + line + "\n", nullptr);
    }

    // -------------------------------------------------------------------------
    // 广播 —— 本文件的核心
    //
    // 关键点：members_ 里的连接分布在不同线程上，但我们**不需要**关心。
    //         conn->send() 内部会判断：
    //           在同一线程 -> 直接写
    //           不同线程   -> 拷贝数据 + 投递任务 + 唤醒对方的 loop
    //         这就是把「跨线程共享状态」转化成「跨线程投递消息」的好处。
    //
    // 注意：这里先把成员列表拷贝出来再解锁，然后才发送。
    //       如果持锁发送，send 内部可能触发回调，回调里又访问 members_ -> 死锁。
    //       「不要在持锁时调用外部代码」是并发编程的通用铁律。
    // -------------------------------------------------------------------------
    void broadcast(const std::string& msg, const TcpConnection* except) {
        std::set<TcpConnectionPtr> snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            snapshot = members_;          // 拷贝一份（shared_ptr 拷贝，保证目标存活）
        }                                  // 锁在这里就释放了
        for (const auto& m : snapshot) {
            if (m.get() == except) continue;
            m->send(msg);                  // 可能跨线程，send 自己会处理
        }
    }

    size_t size() {
        std::lock_guard<std::mutex> lk(mutex_);
        return members_.size();
    }

    TcpServer                  server_;
    std::mutex                 mutex_;
    std::set<TcpConnectionPtr> members_;   // 被 mutex_ 保护
};

// =============================================================================
// 解法 B 示意：无锁广播（生产环境的做法）
//
// 思路：不维护全局成员列表，而是每个 EventLoop 各维护「属于自己的连接」。
//       广播时对每个 loop 投递一个任务，让它在自己线程里遍历自己的列表。
//       这样任何时刻访问某个列表的都只有它的所属线程 —— 零锁。
//
//   // 每个 loop 一份，用 thread_local 或 map<EventLoop*, set> 都可以
//   std::map<EventLoop*, std::set<TcpConnectionPtr>> perLoopMembers;
//
//   void broadcastLockFree(const std::string& msg) {
//       for (EventLoop* loop : server_.getAllLoops()) {
//           loop->runInLoop([loop, msg] {
//               // 这里在 loop 自己的线程里，访问自己的成员集合，无需加锁
//               for (const auto& conn : perLoopMembers[loop]) {
//                   conn->send(msg);      // 同线程，直接写，也没有拷贝开销
//               }
//           });
//       }
//   }
//
// 代价：连接加入/离开时也要投递到对应 loop 处理，代码更绕。
//       成员数少（< 几千）时解法 A 完全够用，别过早优化。
// =============================================================================

int main(int argc, char* argv[]) {
    netc::Startup guard;

    uint16_t port      = (argc > 1) ? static_cast<uint16_t>(std::atoi(argv[1])) : 9002;
    int      threadNum = (argc > 2) ? std::atoi(argv[2]) : 3;

    std::cout <<
        "============================================================\n"
        "  Reactor 聊天室服务器\n"
        "============================================================\n"
        "  端口     : " << port << "\n"
        "  线程数   : " << threadNum << "\n"
        "  用法     : 开多个终端各自 nc 127.0.0.1 " << port << "\n"
        "             第一条消息是昵称，之后 /who 看在线，/quit 退出\n"
        "============================================================\n\n";

    try {
        EventLoop loop;
        g_loop = &loop;
        std::signal(SIGINT, onSignal);

        ChatRoom room(&loop, port, threadNum);
        room.start();
        loop.loop();

        std::cout << "\n服务器已停止。\n";
    } catch (const std::exception& e) {
        std::cerr << "启动失败: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
