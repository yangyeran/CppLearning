# 第 7 章 · 设计模式（现代 C++ 版）

▶ 对应程序：`ch07_patterns`

**先说结论**：GoF 那 23 个模式是在 1994 年、面向 C++98/Java 总结的。
现代 C++ 里有一批模式已经被语言特性直接吃掉了。这一章讲**现在还值得用的**，
以及**每个模式的适用边界**。

```
C++ 专属惯用法（比 GoF 更常用，优先掌握）
   RAII  ·  Pimpl  ·  CRTP  ·  类型擦除  ·  策略模板

创建型     单例   工厂   建造者   原型
结构型     适配器  装饰器  代理  外观  桥接  组合  享元
行为型     策略  观察者  命令  模板方法  状态  访问者  责任链  迭代器  中介者  备忘录
```

---

## 7.1 RAII —— C++ 最重要的模式

**资源获取即初始化**：构造函数拿资源，析构函数还资源。
因为析构**一定**会被调用（正常返回、提前 return、抛异常都一样），所以永不泄漏。

```cpp
class FileHandle {
    std::FILE* f_ = nullptr;
public:
    FileHandle(const char* p, const char* m) : f_(std::fopen(p, m)) {
        if (!f_) throw std::runtime_error("打不开");
    }
    ~FileHandle() { if (f_) std::fclose(f_); }

    FileHandle(FileHandle&& o) noexcept : f_(std::exchange(o.f_, nullptr)) {}
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) { if (f_) std::fclose(f_); f_ = std::exchange(o.f_, nullptr); }
        return *this;
    }
    FileHandle(const FileHandle&)            = delete;   // 资源类通常禁止拷贝
    FileHandle& operator=(const FileHandle&) = delete;
};
```

**通用 ScopeGuard**

```cpp
template <typename F>
class ScopeGuard {
    F f_; bool active_ = true;
public:
    explicit ScopeGuard(F f) : f_(std::move(f)) {}
    ~ScopeGuard() { if (active_) f_(); }
    void dismiss() { active_ = false; }        // 成功了就取消回滚
};
template <typename F> ScopeGuard(F) -> ScopeGuard<F>;

// 用法：事务回滚
void transfer() {
    begin_transaction();
    ScopeGuard rollback{[]{ rollback_transaction(); }};
    do_work();                                  // 抛异常就自动回滚
    commit();
    rollback.dismiss();                         // 成功了，取消回滚
}
```

**【用在哪】** 文件、锁、socket、数据库连接、GPU 句柄、事务、临时状态恢复、
性能计时器。标准库里的例子：`unique_ptr` / `lock_guard` / `ifstream` / `jthread`。

---

## 7.2 Pimpl —— 隔断编译依赖

```cpp
// widget.h —— 干净，不 #include 任何重量级依赖
class Widget {
public:
    Widget();
    ~Widget();                          // ★ 必须在 .cpp 定义
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;
    void do_work();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;        // 头文件里只有一个指针
};

// widget.cpp
#include <heavy_library.h>
struct Widget::Impl { HeavyType data; void work(); };
Widget::Widget() : impl_(std::make_unique<Impl>()) {}
Widget::~Widget() = default;            // ★ 此处 Impl 是完整类型
void Widget::do_work() { impl_->work(); }
```

**好处**
1. 头文件不 include 重依赖 → **全项目编译快很多**
2. 改实现不触发下游重编译
3. **ABI 稳定**（类大小永远是一个指针）—— 做动态库/SDK 必备

**代价**：一次额外堆分配 + 一次间接访问。热点小对象别用。

**【坑】析构函数必须在 .cpp 里定义**（哪怕是 `= default`）。
否则在头文件处 `unique_ptr<Impl>` 析构时 `Impl` 还是不完整类型，编译报错。

---

## 7.3 CRTP 与 类型擦除

### CRTP（奇异递归模板模式）—— 静态多态

```cpp
template <typename Derived>
class Printable {
public:
    void print() const {
        std::cout << static_cast<const Derived&>(*this).to_string();
    }
};
class Money : public Printable<Money> {
public:
    std::string to_string() const { return "..."; }
};
```

基类通过 `static_cast<Derived&>(*this)` 调到派生类，**全部编译期解析，可内联，零虚函数开销**。

**【用在哪】** 给一堆类批量加公共功能（mixin）、表达式模板、单例基类、
统计对象数量。

**注意**：C++20 有了 `<=>` 和 concepts 后，CRTP 的一部分传统用途（比较运算符 mixin、
接口约束）已经被取代了。

### 类型擦除 —— 值语义 + 多态 + 不需要继承

```cpp
class Drawable {
    struct Concept {
        virtual ~Concept() = default;
        virtual void draw() const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;
    };
    template <typename T>
    struct Model final : Concept {
        T obj;
        explicit Model(T o) : obj(std::move(o)) {}
        void draw() const override { obj.draw(); }        // 鸭子类型
        std::unique_ptr<Concept> clone() const override {
            return std::make_unique<Model>(obj);
        }
    };
    std::unique_ptr<Concept> self_;
public:
    template <typename T>
    Drawable(T obj) : self_(std::make_unique<Model<T>>(std::move(obj))) {}
    Drawable(const Drawable& o) : self_(o.self_->clone()) {}
    void draw() const { self_->draw(); }
};

struct Cat { void draw() const; };     // ← 不继承任何东西
struct Dog { void draw() const; };

std::vector<Drawable> zoo{Cat{}, Dog{}};
auto copy = zoo;                       // ✅ 可以整体拷贝（虚继承做不到）
```

**【用在哪】** `std::function` / `std::any` 就是这么实现的。
插件系统、想要「多态 + 值语义容器」时。

---

## 7.4 创建型

### 单例（Meyers Singleton）

```cpp
class Logger {
public:
    static Logger& instance() {
        static Logger inst;          // C++11 起局部静态初始化是【线程安全】的
        return inst;
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
private:
    Logger() = default;
};
```

**警告**：单例是全局状态。它会让单元测试变难（没法替换成 mock）、
隐藏依赖关系、有静态析构顺序问题。
**能用依赖注入（把对象当参数传进去）就别用单例。**

### 工厂（注册式）—— 加新类型不用改工厂代码

```cpp
class ShapeFactory {
public:
    using Creator = std::function<std::unique_ptr<Shape>(double)>;
    static ShapeFactory& instance() { static ShapeFactory f; return f; }
    void register_type(std::string k, Creator c) { creators_[std::move(k)] = std::move(c); }
    std::unique_ptr<Shape> create(const std::string& k, double p) const {
        auto it = creators_.find(k);
        return it == creators_.end() ? nullptr : it->second(p);
    }
private:
    std::map<std::string, Creator> creators_;
};

// 自动注册：静态对象在 main 之前构造
template <typename T>
struct Registrar {
    explicit Registrar(std::string k) {
        ShapeFactory::instance().register_type(std::move(k),
            [](double p) { return std::make_unique<T>(p); });
    }
};
static Registrar<Circle> reg_circle{"circle"};
```

加一个新形状只需要写一行 `static Registrar<Triangle> reg{"triangle"};`，
**工厂本身一行都不用改** —— 这就是开闭原则。

**【用在哪】** 插件、配置驱动的对象创建、反序列化、消息分发。

### 建造者

```cpp
auto req = HttpRequest::Builder{}
               .url("https://api.example.com")
               .method("POST")
               .header("Content-Type", "application/json")
               .timeout(3000)
               .build();
```

**【用在哪】** 构造参数多、大部分可选、且构造后不可变的对象。

**现代替代**：参数少时 C++20 指定初始化器更轻量：

```cpp
struct Options { int retries = 3; bool verbose = false; };
configure({.retries = 5});
```

---

## 7.5 结构型

| 模式 | 一句话 | 【用在哪】 |
|------|--------|-----------|
| **适配器** | 包一层，让老接口符合新接口 | 对接遗留代码 / 第三方库。标准库的 `stack`/`queue` 就是 `deque` 的适配器 |
| **装饰器** | 层层包裹，动态添加职责 | IO 流（缓冲/加密/压缩）、HTTP 中间件、UI 控件加边框滚动条 |
| **代理** | 替身，控制对真实对象的访问 | 延迟加载、远程代理(RPC stub)、权限检查、缓存。**智能指针就是所有权代理** |
| **外观** | 一个简单接口包住一堆子系统 | SDK 对外 API、复杂库的便捷封装 |
| **桥接** | 抽象和实现两条继承线各自演化 | 避免 N×M 类爆炸。3 种形状 × 4 种渲染器：继承要 12 个类，桥接只要 7 个 |
| **组合** | 树形结构，叶子和容器一视同仁 | 文件系统、UI 控件树、AST、组织架构 |
| **享元** | 共享不变的内部状态 | 字体渲染、游戏里的树/草模型、字符串驻留、连接池 |

**装饰器的调用链**

```cpp
auto src = std::make_unique<Encrypted>(
               std::make_unique<Compressed>(
                   std::make_unique<FileSource>()));
src->read();
// 执行顺序: Encrypted::read -> Compressed::read -> FileSource::read
//           然后反向返回，每层做自己的解压/解密
```

---

## 7.6 策略 —— 三种实现方式，怎么选

```cpp
// (a) 虚函数：运行期可切换，有间接调用开销
class Compressor { public: virtual std::string compress(std::string_view) = 0; };
archiver.set_policy(std::make_unique<GzipPolicy>());

// (b) std::function：最灵活，可以直接塞 lambda
std::function<std::string(std::string_view)> policy = [](auto s) { return ...; };

// (c) 模板策略：编译期确定，可内联，零开销
template <typename Policy>
class FastArchiver {
public:
    auto archive(std::string_view s) { return Policy::compress(s); }
};
FastArchiver<Gzip> a;
```

| 场景 | 选哪个 |
|------|--------|
| 运行期要换（用户配置、插件） | 虚函数 或 `std::function` |
| 编译期就定死，且在热点路径 | 模板策略 |
| 只是一个小回调 | 直接传 lambda，别搞类 |

---

## 7.7 观察者（信号槽）

```cpp
template <typename... Args>
class Signal {
public:
    using Slot = std::function<void(Args...)>;
    using Token = std::size_t;
    Token connect(Slot s) { slots_.emplace(next_, std::move(s)); return next_++; }
    void  disconnect(Token t) { slots_.erase(t); }
    void  emit(Args... args) const { for (auto& [id, s] : slots_) s(args...); }
private:
    std::map<Token, Slot> slots_;
    Token next_ = 0;
};

class Button { public: Signal<int, int> on_click; };
auto tok = btn.on_click.connect([](int x, int y) { ... });
btn.on_click.disconnect(tok);
```

**【坑】生命周期** —— 观察者先于被观察者析构 = 悬垂回调 = 崩溃。

两种解法：
1. `connect` 返回 token，观察者析构时 `disconnect`（上面这样）
2. 观察者列表存 `weak_ptr`，`emit` 时 `lock()`，失效的顺手清掉：

```cpp
void notify(const Event& e) {
    std::erase_if(observers_, [&](const std::weak_ptr<IObserver>& w) {
        if (auto s = w.lock()) { s->on_event(e); return false; }
        return true;                     // 已死，移除
    });
}
```

**【用在哪】** GUI 事件、模型-视图同步、发布订阅、状态变更通知、插件钩子。

---

## 7.8 命令 + 撤销/重做

```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
};

class History {
    std::vector<std::unique_ptr<Command>> done_, undone_;
public:
    void run(std::unique_ptr<Command> c) {
        c->execute();
        done_.push_back(std::move(c));
        undone_.clear();                 // 新操作清空重做栈
    }
    void undo() {
        if (done_.empty()) return;
        done_.back()->undo();
        undone_.push_back(std::move(done_.back()));
        done_.pop_back();
    }
    void redo() { /* 反过来 */ }
};
```

**要点**：命令对象要自带「怎么撤销」所需的全部信息
（比如 `InsertText` 要记住插入位置和内容，才能撤销时精确删除）。

**【用在哪】** 编辑器撤销栈、事务、任务队列、宏录制、请求重放、游戏回放。

---

## 7.9 状态机（variant 版）—— 强烈推荐

```cpp
struct Idle {};
struct Connecting { int attempts; };
struct Connected  { int fd; };
struct Failed     { std::string reason; };
using ConnState = std::variant<Idle, Connecting, Connected, Failed>;

ConnState on_event(const ConnState& s, std::string_view ev) {
    if (ev == "connect") {
        return std::visit(overloaded{
            [](Idle)                -> ConnState { return Connecting{1}; },
            [](const Connecting& c) -> ConnState {
                return c.attempts >= 3 ? ConnState{Failed{"重试超限"}}
                                       : ConnState{Connecting{c.attempts + 1}};
            },
            [](const Connected& c)  -> ConnState { return c; },
            [](const Failed&)       -> ConnState { return Connecting{1}; },
        }, s);
    }
    // ...
}
```

```
状态转移图

  Idle ──connect──> Connecting ──success──> Connected
                      │   ▲                     │
                   fail│   │connect(重试)    close│
                      ▼   │                     ▼
                    Failed ────connect────>  Connecting / Idle
```

**为什么比 `enum` + 一堆成员变量好**

1. 每个状态**带自己独有的数据**（`Connecting` 有 attempts，`Connected` 有 fd）
2. **不合法的状态根本表示不出来**（不会出现 "Idle 状态但 fd 有值" 这种脏数据）
3. **漏处理一个状态，`visit` 编译期就报错**
4. 无堆分配，全在栈上

---

## 7.10 访问者（variant 版）与「表达式问题」

```cpp
using ShapeV = std::variant<Circle, Square, Triangle>;

double area(const ShapeV& s) {
    return std::visit(overloaded{
        [](const Circle& c)   { return 3.14159 * c.r * c.r; },
        [](const Square& q)   { return q.s * q.s; },
        [](const Triangle& t) { return 0.5 * t.b * t.h; },
    }, s);
}
```

**这是个经典权衡（"表达式问题"）**

| | 加**新类型** | 加**新操作** |
|---|---|---|
| 虚函数继承 | ✅ 容易（新写一个派生类） | ❌ 难（要改所有类） |
| `variant` + `visit` | ❌ 难（要改所有 visit） | ✅ 容易（新写一个函数） |

**怎么选**：
- 类型集合**固定**（AST 节点、协议消息、状态、token）→ **variant**
- 类型集合会**不断增加**（插件、图形对象、设备驱动）→ **虚函数**

---

## 7.11 责任链（中间件管道）

```cpp
using Next       = std::function<void()>;
using Middleware = std::function<void(Request&, Next)>;

class Pipeline {
    std::vector<Middleware> mws_;
public:
    Pipeline& use(Middleware m) { mws_.push_back(std::move(m)); return *this; }
    void run(Request& r) { invoke(r, 0); }
private:
    void invoke(Request& r, size_t i) {
        if (i >= mws_.size()) return;
        mws_[i](r, [this, &r, i] { invoke(r, i + 1); });
    }
};

pipe.use([](Request& r, Next next) {
        log_start(r);
        next();                    // 调下一层
        log_end(r);                // 下一层返回后还能做事（洋葱模型）
    })
    .use([](Request& r, Next next) {
        if (!authed(r)) return;    // 不调 next()，链在这里断开
        next();
    })
    .use([](Request& r, Next) { handle(r); });
```

**洋葱模型**：请求一层层进去，响应一层层出来。

```
   请求 ──> [日志前] ──> [鉴权] ──> [处理器]
                                        │
   响应 <── [日志后] <──────────────────┘
```

**【用在哪】** Web 框架中间件、拦截器、日志/鉴权/限流/压缩层、事件处理链。
第 12 章的 HTTP 服务器就是这么组织的。

---

## 7.12 模式选择速查

| 需求 | 用什么 |
|------|--------|
| 管资源 | **RAII**（永远先想这个） |
| 运行期换算法 | 策略（`std::function` / 虚函数） |
| 编译期定死算法且在热点路径 | 模板策略 |
| 一对多通知 | 观察者 / 信号槽 |
| 撤销重做 / 请求排队 | 命令 |
| 隐藏实现、降低编译依赖、稳定 ABI | Pimpl |
| 类型集合封闭 + 多种操作 | `variant` + `visit` |
| 类型集合开放 + 操作固定 | 虚函数继承 |
| 要值语义又要多态 | 类型擦除 |
| 两个维度各自变化（N×M） | 桥接 |
| 树形结构统一处理 | 组合 |
| 动态叠加职责 | 装饰器 |
| 请求可能被多个处理者处理 | 责任链 / 中间件 |
| 大量重复的不可变对象 | 享元 |

## 7.13 反模式提醒

**不要为了用模式而用模式。** 现代 C++ 里很多 GoF 模式已经被语言吃掉了：

| 传统模式 | 现代替代 |
|---------|---------|
| 简单 Strategy / Command | 一个 lambda |
| Visitor | `std::variant` + `std::visit` |
| 比较运算符 mixin（CRTP） | `auto operator<=>() = default` |
| 简单 Builder | C++20 指定初始化器 |
| Iterator | 直接满足 ranges 概念 |
| Prototype（clone） | 拷贝构造 + 值语义 |
| Singleton | 依赖注入（大多数情况） |

**模式是用来沟通的词汇，不是必须完成的仪式。**
代码里出现 `AbstractFactoryProxyBuilderImpl` 这种名字，说明方向错了。

---

<div style="page-break-after: always;"></div>
