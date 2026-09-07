# 第 14 章 · 智能指针深入

▶ 对应程序：`ch14_smartptr`

## 1. 一句话说清它解决什么

智能指针**不是**垃圾回收。它做的是一件更根本的事：**把"谁负责释放"这个信息写进类型系统，让编译器帮你检查。**

裸指针的问题不是"容易忘记 delete"，而是 `T*` 这个类型**什么都没说**：

```cpp
void process(Widget* w);
```

看这个签名，你无法回答：我要不要释放它？它可以为空吗？函数会不会保存它？函数会不会释放它？——只能看文档、看实现、猜。

换成智能指针，签名自己就说清楚了：

| 签名 | 含义 |
|---|---|
| `void sink(std::unique_ptr<T> p)` | 我**接管**所有权（调用方必须 move 进来） |
| `std::unique_ptr<T> source()` | 我把所有权**交给**你（工厂函数） |
| `void use(const T& t)` | 我只读，所有权在你那儿 ← **最常用** |
| `void modify(T& t)` | 我要改，所有权在你那儿 |
| `void maybe(T* p)` | 我只读，且它可能为空 |
| `void share(std::shared_ptr<T> p)` | 我要**共享**所有权（延长它的寿命） |

反例：`void bad(std::shared_ptr<T> p)` —— 只是想读一下，却强迫调用方承担引用计数的原子操作开销，还把接口和 `shared_ptr` 绑死了。

## 2. 裸指针的四种失败模式

**失败 1：忘记 delete。** 单次泄漏 32 字节看着无害，但如果这个函数在事件循环里每秒调用一千次，一天就是 2.7 GB。

**失败 2：提前 return / 抛异常跳过了 delete。**

```cpp
void f() {
    Widget* p = new Widget();
    if (!check()) return;      // 泄漏路径 1
    may_throw();               // 泄漏路径 2
    delete p;                  // 只有一切顺利才走到这里
}
```

一个函数有 N 个出口，你就要写 N 处 delete。异常路径**根本无法用 delete 覆盖** —— 这是 RAII 出现的直接原因。

**失败 3：重复 delete。** 更隐蔽的版本是两个模块都以为自己拥有这块内存。

**失败 4：悬垂指针（use-after-free）。** 最难查：崩溃点离出错点很远，且 Debug 下常常"正常"。

`ch14_smartptr` 的 Part A 实测了异常路径：裸指针版本抛异常后**没有任何析构输出**（泄漏），`unique_ptr` 版本的析构在抛出时（栈展开）就执行了。

## 3. unique_ptr —— 默认选择

### 3.1 零开销

内部就是一个裸指针，所有操作编译期内联掉。**没有任何运行时代价**：

```
sizeof(Widget*)                    = 8
sizeof(unique_ptr<Widget>)         = 8      完全一样
sizeof(unique_ptr<W, 无捕获lambda>) = 8      空基类优化
sizeof(unique_ptr<W, 函数指针>)     = 16     要存函数指针
```

所以**不要因为怕性能而用裸指针**。同时这也告诉你：删除器优先用无捕获 lambda，而不是函数指针。

### 3.2 核心操作

```cpp
auto p = std::make_unique<Widget>(args...);   // 首选：一次分配，异常安全

auto q = std::move(p);      // 转移所有权，p 变成 nullptr（标准保证）
// auto q = p;              // 编译错误：拷贝被 delete —— 这就是「独占」的实现

Widget* raw = p.release();  // 放弃所有权，**你现在要负责 delete**
p.reset(new Widget());      // 释放旧的，接管新的
p.reset();                  // 等价于 p = nullptr
```

`release()` 的唯一正当用途是把所有权交给 C API。

### 3.3 自定义删除器：管理任何资源

智能指针不只管 `new` 出来的内存 —— 任何"需要成对操作"的资源都能管：

```cpp
// FILE* 要用 fclose
auto closer = [](std::FILE* f) { if (f) std::fclose(f); };
std::unique_ptr<std::FILE, decltype(closer)> fp(std::fopen("f.txt", "w"), closer);

// malloc 的内存要用 free
auto freer = [](void* p) { std::free(p); };
std::unique_ptr<void, decltype(freer)> mem(std::malloc(1024), freer);

// socket、互斥量、GPU 句柄、数据库连接、事务……同理
```

注意：**`unique_ptr` 的删除器类型是模板参数的一部分**，所以不同删除器的 `unique_ptr<FILE, ...>` 是不同类型，不能放进同一个容器。`shared_ptr` 相反（删除器存在控制块里，不进类型）。

### 3.4 两个典型应用

**工厂函数返回多态对象：**

```cpp
std::unique_ptr<Shape> make_shape(const std::string& kind) {
    if (kind == "circle") return std::make_unique<Circle>(2.0);
    if (kind == "rect")   return std::make_unique<Rect>(3.0, 4.0);
    return nullptr;
}
```

调用方拿到所有权，多态可用，且不可能忘记释放。

**Pimpl —— 隐藏实现、稳定 ABI、降低编译依赖：**

```cpp
// widget.h —— 头文件干净，改实现不触发下游重编译
class Widget {
public:
    Widget();
    ~Widget();                       // 必须在 .cpp 定义（此处 Impl 不完整）
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;
    void draw() const;
private:
    struct Impl;                     // 只声明
    std::unique_ptr<Impl> impl_;
};

// widget.cpp
struct Widget::Impl { /* 全部重量级细节 */ };
Widget::Widget() : impl_(std::make_unique<Impl>()) {}
Widget::~Widget() = default;         // 到这里 Impl 已完整
```

**注意那个 `~Widget()` 必须在 .cpp 里定义。** 如果让编译器在头文件里隐式生成，它需要 `Impl` 的完整定义来调析构，而头文件里只有声明 —— 报一个很难懂的错误。这是 Pimpl 最常见的踩坑点。

## 4. shared_ptr —— 内部结构

`shared_ptr` 是**两个指针**：

```
shared_ptr<T> sp;              控制块 (control block)
┌──────────────┐              ┌────────────────────────┐
│ ptr        ──┼─────────────►│ strong_count  (原子)   │
│ ctrl       ──┼─────────────►│ weak_count    (原子)   │
└──────────────┘              │ deleter                │
                              │ allocator              │
                              └────────────────────────┘
```

### 4.1 两个计数的分工

很多人只知道 strong。分工是：

- `strong_count == 0` → 销毁**对象**（调 T 的析构 / 删除器）
- `weak_count == 0` → 释放**控制块本身**

为什么要分开？因为 `weak_ptr` 必须能在对象已死后安全地回答"对象还活着吗"—— 它要读 `strong_count`，所以控制块必须比对象活得久。

实现细节：`weak_count` 的**初值是 1**。所有 `shared_ptr` 作为一个整体，共同持有"一份 weak 引用"。这样最后一个 shared 走掉时（strong: 1→0），顺便把这份 weak 也还掉，逻辑统一，无需特殊分支。

### 4.2 make_shared vs shared_ptr(new T)

| | `shared_ptr<T>(new T)` | `make_shared<T>()` |
|---|---|---|
| 堆分配次数 | 2 次（对象 + 控制块） | **1 次**（对象内嵌在控制块里） |
| 异常安全 | 控制块分配失败时对象泄漏 | 安全 |
| 缓存局部性 | 对象和计数分开 | 相邻，更好 |
| 缺点 | — | 只要还有 weak_ptr，整块内存（含对象部分）都无法归还系统 |

**默认用 `make_shared`。** 唯一需要用 `shared_ptr(new T)` 的场景：对象很大且会长期被 weak_ptr 观察。

### 4.3 移动比拷贝快得多

```cpp
auto copied = src;               // 原子自增（有锁总线开销）
auto moved  = std::move(copied); // 只搬两个指针，**无**原子操作
```

高并发下原子操作是真实瓶颈。**能 move 的地方别 copy。**

### 4.4 别名构造 —— 持有成员却延长整个对象的寿命

```cpp
auto parent = std::make_shared<Parent>();
// ptr 指向成员，但控制块用 parent 的
std::shared_ptr<std::string> member_ptr(parent, &parent->child);
```

`parent` 离开作用域后，`Parent` 对象**仍然存活**（`member_ptr` 还持有控制块），通过 `member_ptr` 访问成员完全安全。用于"我只关心这个大对象里的一个字段，但需要保证大对象不被销毁"的场景。

### 4.5 线程安全的边界（极易误解）

- ✅ **安全**：多线程同时拷贝/析构同一个 `shared_ptr`（引用计数是原子的）
- ❌ **不安全**：多线程同时给同一个 `shared_ptr` **变量**赋值（改的是 ptr 和 ctrl 两个字段，不是原子的）
- ❌ **不安全**：多线程同时读写它**指向的对象**（`shared_ptr` 只管指针的线程安全，不管数据的）

**一句话：引用计数是原子的，指针本身和被指对象都不是。**

C++20 起有 `std::atomic<std::shared_ptr<T>>` 解决第二种情况。

## 5. weak_ptr

### 5.1 为什么必须 lock()

```cpp
// 错误！
if (!w.expired()) w.lock()->f();
```

多线程下，"检查 expired"和"使用对象"之间，对象可能刚好被销毁。`lock()` 把"检查 + 加引用"做成一个**原子操作**，拿到 `shared_ptr` 后对象就一定活着：

```cpp
if (auto locked = w.lock()) {
    locked->f();          // 安全
}
```

### 5.2 三个用途

**打破循环引用**（见第 13 章 4.3）：父拥有子用 shared，子指向父用 weak。

**缓存 —— 不阻止对象被回收：**

```cpp
class ImageCache {
    std::map<std::string, std::weak_ptr<Image>> cache_;
public:
    std::shared_ptr<Image> get(const std::string& key) {
        if (auto it = cache_.find(key); it != cache_.end()) {
            if (auto hit = it->second.lock()) return hit;   // 命中
            cache_.erase(it);                               // 条目已失效，清理
        }
        auto obj = std::make_shared<Image>(load(key));
        cache_[key] = obj;        // 只存 weak，不延长寿命
        return obj;
    }
};
```

只要外部还在用就命中缓存，没人用了就自动失效 —— 缓存不会因为持有强引用而导致内存无法回收。

**异步回调中安全引用 this**（见下节）。

## 6. enable_shared_from_this 的原理

**问题**：成员函数里怎么拿到"管理自己的那个 shared_ptr"？直接 `shared_ptr<T>(this)` 会创建第二个控制块 → 双重释放。

**原理**：`enable_shared_from_this<T>` 内部有一个 `weak_ptr<T>` 成员。当你用 `shared_ptr` 首次接管这个对象时，`shared_ptr` 的构造函数会通过 SFINAE 检测到 T 继承自它，于是把自己的控制块塞进那个 `weak_ptr` 里。之后 `shared_from_this()` 只是 `weak.lock()`。

```cpp
// 简化后的实现
template <class T>
class enable_shared_from_this {
    mutable std::weak_ptr<T> weak_this_;     // 由 shared_ptr 构造函数填写
public:
    std::shared_ptr<T> shared_from_this() { return std::shared_ptr<T>(weak_this_); }
    std::weak_ptr<T>   weak_from_this()   { return weak_this_; }    // C++17
};
```

**三个必须遵守的前提：**

1. 对象必须**已经**被 `shared_ptr` 管理，否则 `weak_this_` 是空的 → `shared_from_this()` 抛 `std::bad_weak_ptr`
2. 不能在构造函数里调用（此时 `shared_ptr` 还没接管）
3. 必须是 public 继承（`shared_ptr` 的构造函数要能看到基类）

### shared_from_this 还是 weak_from_this？

```cpp
// 用 shared：延长自身寿命直到回调执行完
void start(Queue& q) {
    auto self = shared_from_this();
    q.push([self, this] { doWork(); });     // self 保证对象不死
}

// 用 weak：对象销毁则跳过回调
void start_weak(Queue& q) {
    q.push([weak = weak_from_this()] {
        if (auto self = weak.lock()) self->doWork();
        // 否则安全跳过
    });
}
```

**选择标准：**

- 回调**必须**执行（如写入落盘、释放外部资源）→ `shared_from_this`
- 回调只是"通知"（对象没了就不用通知了）→ `weak_from_this`

网络库里 99% 是后者 —— 连接断了就没必要回调了。但也要注意：用 `shared_from_this` 时，如果回调队列一直不清空，对象就永远不释放（相当于泄漏）。

## 7. 手写实现要点

`ch14_smartptr` 的 Part C 从零实现了三个智能指针并通过多线程压测（4 线程 × 2 万次拷贝，计数正确）。几个关键设计点：

### MyUniquePtr

```cpp
template <typename T, typename Deleter = DefaultDelete<T>>
class MyUniquePtr : private Deleter {      // 私有继承 -> 空删除器不占空间
    T* ptr_ = nullptr;
public:
    // 这两行就是「独占」的全部实现
    MyUniquePtr(const MyUniquePtr&)            = delete;
    MyUniquePtr& operator=(const MyUniquePtr&) = delete;

    // 移动必须把源置空，漏了就双重释放
    MyUniquePtr(MyUniquePtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }

    void reset(T* p = nullptr) noexcept {
        T* old = ptr_;
        ptr_ = p;                 // 先改状态
        if (old) Deleter::operator()(old);   // 再释放：防止删除器里回访本对象
    }
};
```

用**私有继承**而非成员来持有删除器，是为了获得空基类优化 —— 无状态删除器（无捕获 lambda）就不占空间。

### 控制块与 lock() 的 CAS 循环

`weak_ptr::lock()` 的核心是"原子地只有在计数不为 0 时才 +1"：

```cpp
bool try_add_strong() noexcept {
    long cur = strong_.load(std::memory_order_relaxed);
    while (cur != 0) {
        if (strong_.compare_exchange_weak(cur, cur + 1,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
            return true;       // 抢到了，对象保证存活
        }
        // CAS 失败：cur 已被更新为最新值，重试
    }
    return false;              // 计数已归零，对象没了
}
```

**必须用 CAS 循环，不能写成 `if (strong_ != 0) ++strong_;`** —— 那样在判断和自增之间对象可能已被销毁。这是无锁编程最基本的模式。

引用计数递减用 `memory_order_acq_rel`：保证对象的所有写操作在销毁前对本线程可见，且本线程的写操作对最后那个执行销毁的线程可见。

### 手写时踩到的真实坑

```cpp
// 这个 requires 不是装饰
template <typename Deleter = DefaultDelete<T>>
    requires std::is_invocable_v<Deleter&, T*>
explicit MySharedPtr(T* p, Deleter d = Deleter{});
```

没有这个约束，`make_shared_ptr` 传进来的 `MyControlBlockInline<T>*`（派生类指针）对这个模板是**精确匹配**（`Deleter = MyControlBlockInline<T>*`），而对"已有控制块"的私有构造函数需要一次派生类到基类的指针转换 —— 精确匹配优先，模板胜出，然后在 `del_(ptr_)` 处报"指针不是可调用对象"。

这就是第 13 章 4.5「万能引用吞掉重载」的真实翻版。最终用 tag 参数解决（`AdoptCtrl{}`），这也是标准库的通用手法 —— `std::piecewise_construct`、`std::in_place`、`std::nothrow` 都是同一个套路。

## 8. 选型决策

```
需要管理动态分配的对象吗？
 ├─ 不需要 -> 栈对象 / 成员对象（最快，最安全）
 └─ 需要 -> 所有权要共享吗？
            ├─ 不共享（99% 的情况）-> unique_ptr      零开销
            └─ 共享 -> shared_ptr + 反向引用用 weak_ptr

只是「借用」不涉及所有权 -> 传 T& / const T& / T*（不要传智能指针）
```

| | 大小 | 运行时开销 |
|---|---|---|
| `unique_ptr` | 1 指针 | **0**（全部内联） |
| `shared_ptr` | 2 指针 | 拷贝/析构各一次原子操作 |
| `weak_ptr` | 2 指针 | `lock()` 是一次 CAS 循环 |

**三条实践准则：**

1. 永远用 `make_unique` / `make_shared`，不写裸 `new`
2. 函数参数默认传引用，只在"转移或共享所有权"时传智能指针
3. **`shared_ptr` 是最后的选择，不是默认选择** —— 它意味着"生命周期由运行时决定"，这本身就让程序更难推理。能用 `unique_ptr` 表达的所有权关系，就不要用 `shared_ptr`
