# 第 18~19 章 · 练习题参考答案

▶ 对应程序：`ch18_solutions`（第 1~8 题）、`ch19_net_solutions`（第 9~14 题）

练习题本身在**附录 B**。这里讲每题的考点和易错处；完整代码在源文件里，每题都带自检，运行后能看到逐项通过情况。

「参考答案」的意思是：这是一种可用的写法，不是唯一写法。每题都标了**【常见错法】**—— 那部分往往比答案本身更有价值。

---

## B.1 语法与 STL

### 第 1 题 · SafeVector\<T\>

**考点**：组合优于继承、完美转发、五法则里的"只要移动不要拷贝"。

```cpp
template <typename T>
class SafeVector {
    std::vector<T> data_;          // 组合，不是继承
public:
    SafeVector(const SafeVector&)            = delete;   // 这两行就是「不可拷贝」的全部实现
    SafeVector& operator=(const SafeVector&) = delete;
    SafeVector(SafeVector&&) noexcept        = default;

    T&       operator[](size_type i)       { if (i >= data_.size()) throwOutOfRange(i); return data_[i]; }
    const T& operator[](size_type i) const { if (i >= data_.size()) throwOutOfRange(i); return data_[i]; }

    template <typename... Args>
    T& emplace_back(Args&&... args) { return data_.emplace_back(std::forward<Args>(args)...); }
};
```

**常见错法**：

- ❌ **继承 `std::vector<T>`**。标准容器的析构函数不是 `virtual`，通过基类指针删除派生对象是 UB（第 13 章 3.1），而且会继承到一堆你不想暴露的接口
- ❌ 只写非 const 版本的 `operator[]` → const 对象上没法读
- ❌ `emplace_back(Args... args)` 按值接参 → 多一次拷贝

只要提供了 `begin()`/`end()`，标准算法就直接可用 —— `std::sort(v.begin(), v.end())` 无需任何额外适配。这就是第 15 章讲的迭代器解耦的好处。

### 第 2 题 · join

**考点**：折叠表达式、`sizeof...` 处理"最后一个不加分隔符"。

两种写法：

```cpp
// 写法 A：折叠 + 索引计数
template <typename... Ts>
std::string join(const char* sep, Ts&&... args) {
    std::ostringstream oss;
    std::size_t index = 0;
    constexpr std::size_t count = sizeof...(Ts);
    ((oss << std::forward<Ts>(args), (++index < count ? oss << sep : oss)), ...);
    return oss.str();
}

// 写法 B：分隔符加在「除第一个之外」的每个元素前面
template <typename First, typename... Rest>
std::string join2(const char* sep, First&& first, Rest&&... rest) {
    std::ostringstream oss;
    oss << std::forward<First>(first);
    ((oss << sep << std::forward<Rest>(rest)), ...);   // 空包时整个折叠为空
    return oss.str();
}
inline std::string join2(const char*) { return {}; }   // 零参数重载
```

**常见错法**：

- ❌ 每个元素后面都加分隔符再 pop 掉最后一个 → **空参数包时崩溃**
- ❌ 用 `std::string` 累加 → O(n²) 拷贝，用 `ostringstream` 更合适
- ❌ 用递归展开（能做，但代码长、编译慢）

### 第 3 题 · variant 实现 JSON

**考点**：`variant` 里怎么放**递归类型**。

这是本题唯一的难点。直接写编译不过：

```cpp
// ❌ 编译错误：Json 在定义自己的时候还不完整
using Json = std::variant<..., std::vector<Json>, std::map<std::string, Json>>;
```

解法是前置声明 + 包一层：

```cpp
struct Json;                        // 此时是不完整类型

using JsonValue = std::variant<
    std::nullptr_t, bool, double, std::string,
    std::vector<Json>,                    // vector 允许元素是不完整类型
    std::map<std::string, Json>           // map 同上（C++17 起标准明确保证这两个）
>;

struct Json { JsonValue v; };
```

序列化用 `std::visit` + `overloaded` 惯用法，递归处理数组和对象。

两个容易忘的细节：

- **字符串必须转义** `"` `\` 和控制字符，否则输出的不是合法 JSON
- **整数值的 double 要输出成整数形态**：`3.0` 应该输出 `3` 而不是 `3.000000`

### 第 4 题 · 概念约束的快排

**考点**：C++20 concepts 约束算法 + 快排的三个工程细节。

```cpp
template <std::random_access_iterator It, typename Comp = std::ranges::less>
    requires std::sortable<It, Comp>
void my_quick_sort(It first, It last, Comp comp = {});
```

三个细节缺一不可（这也是第 15 章 introsort 那节讲过的）：

1. **三点取中选 pivot** —— 直接取首元素的话，有序输入会退化成 O(n²)
2. **小区间（≤16）切插入排序** —— 元素少时它更快，且对"几乎有序"极快
3. **只递归较小的一半，循环处理较大的一半** —— 栈深度从最坏 O(n) 降到 O(log n)

自检里专门测了三种最坏输入：已排序、完全逆序、大量重复值。第三种最容易让分区逻辑死循环。

**concept 的实际价值**：传 `std::list` 的迭代器进去，报的是"不满足 `random_access_iterator`"一行人话，而不是几百行模板实例化错误。

### 第 5 题 · O(1) LRU 缓存

**考点**：`std::list` + `std::unordered_map` 的组合。

```cpp
std::list<std::pair<K, V>>                                  order_;   // 队首 = 最近使用
std::unordered_map<K, typename std::list<Entry>::iterator>  index_;
```

`get` 命中后用 `splice` 把节点移到队首：

```cpp
order_.splice(order_.begin(), order_, it->second);   // O(1)，且迭代器保持有效
```

**这是全书唯一一个"`list` 明显优于 `vector`"的场景**：`splice` 是 O(1) 且**不使迭代器失效**，所以 map 里存的迭代器在无数次重排后依然有效。换成 `vector` 或 `deque` 都做不到（第 15 章 6）。

**常见错法**：

- ❌ 用 `vector` 存访问顺序 → 移到队首是 O(n)
- ❌ map 里存下标而不是迭代器 → 链表变动后下标全错
- ❌ **淘汰时忘了从 map 里也删掉** → map 无限增长（这就是内存泄漏）
- ❌ `get` 命中后忘记更新顺序 → 退化成 FIFO，不是 LRU

自检里跑了 20 万次 `put`/`get`，验证容量恒定（淘汰正常）且耗时线性（单次真的是 O(1)）。

---

## B.2 设计模式

### 第 6 题 · 线程安全事件总线

**考点**：`type_index` 按类型分发 + `weak_ptr` 观察者 + 不持锁调用外部代码。

三个设计要点：

**1. 用 `std::type_index` 做 key**，不要用 `typeid(E).name()`

`name()` 的格式跨编译器不同，而且理论上可能重名。`type_index` 包装了 `type_info`，可哈希可比较，跨编译器行为稳定。

**2. 持锁只拷贝订阅者列表，调用 handler 时不持锁**

```cpp
std::vector<std::pair<Id, HandlerFn>> snapshot;
{
    std::shared_lock lk(mutex_);
    snapshot = handlers_[type];      // 拷一份
}                                     // 锁在这里释放
for (auto& [id, fn] : snapshot) {
    if (!fn(&event)) dead.push_back(id);   // 不持锁调用
}
```

持锁调用的话，handler 里再 `subscribe`/`publish` 就自己死锁了。自检里专门有一项验证这个可重入性。**"不要在持锁时调用外部代码"是并发编程的通用铁律**（第 13 章 5.3、第 16 章 10 都在讲同一件事）。

**3. handler 返回 `bool` 表示自己是否还有效**

订阅者用 `weak_ptr` 持有。`lock()` 失败就返回 `false`，总线在本轮 publish 之后统一回收。这样订阅者对象销毁后不需要手动退订。

**常见错法**：

- ❌ 用 `shared_ptr` 持有订阅者 → 订阅者永远不释放
- ❌ 只在 publish 时清理 → 从不 publish 的事件类型会无限累积失效订阅

### 第 7 题 · 类型擦除的 AnyCallable

**考点**：类型擦除的标准结构。

```cpp
struct Concept {                                          // 抽象接口
    virtual ~Concept() = default;                         // 必须 virtual
    virtual R invoke(Args&&...) const = 0;
    virtual std::unique_ptr<Concept> clone() const = 0;    // 支持拷贝的关键
};

template <typename F>
struct Model final : Concept {                            // 每个 F 一份具体实现
    F fn;
    R invoke(Args&&... args) const override { return fn(std::forward<Args>(args)...); }
    std::unique_ptr<Concept> clone() const override { return std::make_unique<Model>(fn); }
};

std::unique_ptr<Concept> impl_;                           // 持有基类指针
```

**三件套：virtual 析构、invoke、clone。** 少了 clone 就没法支持拷贝（`unique_ptr` 不可拷贝）。

**一个真实踩到的坑**：构造函数的类型变换必须用 `std::decay_t`，不能用 `std::remove_cvref_t`：

```cpp
template <typename F>
    requires std::is_invocable_r_v<R, std::decay_t<F>&, Args...>
AnyCallable(F&& f) : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}
```

传自由函数时 `F` 推导为 `int(&)(int)`，`remove_cvref_t` 得到的是**函数类型** `int(int)` —— 而类的成员不能是函数类型（MSVC 报 C2207）。`decay_t` 会把函数衰减成函数指针，这正是"按值存一份"需要的语义。

**与 `std::function` 的差异**：本实现每次都堆分配；标准库有**小对象优化（SBO）**，小的可调用对象直接存在对象内部。想加 SBO 就在类里放一个 `alignas` 的 char buffer，`sizeof(F)` 足够小时 placement new 进去。

### 第 8 题 · Handler 装饰器

**考点**：装饰器模式的函数式写法。

关键是**装饰器的签名必须是 `Handler -> Handler`**，这保证了可以无限叠加：

```cpp
using Handler   = std::function<HttpResponse(const HttpRequest&)>;
using Decorator = std::function<Handler(Handler)>;

Decorator withTiming(std::vector<std::string>* log) {
    return [log](Handler next) -> Handler {
        return [next, log](const HttpRequest& req) -> HttpResponse {
            auto t0 = std::chrono::steady_clock::now();     // 测耗时必须用 steady_clock
            HttpResponse resp = next(req);
            resp.headers["X-Elapsed-Us"] = ...;
            return resp;
        };
    };
}
```

**顺序决定语义**：

- `{withErrorHandling, withTiming}` → 异常在外层，能捕获计时器内的异常，但计时不包含异常处理本身
- `{withTiming, withErrorHandling}` → 计时在外层，能测到异常处理的耗时，但异常若从计时器抛出就没人管了

一般把「异常处理」和「日志」放最外层，「鉴权」「限流」放中层，业务在最内。这和第 16 章 HTTP 中间件是同一个思路。

**常见错法**：

- ❌ 用继承做装饰 → 每加一种装饰就要一个类，且无法动态组合
- ❌ 装饰器改变了函数签名 → 无法叠加
- ❌ 计时用 `system_clock` → 它会被 NTP 调整

---

## B.3 网络编程

第 11、12 题的服务端**直接用第 16 章的 Reactor 库**实现 —— 既是答案，也顺便证明那个库真的能用来干活。

### 第 9 题 · 长度前缀协议

**考点不是编码，而是解码器必须可重入。**

协议 `[4 字节大端长度][消息体]` 本身很简单。真正的考点是：**每次只喂进来几个字节也要能正确工作**，因为 4 字节的长度头本身也会被 TCP 拆开。

```cpp
bool feed(const char* data, std::size_t len, const MessageCallback& onMessage) {
    buffer_.append(data, len);
    while (true) {                                    // while 而不是 if
        if (buffer_.size() < 4) return true;          // 连长度字段都不完整
        std::uint32_t bodyLen = ::ntohl(读出前 4 字节);
        if (bodyLen > maxLength_) return false;       // 内存炸弹防护
        if (buffer_.size() < 4 + bodyLen) return true; // 消息体还没收全
        onMessage(取出这一条);
    }
}
```

**常见错法**：

- ❌ 假设一次 `recv` 就能拿到完整的 4 字节头
- ❌ **不校验长度上限** → 对方发一个 `0xFFFFFFFF`，你就去 `resize` 4 GB
- ❌ 用主机字节序写长度 → 跨平台/跨语言立刻出错
- ❌ 解出一条消息就 `return` → 粘包时剩下的消息要等下一次事件才处理，延迟翻倍

自检覆盖：逐字节喂入、3 条粘包、切在消息中间、空消息体、4 GB 长度攻击、500 条消息以随机 1~64 字节分片喂入。

### 第 10 题 · 定时器堆与超时踢出

**考点**：小顶堆 + `steady_clock` + 超时值怎么喂给 `poll`。

```cpp
int nextTimeoutMs(int defaultMs = 10000) {
    if (heap_.empty()) return defaultMs;
    auto remain = ...(heap_.top().when - Clock::now());
    if (remain <= 0) return 0;        // 已过期 -> 返回 0，让 poll 立刻返回
    return std::min(remain, defaultMs);
}
```

**两个必踩的坑**：

1. **必须用 `steady_clock`**。`system_clock` 会被 NTP 或用户改时间往回调，一次校时就能让所有定时器失效或提前触发
2. **已过期要返回 0 而不是负数** —— 负数在 `poll` 里表示"无限等待"，会把整个事件循环挂死

`std::priority_queue` 不支持删除中间元素，所以取消用**惰性删除**：记下 id，弹出时检查并跳过。

还有一个细节：`tick()` 必须**先把到期项全部取出，再逐个调用回调**。因为回调里可能再 `addTimer`（周期性心跳的标准写法），边遍历边加会陷入死循环。

**实战部分**用 Reactor 库搭了一个"800ms 没消息就踢下线"的服务器，验证持续活跃的连接不被踢、沉默的连接被踢。注意那里的 `lastActive` map **不需要加锁** —— 连接归属单一 loop 线程，这是 one loop per thread 的直接收益。

### 第 11 题 · 文件传输

**考点**：64 位 offset、断点续传、整文件校验。

协议：

```
请求：  "GET\0<文件名>\0<8字节起始offset(大端)>"
响应头："OK\0<8字节文件总长(大端)><32字节MD5十六进制>"
之后：  从 offset 开始的原始字节流
```

**三个必踩的坑**：

1. **offset 必须用 `uint64_t`/`int64_t`**。Windows 上 `long` 是 32 位，用它做 offset 在 2 GB 处**静默溢出**。自检专门测了 `0x7FFFFFFF`、`0x80000000`、`0xFFFFFFFF`、`0x100000000`、`0xFFFFFFFFFF` 五个边界值的编解码往返
2. `seekg` 的参数类型是 `std::streamoff`，别传 `int`
3. **别把整个文件读进内存再 send**。应该配合 `WriteCompleteCallback` 分块发：发完一块才读下一块，这样内存占用恒定

还有一个**协议切换的陷阱**，客户端侧很容易中招：响应先是一条长度前缀的头，之后是裸字节流。所以头必须**精确读取**（4 字节长度 + 恰好那么多字节），**不能用 `LengthPrefixCodec`** —— 它会把头之后的文件字节当成"下一条消息"缓存起来，导致文件少一截，最后 MD5 校验失败。

HTTP 的 chunked → raw、WebSocket 的 Upgrade 都有同样的问题：**同一连接上协议中途切换时，前一个解码器的残留缓冲必须交还给后一个处理者**。

自检流程：下载 1 MB 后主动断开 → 从断点续传 → 校验最终文件大小和 MD5 与源文件完全一致。

### 第 12 题 · 简易 RPC

**考点**：请求 ID 为什么必需。

协议：`[4 字节长度][4 字节请求ID][方法名\0][参数]`

客户端可以并发发出多个请求，而服务端**不保证按顺序回复**（方法 A 慢、方法 B 快，B 的响应会先到）。靠请求 ID 才能把响应匹配到正确的调用方。**这就是 RPC 与"一问一答"协议的本质区别。**

自检里专门构造了这个场景：先发一个耗时 120ms 的 `slow`，紧接着发 3 个瞬时的 `echo`。响应必然乱序返回，但每个都能靠 ID 对上号。

服务端用第 16 章的 Reactor 库，每个连接一个独立的 `LengthPrefixCodec` 挂在 `context` 上：

```cpp
server.setConnectionCallback([](const TcpConnectionPtr& c) {
    if (c->connected()) c->setContext(std::make_shared<LengthPrefixCodec>());
});
server.setMessageCallback([this](const TcpConnectionPtr& c, Buffer* buf) {
    auto codec = c->context<LengthPrefixCodec>();
    if (!codec->feed(buf->retrieveAllAsString(), [&](std::string&& body) { handleOne(c, body); }))
        c->forceClose();                              // 协议错误
});
```

连接归属单一 loop 线程，所以这个 codec **不需要加锁**。

自检最后跑了 8 客户端 × 50 次调用 = 400 次并发调用，全部正确匹配。

### 第 13 题 · HTTP 改事件驱动（☆）

**这题的答案就是第 16 章本身。** 改造清单：

1. 所有 fd 设为非阻塞
2. 每连接一个应用层**读**缓冲（`Buffer` + `readv` + 栈上 extrabuf）
3. 每连接一个应用层**写**缓冲（`outputBuffer_` + `enableWriting()`）
4. **写完立刻 `disableWriting()`**，否则 LT 模式 CPU 打满
5. HTTP 解析器必须可重入
6. 连接生命周期用 `shared_ptr` + `Channel::tie`

第 3、4 条是本题的真正难点（题面也这么说）。还要注意 `sendInLoop` 里"输出缓冲区非空时必须排队，不能直接写"这个**保序判断** —— 漏了它会导致数据顺序错乱。

### 第 14 题 · WebSocket

**握手**：`accept = base64(SHA1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))`

自检用的是 RFC 6455 第 1.3 节的官方示例：key `dGhlIHNhbXBsZSBub25jZQ==` → accept `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`。

**帧格式**的三个硬规则：

1. **客户端发的帧必须掩码，服务端发的必须不掩码。** 服务端收到未掩码帧要以 1002（协议错误）关闭连接
2. **长度是变长编码**：0~125 直接放；126 表示后跟 2 字节；127 表示后跟 8 字节。自检测了 0/125/126/1000/65535/65536/200000 七个值，覆盖三条编码路径
3. **FIN=0 表示分片**，后续帧的 opcode 必须是 0（continuation）

两个"为什么"，理解了就不会记错：

- **掩码不是加密**（密钥就在帧里明文传）。它的目的是防止恶意 JS 构造出能欺骗中间代理的字节序列（缓存投毒）
- **魔数 GUID 同理** —— 不为安全（谁都知道它），只为确认对端真的实现了 WebSocket，而不是某个傻转发的代理

解码器和第 9 题一样必须可重入。自检验证了截断到 0/1/2/5/9/n-1 字节时都正确返回 `NeedMore` 而不是崩溃。

### 附带实现的摘要算法

C++ 标准库**没有**任何加密摘要算法，所以第 11、14 题需要自己实现 MD5、SHA-1、Base64。三者都对着标准测试向量验证：

| 算法 | 测试向量来源 |
|---|---|
| MD5 | RFC 1321 附录 A.5（6 组）+ 分块喂入一致性 |
| SHA-1 | RFC 3174（3 组） |
| Base64 | RFC 4648 第 10 节（7 组）+ 解码往返 |

**一个易错的差异**：MD5 的长度字段是**小端**，SHA-1 是**大端**；两者读入 32 位字的字节序也相反。照着一个改另一个，这里最容易出错。

> 生产环境请用 OpenSSL / libsodium。这里的实现没有做侧信道防护，且 MD5/SHA-1 早已不适合安全用途（碰撞攻击已实用化）。用它们做**完整性校验**仍然可以 —— 本章就是这个用途。

---

## 关于 CI

这两章（以及全部 19 章）都跑在 GitHub Actions 上，配置见 `.github/workflows/ci.yml`：

| Job | 作用 |
|---|---|
| Linux · gcc/clang · Debug/Release | 编译 + 运行全部章节 + 驱动 select/poll/**epoll** 三种模式 |
| Linux · Clang · ASan+UBSan | 内存错误与未定义行为检查 |
| Windows · MSVC · Debug/Release | 验证 Winsock 兼容层 |
| 文档生成 | 确认 PDF 能重新生成且章节齐全 |

**Linux job 的首要价值是验证 epoll。** `EpollPoller`、`eventfd`、`readv`、`SO_REUSEPORT` 这些代码路径在 Windows 上连编译都不会发生 —— 只有 CI 能证明它们真的可用。CI 里有一步专门 grep 服务器日志里的 `Poller = epoll`，确认没有静默回退到 `PollPoller`。

### 第一次跑 CI 踩的五个坑

接 CI 的过程本身很有教学价值 —— 这五个问题在本地全都测不出来，值得单独记一下。

**① 编译失败被管道吞掉**

```bash
cmake --build build -j$(nproc) 2>&1 | tee build.log      # 错
```

管道的退出码是**最后一个命令**（`tee`）的，编译失败返回 0。于是后续步骤在
缺少可执行文件的情况下继续跑，报出「Reactor 没有走 epoll」这种完全误导性的
错误 —— 真正的编译错误一个字都看不到。

```bash
if ! cmake --build build -j$(nproc) > build.log 2>&1; then ... fi   # 对
```

（或者 `set -o pipefail`。）

**② 裸 `wait` 会等待后台的服务器进程**

```bash
"$BIN/ch12_http_server" 18080 &            # 永不退出
for i in $(seq 1 50); do curl ... & done
wait                                        # 错：也在等服务器
```

这一步挂到 job 30 分钟超时，日志里毫无线索。正确做法是收集要等的 PID：

```bash
pids=""
for i in $(seq 1 50); do curl ... & pids="$pids $!"; done
for pid in $pids; do wait "$pid"; done
```

顺带说明：修好之后实测 ch12 的线程池在 605 ms 内完成了全部 50 个并发请求 ——
一开始怀疑是它的并发上限，结果是测试脚本自己的问题。**先怀疑测量工具，再怀疑被测对象。**

**③ `__has_include` 不等于特性可用**

clang + libstdc++ 上 `<expected>` 头文件存在，但内部还有一层
`#if __cplusplus > 202002L` 的门；门没过时头文件包含成功却什么都不声明，
于是编译在**使用处**才报 `no template named 'expected' in namespace 'std'`。

必须两级判断：`__has_include` 决定能不能 include，`__cpp_lib_expected`
决定特性是否真的可用。

**④ Windows runner 的 stdout 不是 UTF-8**

GitHub 的 Windows runner 上 Python 的 stdout 编码是 **cp1252**，打印中文直接
`UnicodeEncodeError` 崩溃 —— 而且崩在日志函数里，比真正要报告的失败更早，
堆栈完全误导。本地 Git Bash 是 GBK 能编码中文，所以测不出来。

```python
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
```

**⑤ MSVC 的传递包含惯坏了你**

MSVC 的标准库头文件之间互相包含很多，所以缺 `#include` 也能编译；GCC/libstdc++
不会。首轮 CI 报出 `std::exchange` 缺 `<utility>`、`std::uint32_t` 缺 `<cstdint>`。

与其逐个等 CI 报，写了个按「符号 → 所需头」规则扫全仓库的审计脚本（先剥掉注释和
字符串字面量避免误判），一次补齐了 27 个文件、72 个 include。

> 这五个坑有个共同点：**它们都只在「与本地不同的环境」里出现**。
> 这就是 CI 的价值 —— 不是替你跑测试，而是替你在你没有的环境里跑测试。

**Sanitizer job 有个值得一提的细节**：第 13、14 章是「反面教材」章节，代码里刻意保留了真实的内存错误（非虚析构基类删除、`shared_ptr` 循环引用泄漏）。所以这两章单独放宽 `detect_leaks` 和 `new_delete_type_mismatch` 两项，其余检查全开 —— 如果它们有**非故意**的错误，照样会被抓出来。
