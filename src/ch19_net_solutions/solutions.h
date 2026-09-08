// =============================================================================
// 第 19 章 —— 网络练习题参考答案（可复用组件）
//
// 对应练习：第 9~14 题
//
// 本头文件放「纯算法/纯协议」部分，它们不依赖 socket，因此可以单独单元测试。
// 需要真实网络的部分（RPC 服务端、文件传输）在 main.cpp 里，基于第 16 章的
// Reactor 库实现 —— 这也正好验证那个库确实能用来干活。
//
// 包含：
//   1. LengthPrefixCodec  第 9 题：长度前缀协议的编解码（能处理任意拆包）
//   2. TimerHeap          第 10 题：小顶堆管理超时，供 poll/epoll 的 timeout 用
//   3. Md5 / Sha1 / Base64  第 11、14 题需要的摘要算法（带标准测试向量）
//   4. WebSocket          第 14 题：握手计算 + 帧编解码
// =============================================================================
#pragma once

#include "net_compat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cctype>
#include <cstddef>
#include <utility>

namespace sol {

// =============================================================================
// 第 9 题：长度前缀协议
//
// 协议：[4 字节大端长度][长度字节的消息体]
//
// 【为什么需要它】
//   TCP 是**字节流**，没有消息边界。你 send 三次，对方可能一次 recv 到全部
//   （粘包），也可能分五次收到（拆包）。所以应用层必须自己划定边界。
//   三种常见做法：
//     a) 固定长度        —— 简单但浪费
//     b) 分隔符（\n）    —— 消息体里出现分隔符就要转义（HTTP 头、Redis 用这个）
//     c) 长度前缀        —— 最通用，二进制安全，无需转义 ← 本题
//
// 【本题真正的考点】
//   解码器必须是**可重入**的：每次只喂进来几个字节也要能正确工作。
//   这意味着不能用「读满 4 字节头再读 N 字节体」的阻塞式思路，
//   而要写成一个状态机：有多少数据处理多少，不够就存着等下次。
//
// 【常见错法】
//   ✗ 假设一次 recv 就能拿到完整的 4 字节头（头本身也会被拆！）
//   ✗ 不校验长度上限 -> 对方发一个 0xFFFFFFFF，你就去 resize 4 GB（内存炸弹）
//   ✗ 用主机字节序写长度 -> 跨平台/跨语言立刻出错
//   ✗ 解出一条消息就 return -> 粘包时剩下的消息要等下一次事件才处理（延迟翻倍）
// =============================================================================
class LengthPrefixCodec {
public:
    using MessageCallback = std::function<void(std::string&& payload)>;

    static constexpr std::uint32_t kDefaultMaxLength = 16u * 1024 * 1024;   // 16 MB

    explicit LengthPrefixCodec(std::uint32_t maxLength = kDefaultMaxLength)
        : maxLength_(maxLength) {}

    // ---- 编码 ----
    static std::string encode(std::string_view payload) {
        std::string out;
        out.reserve(4 + payload.size());
        std::uint32_t be = ::htonl(static_cast<std::uint32_t>(payload.size()));
        out.append(reinterpret_cast<const char*>(&be), 4);
        out.append(payload);
        return out;
    }

    // ---- 解码 ----
    // 把收到的任意长度数据喂进来（哪怕只有 1 字节）。
    // 每凑齐一条完整消息就回调一次；一次调用可能回调 0 次、1 次或多次。
    // 返回 false 表示协议错误（长度超限），调用方应该关闭连接。
    bool feed(const char* data, std::size_t len, const MessageCallback& onMessage) {
        buffer_.append(data, len);

        // while 而不是 if：一次喂进来的数据可能包含多条完整消息
        while (true) {
            if (buffer_.size() < 4) return true;            // 连长度字段都不完整

            std::uint32_t be = 0;
            std::memcpy(&be, buffer_.data(), 4);
            std::uint32_t bodyLen = ::ntohl(be);

            // 必须校验：否则一个恶意的长度值就能让你分配天量内存
            if (bodyLen > maxLength_) {
                lastError_ = "消息长度 " + std::to_string(bodyLen)
                             + " 超过上限 " + std::to_string(maxLength_);
                return false;
            }

            if (buffer_.size() < 4 + static_cast<std::size_t>(bodyLen)) {
                return true;                                 // 消息体还没收全，等下次
            }

            std::string payload = buffer_.substr(4, bodyLen);
            buffer_.erase(0, 4 + static_cast<std::size_t>(bodyLen));
            ++decodedCount_;
            onMessage(std::move(payload));
        }
    }

    bool feed(std::string_view sv, const MessageCallback& onMessage) {
        return feed(sv.data(), sv.size(), onMessage);
    }

    std::size_t        pendingBytes() const noexcept { return buffer_.size(); }
    std::size_t        decodedCount() const noexcept { return decodedCount_; }
    const std::string& lastError()    const noexcept { return lastError_; }
    void               reset() { buffer_.clear(); decodedCount_ = 0; lastError_.clear(); }

private:
    std::string   buffer_;          // 尚未凑成完整消息的残留数据
    std::uint32_t maxLength_;
    std::size_t   decodedCount_ = 0;
    std::string   lastError_;
};

// =============================================================================
// 第 10 题：定时器堆
//
// 用途：给事件循环算出「下次 poll/epoll_wait 最多能睡多久」。
//
// 【为什么用小顶堆】
//   事件循环每轮都要问「最近的超时还有多久」，这是取最小值 -> 堆的 top()，O(1)。
//   插入/删除 O(log n)。
//   替代方案：
//     - 有序链表：插入 O(n)，不行
//     - 时间轮 (timing wheel)：O(1) 插入和到期，但精度受槽位粒度限制，
//       且需要预分配。连接数极多（十万级心跳）时比堆好，Kafka/Netty 用它。
//     - 红黑树 (std::map)：也是 O(log n)，muduo 用的是 set<pair<Timestamp,Timer*>>
//
// 【本题关键：怎么取消定时器】
//   std::priority_queue 不支持删除中间元素。两种解法：
//     a) 惰性删除：给每个定时器一个 id + 一个「已取消」集合，
//        弹出时检查是否已取消，是就跳过。← 本实现
//     b) 用 std::multimap / std::set，支持按迭代器删除。
//   惰性删除的代价是堆里可能积累已取消的项，所以要在 size 过大时清理。
//
// 【常见错法】
//   ✗ 用 system_clock 计时 -> 它可能被 NTP 或用户改时间往回调，
//     导致定时器永远不触发。**必须用 steady_clock**。
//   ✗ timeout 传给 poll 时算出负数 -> poll 会永久阻塞（-1 = 无限等待）
//   ✗ 忘了「已经过期」的情况 -> 应该返回 0 让 poll 立刻返回
// =============================================================================
class TimerHeap {
public:
    using Clock     = std::chrono::steady_clock;      // 必须是单调时钟
    using TimePoint = Clock::time_point;
    using Callback  = std::function<void()>;
    using TimerId   = std::uint64_t;

    // 在 delay 之后触发。返回可用于取消的 id。
    TimerId addTimer(std::chrono::milliseconds delay, Callback cb) {
        TimerId id = nextId_++;
        heap_.push(Entry{Clock::now() + delay, id, std::move(cb)});
        return id;
    }

    TimerId addTimerAt(TimePoint when, Callback cb) {
        TimerId id = nextId_++;
        heap_.push(Entry{when, id, std::move(cb)});
        return id;
    }

    // 惰性取消：只记下 id，实际条目留在堆里，弹出时跳过
    void cancel(TimerId id) { cancelled_.insert(id); }

    // 供 poll/epoll_wait 使用的超时值（毫秒）。
    //   没有定时器      -> 返回 defaultMs（通常是几秒，让循环有机会检查退出标志）
    //   有定时器且未到期 -> 返回剩余毫秒
    //   有定时器已过期   -> 返回 0，让 poll 立刻返回去处理它
    int nextTimeoutMs(int defaultMs = 10000) {
        purgeCancelledTop();
        if (heap_.empty()) return defaultMs;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                          heap_.top().when - Clock::now()).count();
        if (remain <= 0) return 0;
        return static_cast<int>(std::min<long long>(remain, defaultMs));
    }

    // 触发所有已到期的定时器，返回触发个数。
    // 注意：回调里可能再 addTimer（比如周期性心跳），所以要先取出再调用，
    //       否则可能陷入「回调加的定时器立刻又到期」的死循环。
    std::size_t tick() {
        auto        now = Clock::now();
        std::vector<Entry> due;
        while (!heap_.empty() && heap_.top().when <= now) {
            Entry e = heap_.top();
            heap_.pop();
            if (cancelled_.erase(e.id) == 0) due.push_back(std::move(e));
        }
        for (auto& e : due) e.cb();
        return due.size();
    }

    std::size_t size() const noexcept { return heap_.size(); }
    bool        empty() const noexcept { return heap_.empty(); }

private:
    struct Entry {
        TimePoint when;
        TimerId   id;
        Callback  cb;
        // priority_queue 默认是**大**顶堆，所以比较符要反过来写才得到小顶堆
        bool operator<(const Entry& o) const { return when > o.when; }
    };

    void purgeCancelledTop() {
        while (!heap_.empty() && cancelled_.count(heap_.top().id)) {
            cancelled_.erase(heap_.top().id);
            heap_.pop();
        }
    }

    std::priority_queue<Entry> heap_;

    // 已取消的 id 集合（惰性删除用）。
    // 用 map<id,char> 而不是 set 只是为了 erase 返回删除个数，
    // 这样 tick() 里能一次完成「是否已取消 + 移除记录」两件事。
    struct IdSet {
        std::unordered_map<TimerId, char> m;
        void        insert(TimerId id) { m[id] = 1; }
        std::size_t erase(TimerId id)  { return m.erase(id); }
        std::size_t count(TimerId id) const { return m.count(id); }
    } cancelled_;

    TimerId nextId_ = 1;
};

// =============================================================================
// 摘要算法（第 11、14 题需要）
//
// 自己实现是因为：C++ 标准库**没有**任何加密摘要算法。
// 生产环境请用 OpenSSL / libsodium，别用这里的实现 ——
// 它没有做侧信道防护，且 MD5/SHA-1 早已不适合安全用途
// （碰撞攻击已实用化）。用它们做**完整性校验**仍然可以。
// =============================================================================

// ---------------------------------------------------------------------------
// MD5（RFC 1321）
// ---------------------------------------------------------------------------
class Md5 {
public:
    Md5() { reset(); }

    void reset() {
        state_ = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
        bitCount_ = 0;
        bufferLen_ = 0;
    }

    void update(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bitCount_ += static_cast<std::uint64_t>(len) * 8;

        // 先把上次剩下的凑满 64 字节
        if (bufferLen_ > 0) {
            std::size_t need = 64 - bufferLen_;
            std::size_t take = std::min(need, len);
            std::memcpy(buffer_.data() + bufferLen_, p, take);
            bufferLen_ += take;
            p   += take;
            len -= take;
            if (bufferLen_ == 64) { transform(buffer_.data()); bufferLen_ = 0; }
        }
        // 整块处理
        while (len >= 64) { transform(p); p += 64; len -= 64; }
        // 剩余存起来
        if (len > 0) { std::memcpy(buffer_.data(), p, len); bufferLen_ = len; }
    }

    void update(std::string_view sv) { update(sv.data(), sv.size()); }

    // 返回 16 字节原始摘要
    std::array<std::uint8_t, 16> digest() {
        // 填充：先补一个 0x80，再补 0 到 56 字节，最后 8 字节放**小端**位长度
        std::uint64_t bits = bitCount_;
        std::uint8_t  pad  = 0x80;
        update(&pad, 1);
        std::uint8_t zero = 0;
        while (bufferLen_ != 56) update(&zero, 1);

        std::uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<std::uint8_t>(bits >> (8 * i));
        // 直接 transform，绕过 update 以免再次修改 bitCount_
        std::memcpy(buffer_.data() + 56, lenBytes, 8);
        transform(buffer_.data());
        bufferLen_ = 0;

        std::array<std::uint8_t, 16> out{};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                out[static_cast<std::size_t>(i * 4 + j)] =
                    static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)] >> (8 * j));
            }
        }
        return out;
    }

    std::string hex() {
        auto d = digest();
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (std::uint8_t b : d) ss << std::setw(2) << static_cast<int>(b);
        return ss.str();
    }

    static std::string hexOf(std::string_view data) {
        Md5 m; m.update(data); return m.hex();
    }

private:
    static std::uint32_t rotl(std::uint32_t x, int c) {
        return (x << c) | (x >> (32 - c));
    }

    void transform(const std::uint8_t* block) {
        static const std::uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
            0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
            0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
            0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
            0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
            0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
            0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
            0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
            0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {
            7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
            5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
            4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
            6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};

        std::uint32_t M[16];
        for (int i = 0; i < 16; ++i) {   // MD5 用小端读入
            M[i] = static_cast<std::uint32_t>(block[i * 4])
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16)      { f = (b & c) | (~b & d);            g = i; }
            else if (i < 32) { f = (d & b) | (~d & c);            g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;                     g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);                  g = (7 * i) % 16; }

            std::uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rotl(a + f + K[i] + M[g], S[i]);
            a = tmp;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    }

    std::array<std::uint32_t, 4>  state_{};
    std::array<std::uint8_t, 64>  buffer_{};
    std::size_t                   bufferLen_ = 0;
    std::uint64_t                 bitCount_  = 0;
};

// ---------------------------------------------------------------------------
// SHA-1（RFC 3174）—— WebSocket 握手需要
// ---------------------------------------------------------------------------
class Sha1 {
public:
    Sha1() { reset(); }

    void reset() {
        state_ = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
        bitCount_ = 0;
        bufferLen_ = 0;
    }

    void update(const void* data, std::size_t len) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bitCount_ += static_cast<std::uint64_t>(len) * 8;
        if (bufferLen_ > 0) {
            std::size_t take = std::min<std::size_t>(64 - bufferLen_, len);
            std::memcpy(buffer_.data() + bufferLen_, p, take);
            bufferLen_ += take; p += take; len -= take;
            if (bufferLen_ == 64) { transform(buffer_.data()); bufferLen_ = 0; }
        }
        while (len >= 64) { transform(p); p += 64; len -= 64; }
        if (len > 0) { std::memcpy(buffer_.data(), p, len); bufferLen_ = len; }
    }
    void update(std::string_view sv) { update(sv.data(), sv.size()); }

    std::array<std::uint8_t, 20> digest() {
        std::uint64_t bits = bitCount_;
        std::uint8_t  pad  = 0x80;
        update(&pad, 1);
        std::uint8_t zero = 0;
        while (bufferLen_ != 56) update(&zero, 1);
        // SHA-1 的长度字段是**大端**（与 MD5 相反，这是常见踩坑点）
        std::uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        std::memcpy(buffer_.data() + 56, lenBytes, 8);
        transform(buffer_.data());
        bufferLen_ = 0;

        std::array<std::uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[static_cast<std::size_t>(i * 4 + 0)] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)] >> 24);
            out[static_cast<std::size_t>(i * 4 + 1)] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)] >> 16);
            out[static_cast<std::size_t>(i * 4 + 2)] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)] >> 8);
            out[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(i)]);
        }
        return out;
    }

    std::string hex() {
        auto d = digest();
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (std::uint8_t b : d) ss << std::setw(2) << static_cast<int>(b);
        return ss.str();
    }

    static std::string hexOf(std::string_view data) { Sha1 s; s.update(data); return s.hex(); }
    static std::array<std::uint8_t, 20> raw(std::string_view data) {
        Sha1 s; s.update(data); return s.digest();
    }

private:
    static std::uint32_t rotl(std::uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void transform(const std::uint8_t* block) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {   // SHA-1 用大端读入
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24)
                 | (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16)
                 | (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8)
                 |  static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = state_[0], b = state_[1], c = state_[2],
                      d = state_[3], e = state_[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
            std::uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e;
    }

    std::array<std::uint32_t, 5>  state_{};
    std::array<std::uint8_t, 64>  buffer_{};
    std::size_t                   bufferLen_ = 0;
    std::uint64_t                 bitCount_  = 0;
};

// ---------------------------------------------------------------------------
// Base64（RFC 4648）
// ---------------------------------------------------------------------------
class Base64 {
public:
    static std::string encode(const std::uint8_t* data, std::size_t len) {
        static const char* T =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        std::size_t i = 0;
        while (i + 3 <= len) {
            std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16)
                            | (static_cast<std::uint32_t>(data[i + 1]) << 8)
                            |  static_cast<std::uint32_t>(data[i + 2]);
            out += T[(n >> 18) & 63];
            out += T[(n >> 12) & 63];
            out += T[(n >> 6)  & 63];
            out += T[n & 63];
            i += 3;
        }
        // 尾部补齐：1 个剩余字节补 "=="，2 个补 "="
        if (i + 1 == len) {
            std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
            out += T[(n >> 18) & 63];
            out += T[(n >> 12) & 63];
            out += "==";
        } else if (i + 2 == len) {
            std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16)
                            | (static_cast<std::uint32_t>(data[i + 1]) << 8);
            out += T[(n >> 18) & 63];
            out += T[(n >> 12) & 63];
            out += T[(n >> 6)  & 63];
            out += '=';
        }
        return out;
    }
    static std::string encode(std::string_view sv) {
        return encode(reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size());
    }
    template <std::size_t N>
    static std::string encode(const std::array<std::uint8_t, N>& a) {
        return encode(a.data(), N);
    }

    static std::string decode(std::string_view in) {
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;
        };
        std::string out;
        std::uint32_t buf = 0;
        int bits = 0;
        for (char c : in) {
            int v = val(c);
            if (v < 0) continue;                 // 跳过 '=' 和换行
            buf = (buf << 6) | static_cast<std::uint32_t>(v);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out += static_cast<char>((buf >> bits) & 0xFF);
            }
        }
        return out;
    }
};

// =============================================================================
// 第 14 题：WebSocket（RFC 6455）
// =============================================================================
namespace websocket {

// ---- 握手 ----
//
// 客户端发：
//   GET /chat HTTP/1.1
//   Upgrade: websocket
//   Connection: Upgrade
//   Sec-WebSocket-Key: <16 字节随机数的 base64>
//   Sec-WebSocket-Version: 13
//
// 服务端算：
//   accept = base64( SHA1( key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
//
// 那个魔数 GUID 是协议里硬编码的常量。它的作用不是安全（谁都知道它），
// 而是防止**缓存污染攻击**：让一个不懂 WebSocket 的中间代理无法凭空
// 造出正确的响应，从而确认对端真的理解这个协议。
inline constexpr std::string_view kMagicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline std::string computeAcceptKey(std::string_view clientKey) {
    std::string toHash;
    toHash.reserve(clientKey.size() + kMagicGuid.size());
    toHash.append(clientKey);
    toHash.append(kMagicGuid);
    return Base64::encode(Sha1::raw(toHash));
}

// 极简 HTTP 头解析（只为握手够用；完整实现见第 12 章）
inline std::unordered_map<std::string, std::string> parseHeaders(std::string_view raw) {
    std::unordered_map<std::string, std::string> h;
    std::size_t pos = raw.find("\r\n");
    if (pos == std::string_view::npos) return h;
    pos += 2;                                     // 跳过请求行
    while (pos < raw.size()) {
        std::size_t eol = raw.find("\r\n", pos);
        if (eol == std::string_view::npos || eol == pos) break;
        std::size_t colon = raw.find(':', pos);
        if (colon != std::string_view::npos && colon < eol) {
            std::string key(raw.substr(pos, colon - pos));
            std::size_t vs = colon + 1;
            while (vs < eol && (raw[vs] == ' ' || raw[vs] == '\t')) ++vs;
            std::string value(raw.substr(vs, eol - vs));
            // 头名大小写不敏感 -> 统一转小写做 key
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            h[key] = value;
        }
        pos = eol + 2;
    }
    return h;
}

// 生成握手响应。失败返回 nullopt（调用方应回 400）。
inline std::optional<std::string> makeHandshakeResponse(std::string_view request) {
    auto h = parseHeaders(request);

    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    auto it = h.find("upgrade");
    if (it == h.end() || lower(it->second) != "websocket") return std::nullopt;
    it = h.find("connection");
    if (it == h.end() || lower(it->second).find("upgrade") == std::string::npos) return std::nullopt;
    it = h.find("sec-websocket-key");
    if (it == h.end() || it->second.empty()) return std::nullopt;

    std::string accept = computeAcceptKey(it->second);
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
}

// ---- 帧 ----
//
//  0                   1                   2                   3
//  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
// +-+-+-+-+-------+-+-------------+-------------------------------+
// |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
// |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
// |N|V|V|V|       |S|             |                               |
// | |1|2|3|       |K|             |                               |
// +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
// |     Extended payload length continued, if payload len == 127  |
// + - - - - - - - - - - - - - - - +-------------------------------+
// |                               |Masking-key, if MASK set to 1  |
// +-------------------------------+-------------------------------+
// |                     Payload Data                              |
// +---------------------------------------------------------------+
//
// 三个必须知道的规则：
//   1. **客户端发给服务端的帧必须掩码**，服务端发给客户端的**必须不掩码**。
//      服务端收到未掩码的帧要按协议错误关闭（1002）。
//      掩码的目的不是加密，而是防止恶意 JS 构造出能欺骗中间代理的字节序列。
//   2. 长度是变长编码：0~125 直接放；126 表示后跟 2 字节；127 表示后跟 8 字节。
//   3. FIN=0 表示分片，后续帧的 opcode 必须是 0（continuation）。
enum class Opcode : std::uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

struct Frame {
    bool        fin    = true;
    bool        masked = false;
    Opcode      opcode = Opcode::Text;
    std::string payload;
};

// 编码。maskingKey 非 nullptr 时做掩码（客户端侧必须传）。
inline std::string encodeFrame(const Frame& f, const std::uint8_t* maskingKey = nullptr) {
    std::string out;
    std::uint8_t b0 = static_cast<std::uint8_t>((f.fin ? 0x80 : 0x00)
                        | static_cast<std::uint8_t>(f.opcode));
    out += static_cast<char>(b0);

    std::size_t   n    = f.payload.size();
    std::uint8_t  mask = maskingKey ? 0x80 : 0x00;
    if (n <= 125) {
        out += static_cast<char>(mask | static_cast<std::uint8_t>(n));
    } else if (n <= 0xFFFF) {
        out += static_cast<char>(mask | 126);
        out += static_cast<char>((n >> 8) & 0xFF);
        out += static_cast<char>(n & 0xFF);
    } else {
        out += static_cast<char>(mask | 127);
        for (int i = 7; i >= 0; --i) {
            out += static_cast<char>((static_cast<std::uint64_t>(n) >> (8 * i)) & 0xFF);
        }
    }

    if (maskingKey) {
        out.append(reinterpret_cast<const char*>(maskingKey), 4);
        for (std::size_t i = 0; i < n; ++i) {
            out += static_cast<char>(static_cast<std::uint8_t>(f.payload[i]) ^ maskingKey[i % 4]);
        }
    } else {
        out += f.payload;
    }
    return out;
}

enum class DecodeResult { Ok, NeedMore, ProtocolError };

// 解码一帧。consumed 返回消耗的字节数。
// 与长度前缀 codec 同理：必须能处理「数据不完整」的情况。
inline DecodeResult decodeFrame(std::string_view in, Frame* out, std::size_t* consumed) {
    *consumed = 0;
    if (in.size() < 2) return DecodeResult::NeedMore;

    auto u8 = [&](std::size_t i) { return static_cast<std::uint8_t>(in[i]); };

    bool         fin    = (u8(0) & 0x80) != 0;
    std::uint8_t rsv    = static_cast<std::uint8_t>(u8(0) & 0x70);
    auto         opcode = static_cast<Opcode>(u8(0) & 0x0F);
    bool         masked = (u8(1) & 0x80) != 0;
    std::uint64_t len   = u8(1) & 0x7F;

    if (rsv != 0) return DecodeResult::ProtocolError;   // 没协商扩展，RSV 必须为 0

    std::size_t pos = 2;
    if (len == 126) {
        if (in.size() < pos + 2) return DecodeResult::NeedMore;
        len = (static_cast<std::uint64_t>(u8(pos)) << 8) | u8(pos + 1);
        pos += 2;
    } else if (len == 127) {
        if (in.size() < pos + 8) return DecodeResult::NeedMore;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | u8(pos + static_cast<std::size_t>(i));
        pos += 8;
        if (len > (1ull << 40)) return DecodeResult::ProtocolError;   // 防内存炸弹
    }

    std::uint8_t key[4] = {0, 0, 0, 0};
    if (masked) {
        if (in.size() < pos + 4) return DecodeResult::NeedMore;
        for (int i = 0; i < 4; ++i) key[i] = u8(pos + static_cast<std::size_t>(i));
        pos += 4;
    }

    if (in.size() < pos + len) return DecodeResult::NeedMore;

    out->fin    = fin;
    out->masked = masked;
    out->opcode = opcode;
    out->payload.assign(in.data() + pos, static_cast<std::size_t>(len));
    if (masked) {
        for (std::size_t i = 0; i < out->payload.size(); ++i) {
            out->payload[i] = static_cast<char>(
                static_cast<std::uint8_t>(out->payload[i]) ^ key[i % 4]);
        }
    }
    *consumed = pos + static_cast<std::size_t>(len);
    return DecodeResult::Ok;
}

} // namespace websocket

// =============================================================================
// 第 11、12 题的协议定义（实现在 main.cpp，因为要用 Reactor）
// =============================================================================

// ---- 第 12 题：简易 RPC 协议 ----
//   [4 字节长度][4 字节请求ID][方法名\0][参数字节...]
//   长度字段覆盖它**之后**的全部内容（请求ID + 方法名 + 参数）
//
// 【为什么需要请求 ID】
//   客户端可以并发发出多个请求，服务端不保证按顺序回复
//   （比如方法 A 慢、方法 B 快，B 的响应会先到）。
//   靠请求 ID 才能把响应匹配到正确的调用方。这就是 RPC 与
//   「一问一答」协议的本质区别。
namespace rpc {

struct Request {
    std::uint32_t id = 0;
    std::string   method;
    std::string   args;
};
struct Response {
    std::uint32_t id = 0;
    bool          ok = true;
    std::string   payload;      // ok ? 返回值 : 错误信息
};

inline std::string encodeRequest(const Request& r) {
    std::string body;
    std::uint32_t idBe = ::htonl(r.id);
    body.append(reinterpret_cast<const char*>(&idBe), 4);
    body.append(r.method);
    body.push_back('\0');
    body.append(r.args);
    return LengthPrefixCodec::encode(body);
}

inline std::string encodeResponse(const Response& r) {
    std::string body;
    std::uint32_t idBe = ::htonl(r.id);
    body.append(reinterpret_cast<const char*>(&idBe), 4);
    body.push_back(r.ok ? '\1' : '\0');
    body.append(r.payload);
    return LengthPrefixCodec::encode(body);
}

// body 是 LengthPrefixCodec 解出来的一条完整消息
inline std::optional<Request> decodeRequest(const std::string& body) {
    if (body.size() < 5) return std::nullopt;
    std::uint32_t idBe = 0;
    std::memcpy(&idBe, body.data(), 4);
    std::size_t nul = body.find('\0', 4);
    if (nul == std::string::npos) return std::nullopt;
    Request r;
    r.id     = ::ntohl(idBe);
    r.method = body.substr(4, nul - 4);
    r.args   = body.substr(nul + 1);
    return r;
}

inline std::optional<Response> decodeResponse(const std::string& body) {
    if (body.size() < 5) return std::nullopt;
    std::uint32_t idBe = 0;
    std::memcpy(&idBe, body.data(), 4);
    Response r;
    r.id      = ::ntohl(idBe);
    r.ok      = body[4] != '\0';
    r.payload = body.substr(5);
    return r;
}

} // namespace rpc

// ---- 第 11 题：文件传输协议 ----
//   客户端请求：  "GET\0<文件名>\0<8字节起始offset(大端)>"
//   服务端响应头： "OK\0<8字节文件总长(大端)><32字节MD5十六进制>"  或  "ERR\0<原因>"
//   之后是从 offset 开始的原始字节流
//
// 【为什么要 64 位 offset】
//   要支持 >2 GB 的文件。用 int32 或 long（Windows 上是 32 位）都会溢出。
//   必须显式用 std::uint64_t / int64_t。这是跨平台大文件处理的第一个坑。
//
// 【断点续传怎么做】
//   客户端记录已收到的字节数，重连时把它作为 offset 发过去。
//   服务端 seek 到该位置继续发。协议里带上总长度和整文件的 MD5，
//   客户端收完后校验 —— 这样即使中间断了很多次也能确认最终文件正确。
namespace filexfer {

inline std::string encodeGetRequest(std::string_view filename, std::uint64_t offset) {
    std::string body = "GET";
    body.push_back('\0');
    body.append(filename);
    body.push_back('\0');
    for (int i = 7; i >= 0; --i) {
        body.push_back(static_cast<char>((offset >> (8 * i)) & 0xFF));
    }
    return LengthPrefixCodec::encode(body);
}

struct GetRequest {
    std::string   filename;
    std::uint64_t offset = 0;
};

inline std::optional<GetRequest> decodeGetRequest(const std::string& body) {
    std::size_t p1 = body.find('\0');
    if (p1 == std::string::npos || body.substr(0, p1) != "GET") return std::nullopt;
    std::size_t p2 = body.find('\0', p1 + 1);
    if (p2 == std::string::npos) return std::nullopt;
    if (body.size() < p2 + 1 + 8) return std::nullopt;

    GetRequest r;
    r.filename = body.substr(p1 + 1, p2 - p1 - 1);
    r.offset = 0;
    for (int i = 0; i < 8; ++i) {
        r.offset = (r.offset << 8)
                 | static_cast<std::uint8_t>(body[p2 + 1 + static_cast<std::size_t>(i)]);
    }
    return r;
}

inline std::string encodeOkHeader(std::uint64_t totalSize, const std::string& md5hex) {
    std::string body = "OK";
    body.push_back('\0');
    for (int i = 7; i >= 0; --i) {
        body.push_back(static_cast<char>((totalSize >> (8 * i)) & 0xFF));
    }
    body.append(md5hex);              // 32 个十六进制字符
    return LengthPrefixCodec::encode(body);
}

struct OkHeader {
    std::uint64_t totalSize = 0;
    std::string   md5hex;
};

inline std::optional<OkHeader> decodeOkHeader(const std::string& body) {
    std::size_t p = body.find('\0');
    if (p == std::string::npos || body.substr(0, p) != "OK") return std::nullopt;
    if (body.size() < p + 1 + 8 + 32) return std::nullopt;
    OkHeader h;
    for (int i = 0; i < 8; ++i) {
        h.totalSize = (h.totalSize << 8)
                    | static_cast<std::uint8_t>(body[p + 1 + static_cast<std::size_t>(i)]);
    }
    h.md5hex = body.substr(p + 1 + 8, 32);
    return h;
}

} // namespace filexfer

} // namespace sol
