# 第 13 章 · C++ 易错点大全

▶ 对应程序：`ch13_pitfalls`

这一章是全书唯一「反向」组织的章节：不讲怎么写对，讲**怎么会写错**。

先说一件比记住所有坑更重要的事。

## 0. 关于「未定义行为」

UB（Undefined Behavior，未定义行为）不是「程序会崩溃」，而是**标准放弃对程序行为做任何约定**。编译器可以：

- 让它看起来正常运行（最坏的情况）
- 优化掉你的检查代码
- 在与出错点毫无关系的地方崩溃
- Debug 下正常，Release 下出错

举个真实的例子：

```cpp
bool check_overflow(int x) {
    return x + 1 > x;      // 有符号溢出是 UB
}
```

编译器的推理是：「有符号溢出是 UB，UB 不允许发生，所以 `x + 1` 一定大于 `x`」，于是把整个函数优化成 `return true;`。你的溢出检查被**删掉了**，而且没有任何警告。

所以对 UB 的正确态度不是「小心一点」，而是**用工具把它挡在编译期和测试期**：

```bash
# MSVC
cl /W4 /permissive- /fsanitize=address /analyze main.cpp

# GCC / Clang
g++ -Wall -Wextra -Wpedantic -fsanitize=address,undefined -g main.cpp
```

`-fsanitize=address` 能抓到越界、悬垂、双重释放；`undefined` 能抓到溢出、错误的类型转换。**开销大约 2 倍，但它能在测试阶段发现 90% 的内存 bug**，绝对值得。

再配一个静态检查：

```bash
clang-tidy main.cpp -checks='bugprone-*,cppcoreguidelines-*,performance-*'
```

`bugprone-use-after-move` 一条规则就能省掉你以后无数小时。

---

## 1. 初始化与类型系统

### 1.1 花括号 vs 圆括号

```cpp
std::vector<int> a(10, 5);   // 10 个 5
std::vector<int> b{10, 5};   // 2 个元素：10 和 5
```

只要类型有接收 `std::initializer_list` 的构造函数，花括号就**优先**匹配它，哪怕另一个重载"更合适"。这是标准规定的优先级，没有例外。

更坑的是它的行为**依赖元素类型**：

```cpp
std::vector<std::string> c{10, "x"};   // string 无法从 10 转换
                                        // -> 回退到 (count, value)，得到 10 个 "x"
```

**规则：指定"几个几"一律用圆括号。** 花括号只用于"就这几个元素"。

### 1.2 auto 会衰减

`auto` 完全照抄模板实参推导规则：按值推导会丢引用、丢顶层 const、数组退化成指针。

```cpp
std::vector<BigObject> v;
for (auto x : v)        { }   // 每次循环拷贝一个 BigObject！
for (const auto& x : v) { }   // 只读，正确
for (auto& x : v)       { }   // 要修改，正确
```

范围 for 里写 `auto x` 是**最常见的性能问题**，因为它不报错、不警告，只是慢。

| 写法 | 推导结果（源为 `const int ci`） |
|---|---|
| `auto a = ci;` | `int`（const 丢了，a 可以改） |
| `const auto a = ci;` | `const int` |
| `auto& a = ci;` | `const int&`（引用保留被引用对象的 const） |
| `auto&& a = ci;` | `const int&`（万能引用 + 引用折叠） |
| `decltype(auto) a = ci;` | `const int`（完整保留） |

### 1.3 `vector<bool>` 不是容器

它是个特化版本，为省内存按 bit 存储。`operator[]` 没法返回 `bool&`（引用不能指向一个 bit），只能返回代理对象：

```cpp
std::vector<bool> vb{false, false};
auto proxy = vb[0];    // 类型不是 bool，是 vector<bool>::reference
proxy = true;          // 改到了容器里！
bool real = vb[1];     // 显式写 bool 才是真拷贝
bool* p = &vb[0];      // 编译错误：无法取一个 bit 的地址
```

需要 bool 容器时用 `std::vector<char>`、`std::deque<bool>` 或 `std::bitset`。

### 1.4 无符号下溢：`size() - 1`

`size()` 返回无符号的 `size_t`。空容器上 `0u - 1` 不是 `-1`，而是回绕成 `SIZE_MAX`（18446744073709551615）。

```cpp
// 空容器时循环体执行天文数字次
for (size_t i = 0; i < v.size() - 1; ++i) { }

// 正确写法 1：把减法移到另一侧
for (size_t i = 0; i + 1 < v.size(); ++i) { }

// 正确写法 2：C++20 的 std::ssize 返回有符号长度
for (std::ptrdiff_t i = 0; i < std::ssize(v) - 1; ++i) { }
```

配套的比较陷阱：

```cpp
int si = -1; unsigned ui = 1;
si < ui;    // false！si 被转成 4294967295
```

有符号与无符号比较时，**有符号的一方会被转成无符号**。MSVC 的 C4018 和 GCC 的 `-Wsign-compare` 就是在警告这个，不要忽略。

### 1.5 浮点相等比较

IEEE-754 二进制浮点无法精确表示 `0.1`、`0.2`、`0.3`，就像十进制无法精确表示 1/3。

```cpp
0.1 + 0.2 == 0.3        // false
```

正确的比较要**同时**考虑绝对容差和相对容差：

```cpp
bool nearly_equal(double x, double y, double eps = 1e-9) {
    double diff = std::abs(x - y);
    if (diff <= eps) return true;                        // 处理接近 0
    return diff <= eps * std::max(std::abs(x), std::abs(y));  // 处理大数值
}
```

固定容差在大数值上会失效：`1e16 == 1e16 + 1` 是 `true`，因为 double 在这个量级已经分不出 1 的差别。

**金额绝对不要用 double。** 用整数存"分"，或用定点/十进制库。数据库里对应的是 `DECIMAL` 而不是 `FLOAT`（见第 17 章）。

---

## 2. 指针、引用、生命周期

这一组是崩溃的主要来源。

### 2.1 返回局部变量的引用

```cpp
const std::string& bad() {
    std::string local = "x";
    return local;             // UB：local 在此行之后销毁
}
```

返回引用只能指向比函数活得更久的东西：成员变量、静态变量、参数。

不要为了"性能"返回引用 —— 按值返回有 NRVO（命名返回值优化），编译器会直接在调用方的位置构造对象，**零拷贝零移动**。

### 2.2 string_view / span 悬垂

这是现代 C++ 的新型踩坑重灾区。`string_view` 只存"指针 + 长度"，**不拥有**数据。

```cpp
std::string make_temp() { return "临时对象"; }

std::string_view sv = make_temp();   // UB！临时 string 在这一行末尾销毁
std::cout << sv;                      // 读已释放内存
```

正确做法：

```cpp
std::string owner = make_temp();     // 先让具名变量持有所有权
std::string_view sv = owner;          // 再取 view
```

另一个坑：`string_view` **不保证以 `\0` 结尾**，不能直接喂给 C 接口：

```cpp
std::string full = "HelloWorld";
auto part = std::string_view(full).substr(0, 5);   // "Hello"
printf("%s", part.data());                          // 错！会打印 "HelloWorld"
printf("%.*s", (int)part.size(), part.data());      // 对：带上长度
```

**准则：`string_view` 只适合做函数参数，不适合做成员变量和返回值。** 类的成员存 `string_view` 几乎总是 bug。

### 2.3 迭代器失效

各容器的失效规则（面试高频，值得背）：

| 容器 | 插入 | 删除 |
|---|---|---|
| `vector` | 扩容则**全部**失效；未扩容则插入点之后失效 | 删除点之后失效 |
| `deque` | 迭代器基本都失效；**两端**操作时引用/指针不失效 | 同 |
| `list` / `forward_list` | 不失效 | 仅被删元素失效 |
| `map` / `set` | 不失效 | 仅被删元素失效 |
| `unordered_*` | rehash 时**迭代器**全失效，但引用/指针不失效 | 仅被删元素失效 |

`unordered_map` 那一行是个重要细节：rehash 只是把节点重新挂到不同桶上，**节点本身没有移动**，所以指向 value 的指针和引用依然有效。

边遍历边删的正确写法：

```cpp
// 手写：用 erase 的返回值
for (auto it = v.begin(); it != v.end(); ) {
    if (pred(*it)) it = v.erase(it);   // erase 返回下一个有效迭代器
    else           ++it;                // 只有不删时才自增
}

// C++20：一行搞定，而且是 O(n)
std::erase_if(v, pred);
```

### 2.4 `std::remove` 不会真的删除

```cpp
std::vector<int> v{1, 2, 3, 2, 5};
std::remove(v.begin(), v.end(), 2);
// v.size() 仍是 5！尾部残留旧值
```

`std::remove` 是**算法**，它不知道容器怎么删元素（这是 STL"算法与容器分离"的代价，见第 15 章）。它只把要保留的元素往前搬，返回"新逻辑末尾"。

```cpp
// C++20 之前：erase-remove 惯用法
v.erase(std::remove(v.begin(), v.end(), 2), v.end());

// C++20 起
std::erase(v, 2);
```

### 2.5 const 的位置

**const 修饰它左边的东西；左边没有就修饰右边。** 把声明从右往左念：

```cpp
const int* p1;         // 「指向 const int 的指针」：值只读，指针可变
int* const p2;         // 「指向 int 的 const 指针」：指针只读，值可变
const int* const p3;   // 都只读
int const* p4;         // 等价于 const int*
```

建议统一写在类型右边（`int const*`），这样"const 修饰左边"的规则永远一致。

---

## 3. 类与对象

### 3.1 基类析构函数不是 virtual

```cpp
struct Base { ~Base() {} };                    // 少了 virtual
struct Derived : Base { std::vector<int> big; };

Base* p = new Derived();
delete p;    // 只调用 Base::~Base，Derived 的成员泄漏
```

**只要一个类可能被继承并通过基类指针删除，析构就必须 `virtual`。**

反过来：不打算被继承的类加 `final` 比加 `virtual` 析构更省（没有虚表开销）。

### 3.2 构造/析构函数里调用虚函数

对象是"由内向外"构造的。基类构造函数执行时派生类部分还没初始化，所以标准规定此时对象的**动态类型就是基类**，虚表还指向基类。

```cpp
struct Base {
    Base() { who(); }                          // 永远调 Base::who
    virtual void who() { }
};
struct Derived : Base {
    int value_ = 999;
    void who() override { use(value_); }        // 若被调用，value_ 还未初始化
};
```

如果基类的 `who()` 是**纯虚**的，这就是 UB（调用纯虚函数，通常直接崩）。

需要"构造后初始化"就单独提供 `init()` 方法，或用工厂函数。

### 3.3 成员初始化顺序 = 声明顺序

成员按**类中声明的顺序**初始化，初始化列表里的书写顺序**不影响**执行顺序。

```cpp
class Bad {
    size_t size_;      // 先声明 -> 先初始化
    size_t count_;
public:
    Bad(size_t n) : count_(n), size_(count_ * 2) { }
    //                          ^^^^^^^^^^^^^^^ size_ 先算，此时 count_ 是垃圾
};
```

**规则：初始化列表只依赖构造函数参数，不依赖其它成员。** 顺序与声明顺序保持一致（MSVC 的 C5038、GCC 的 `-Wreorder` 会警告）。

### 3.4 忘记 explicit

单参数构造函数（或除首参外都有默认值的构造函数）会成为"转换构造函数"，允许编译器插入一次隐式转换：

```cpp
struct Bad  { Bad(size_t len); };
struct Good { explicit Good(size_t len); };

void f(const Bad& b);
f('A');            // 编译通过！char -> size_t -> Bad，两次隐式转换
```

**准则：单参数构造函数默认加 explicit**，除非你确实想要隐式转换。`std::vector` 的 `explicit vector(size_t)` 就是为了阻止 `std::vector<int> v = 10;`。

### 3.5 对象切片

```cpp
std::vector<Base> bad;
bad.push_back(Derived{});    // 切片！派生部分被切掉，虚表指针改成 Base 的

std::vector<std::unique_ptr<Base>> good;
good.push_back(std::make_unique<Derived>());   // 正确
```

按值传参也会切片。**多态容器必须存指针。**

防御手段：基类可以 `delete` 拷贝构造，让切片直接编译失败。

### 3.6 名字隐藏

名字查找是**逐作用域**进行的：在派生类里找到该名字就停止，根本不会去看基类。这与重载解析是两个独立阶段。

```cpp
struct Base { void f(int); void f(double); };
struct Derived : Base {
    void f(std::string);      // 基类的 f(int)/f(double) 全被隐藏
};
Derived d;
d.f(1);                        // 编译错误：int 转不成 string
d.Base::f(1);                  // 只能显式限定
```

解法：`using Base::f;` 把基类的重载拉进本作用域。

### 3.7 自赋值与 copy-and-swap

错误的拷贝赋值实现是"先释放旧资源，再拷贝新资源"—— 自赋值时"新资源"就是刚被释放的那块内存。

正确做法是 **copy-and-swap**，一份代码同时解决自赋值安全、异常安全、拷贝与移动赋值：

```cpp
class Widget {
public:
    Widget& operator=(Widget o) noexcept {   // 注意：按值传参
        swap(o);                              // 只交换指针，不会抛
        return *this;                         // o 析构时释放旧资源
    }
    void swap(Widget& o) noexcept { std::swap(data_, o.data_); }
};
```

参数按值传递时先完成拷贝（可能抛异常，但此时 `*this` 还完好），再 swap（不会抛）—— 这就给出了**强异常保证**。

### 3.8 移动之后继续使用

标准只保证被移动对象处于"有效但未指定"状态：可以安全析构、可以重新赋值，但**内容不确定**。

```cpp
std::string s = "abc";
std::string t = std::move(s);
// s 现在的内容不确定（标准库实现里通常变空，但别依赖）
s = "重新赋值";        // 唯一推荐的后续操作
```

例外：`unique_ptr` 移动后**一定**是 `nullptr`，这个有标准保证。

---

## 4. 现代 C++ 的新型坑

### 4.1 lambda 捕获与生命周期

```cpp
std::function<int()> make() {
    int local = 42;
    return [&local]{ return local; };    // UB：local 在函数返回后就没了
    return [local]{ return local; };     // 正确：值捕获
}
```

**准则：立即执行的 lambda 可以用 `[&]`；要存起来 / 跨线程 / 异步执行的，必须值捕获。**

C++14 起可以初始化捕获，用来移动捕获 move-only 对象：

```cpp
auto up = std::make_unique<int>(5);
auto f = [p = std::move(up)]{ return *p; };
```

### 4.2 捕获 this —— 异步回调的头号崩溃原因

`[this]` 和 C++20 前的 `[=]` 捕获的是**裸指针** this，不延长对象寿命。对象销毁后回调才被触发，就是访问已释放内存。

正确姿势：

```cpp
class Session : public std::enable_shared_from_this<Session> {
public:
    auto makeCallback() {
        return [weak = weak_from_this()] {          // C++17
            if (auto self = weak.lock()) {          // 提升成功 = 对象还活着
                self->doWork();
            }
            // 提升失败 = 对象已销毁，安全跳过
        };
    }
};
```

第 16 章的 `TcpConnection` 大量使用这个模式 —— 网络库里连接随时可能断开，这是必需的。

### 4.3 shared_ptr 循环引用

```
父节点 shared_ptr ──► 子节点
子节点 shared_ptr ──► 父节点      两边计数都停在 1，永不释放
```

**准则：「拥有」关系用 `shared_ptr`，「反向引用/观察」关系用 `weak_ptr`。**

典型：父节点拥有子节点（shared），子节点指回父节点（weak）。

### 4.4 同一裸指针交给两个 shared_ptr

`shared_ptr` 的引用计数存在**控制块**里。用裸指针构造 `shared_ptr` 会**新建一个控制块**：

```cpp
int* raw = new int(42);
std::shared_ptr<int> sp1(raw);
std::shared_ptr<int> sp2(raw);   // 第二个控制块！两次 delete -> 崩溃
```

**准则：永远用 `make_shared`，不让裸指针出现。**

同理，类内部不能 `return std::shared_ptr<T>(this)` —— 必须继承 `enable_shared_from_this` 并用 `shared_from_this()`。详见第 14 章。

### 4.5 万能引用吞掉重载

`T&&` 模板是**精确匹配**（不需要任何转换），而普通重载遇到非精确类型需要一次转换。所以模板几乎总是赢：

```cpp
void f(int);
template <typename T> void f(T&&);

short s = 1;
f(s);        // 进了模板！因为 short -> int 需要提升，而模板是精确匹配
```

C++20 起用 concept 约束解决：

```cpp
template <typename T>
    requires (!std::is_arithmetic_v<std::remove_cvref_t<T>>)
void f(T&&);
void f(int);         // 现在算术类型正确走这个
```

第 14 章手写 `MySharedPtr` 时真实遇到了这个坑：删除器模板把"已有控制块"的构造函数挤掉了，因为派生类指针对模板是精确匹配。

### 4.6 std::move 的三种误用

**误用 1：在 return 语句里 move**

```cpp
std::vector<int> good() { std::vector<int> v; return v; }              // NRVO，零成本
std::vector<int> bad()  { std::vector<int> v; return std::move(v); }   // 禁用 NRVO！
```

`return local;` 本身有 NRVO（直接在调用方构造）。写 `std::move` 反而**阻止**了这个优化，退化成一次移动构造。

**误用 2：对 const 对象 move**

```cpp
const std::string cs = "x";
std::string dst = std::move(cs);   // std::move(const T&) 得到 const T&&
                                    // 匹配不上 T&& 的移动构造，静默退回拷贝
```

**误用 3：在万能引用上用 move 而不是 forward**

```cpp
template <typename T>
void wrong(T&& arg) { std::string s = std::move(arg); }        // 无条件搬走
template <typename T>
void right(T&& arg) { std::string s = std::forward<T>(arg); }  // 保持原值类别
```

**口诀：参数写的是 `T&&`（模板推导）就用 `forward`，写的是 `Widget&&` 就用 `move`。**

### 4.7 auto 遇到花括号

```cpp
auto a = 1;       // int
auto b{1};        // int（C++17 起；C++11/14 是 initializer_list<int>）
auto c = {1};     // initializer_list<int>  <- 意料之外
auto d = {1, 2};  // initializer_list<int>
```

`initializer_list` 内部只是指针 + 长度，**不拥有数据**，别存起来（又是悬垂）。

---

## 5. 并发

### 5.1 `++` 不是原子操作

`counter++` 是"读-改-写"三步。两个线程可能都读到 100，各自加到 101 写回 —— 一次自增就丢了。这是 UB，不只是"结果不准"。

| 方案 | 适用场景 |
|---|---|
| `std::atomic<int>` | 单个变量，最快 |
| `std::mutex` | 需要保持多个变量之间的一致性 |
| 无共享 | 最好的方案：每线程独立数据，最后归并 |

### 5.2 条件变量必须带谓词

两个独立问题：

- **虚假唤醒**：`wait` 允许无理由返回，必须循环检查条件
- **丢失唤醒**：如果 `notify` 发生在 `wait` 之前，这次通知就丢了，`wait` 会一直等下去

带谓词的 `wait` 一次性解决两个（它内部就是 `while` 循环）：

```cpp
cv.wait(lk, [&]{ return ready; });     // 永远这样写
cv.wait(lk);                            // 永远不要这样写
```

标准三件套：**持锁改状态 → 解锁 → notify**。等待方一律用谓词版。

### 5.3 死锁

三种解法：

1. 全局固定加锁顺序（比如按地址或 id 排序）
2. `std::scoped_lock lk(m1, m2);` —— C++17，内部有死锁避免算法
3. 缩小临界区，**不要持锁调用外部代码**（回调可能又来加同一把锁）

第 16 章聊天室的广播就用了第 3 条：先把成员列表拷出来再解锁，然后才发送。

### 5.4 thread 忘记 join

`std::thread` 析构时若仍是 joinable 状态，标准规定调用 `std::terminate`。这是刻意设计 —— 默认 detach 会导致悬垂引用，默认 join 会在析构里意外阻塞，两者都比直接崩更危险。

C++20 起用 `std::jthread`，析构自动 `request_stop()` + `join()`：

```cpp
std::jthread jt([](std::stop_token st) {
    while (!st.stop_requested()) { work(); }
});
```

### 5.5 volatile 不是 atomic

`volatile` 只保证"每次都从内存读，不做寄存器缓存"，用于内存映射硬件寄存器。它**不提供**原子性，也**不建立**内存屏障，无法阻止 CPU 和编译器的指令重排。

**线程同步一律用 `std::atomic`；`volatile` 只用于硬件寄存器和信号处理。**

---

## 6. 其它高频坑

### 6.1 数组退化

数组作为函数参数会退化成指针，长度信息彻底丢失。`void f(int arr[10])` 和 `void f(int* arr)` **完全等价**，那个 10 被忽略。

```cpp
void bad(int arr[10])  { sizeof(arr); }        // 8，是指针大小
template <size_t N>
void good(int (&arr)[N]) { }                    // 数组引用，保留长度
void best(std::span<int> s) { s.size(); }       // C++20 首选
```

`std::span` 既保留长度，又能接受 `vector` / `array` / C 数组。

### 6.2 宏

```cpp
#define SQUARE_BAD(x)  x * x        // SQUARE_BAD(1+2) -> 1+2*1+2 = 5
#define SQUARE_OK(x)   ((x) * (x))  // 优先级对了，但仍会求值两次
constexpr int best(int x) { return x * x; }     // 只求值一次，类型安全
```

`SQUARE_OK(i++)` 会让 `i` 自增两次。**能用 constexpr 函数、模板、inline 就绝不用宏。**

Windows 上记得 `#define NOMINMAX`，否则 `min`/`max` 宏会和 `std::min`/`std::max` 冲突。

### 6.3 求值顺序

C++17 前，函数参数的求值顺序完全未指定；C++17 起规定"参数之间不交错"，但**顺序仍然未指定**。

```cpp
printf("%d %d", i++, i++);    // 可能 "0 1" 也可能 "1 0"
```

**一条语句里不要对同一个变量做多次修改。**

C++17 起确定顺序的：`<<`/`>>` 从左到右；`a.b()`、`a->b()` 先求 `a`；赋值先求右侧。

### 6.4 异常安全

```cpp
f(new A, new B);    // 若第二个 new 抛异常，第一块内存就泄漏了
f(std::make_unique<A>(), std::make_unique<B>());   // 安全
```

`make_unique` 把"分配"和"接管所有权"绑成一个不可分割的操作。

**RAII 是 C++ 异常安全的唯一可靠手段。** 手写 try/catch 清理必然遗漏路径。通用守卫：

```cpp
template <typename F>
class ScopeGuard {
    F f_; bool active_ = true;
public:
    explicit ScopeGuard(F f) : f_(std::move(f)) {}
    ~ScopeGuard() { if (active_) f_(); }
    void dismiss() { active_ = false; }
};
template <typename F> ScopeGuard(F) -> ScopeGuard<F>;
```

### 6.5 map 的 operator[] 会插入

`operator[]` 的语义是"返回该键的引用，不存在就默认构造插入"。所以它不可能是 const 成员函数。

```cpp
int v = m["不存在的键"];      // 悄悄插入了 {"不存在的键", 0}

// 查询要用这些
if (auto it = m.find(k); it != m.end()) { }
if (m.contains(k)) { }                        // C++20
m.at(k);                                       // 不存在时抛 out_of_range

// 插入要用这些
m.try_emplace(k, args...);      // 键存在时不构造 value
m.insert_or_assign(k, v);
```

---

## 速查：最容易犯的十个

| # | 坑 | 后果 |
|---|---|---|
| 1 | 基类析构没写 virtual | 派生类资源泄漏 |
| 2 | 迭代器失效后继续用 | 随机崩溃 |
| 3 | `string_view` / `span` 悬垂 | 读已释放内存 |
| 4 | `shared_ptr` 循环引用 | 内存永不释放 |
| 5 | lambda 用 `[&]` 捕获后异步执行 | 悬垂引用 |
| 6 | `size() - 1` 在空容器上 | 无符号下溢 |
| 7 | 条件变量 `wait` 不带谓词 | 偶发卡死 |
| 8 | `map` 用 `[]` 做查询 | 悄悄插入 |
| 9 | `std::remove` 后忘记 `erase` | 元素没删掉 |
| 10 | move 之后继续使用原对象 | 值不确定 |

隐蔽程度排名第一的其实不在上表里 —— 是**隐式类型转换导致的索引失效**（第 17 章 17.7）。它不在 C++ 层面，而在 SQL 层面，不报错、不警告，只是慢 1000 倍。
