// =============================================================================
// 第 1 章 —— C++11：现代 C++ 的起点
//
// C++11 是 C++ 历史上最大的一次升级，大到被称为「一门新语言」。
// 这一章的每个特性都标注了【用在哪】，方便你判断什么时候该用。
//
// 运行： ch01_cpp11.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <memory>
#include <functional>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <future>
#include <condition_variable>
#include <chrono>
#include <type_traits>
#include <cstring>
#include <cstdio>
#include <array>
#include <initializer_list>

// =============================================================================
// 演示用：一个「有资源」的类，用来观察拷贝 vs 移动
// =============================================================================
class Buffer {
public:
    explicit Buffer(size_t n, const char* tag = "buf")
        : size_(n), data_(new char[n]), tag_(tag) {
        std::memset(data_, 0, n);
        std::cout << "    [构造]      " << tag_ << " size=" << size_ << "\n";
    }

    ~Buffer() {
        std::cout << "    [析构]      " << tag_ << (data_ ? "" : "(已被移空)") << "\n";
        delete[] data_;
    }

    // 拷贝构造：深拷贝，贵
    Buffer(const Buffer& o) : size_(o.size_), data_(new char[o.size_]), tag_(o.tag_) {
        std::memcpy(data_, o.data_, size_);
        std::cout << "    [拷贝构造]  " << tag_ << " 复制了 " << size_ << " 字节 <== 贵!\n";
    }

    // 拷贝赋值
    Buffer& operator=(const Buffer& o) {
        if (this != &o) {
            delete[] data_;
            size_ = o.size_;
            data_ = new char[size_];
            std::memcpy(data_, o.data_, size_);
            tag_  = o.tag_;
            std::cout << "    [拷贝赋值]  " << tag_ << " <== 贵!\n";
        }
        return *this;
    }

    // 移动构造：偷资源，便宜。
    // noexcept 非常重要：vector 扩容时只有 noexcept 的移动才会被使用，
    // 否则为了强异常安全保证它会退化成拷贝。
    Buffer(Buffer&& o) noexcept
        : size_(o.size_), data_(o.data_), tag_(std::move(o.tag_)) {
        o.data_ = nullptr;       // 必须置空！否则析构两次 -> 崩溃
        o.size_ = 0;
        std::cout << "    [移动构造]  " << tag_ << " 只搬了指针 <== 便宜!\n";
    }

    // 移动赋值
    Buffer& operator=(Buffer&& o) noexcept {
        if (this != &o) {
            delete[] data_;
            size_ = o.size_;  data_ = o.data_;  tag_ = std::move(o.tag_);
            o.data_ = nullptr; o.size_ = 0;
            std::cout << "    [移动赋值]  " << tag_ << " <== 便宜!\n";
        }
        return *this;
    }

    size_t size() const { return size_; }

private:
    size_t      size_ = 0;
    char*       data_ = nullptr;
    std::string tag_;
};

Buffer make_buffer(size_t n) { return Buffer(n, "工厂产物"); }

// =============================================================================
// 变参模板
// =============================================================================
template <typename T>
void print_all(const T& t) { std::cout << t << "\n"; }

template <typename T, typename... Rest>
void print_all(const T& t, const Rest&... rest) {
    std::cout << t << " ";
    print_all(rest...);                 // 递归展开，每次少一个参数
}

// 完美转发工厂 —— make_unique / make_shared 的原理
template <typename T, typename... Args>
std::unique_ptr<T> my_make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// =============================================================================
// 演示万能引用与完美转发
// =============================================================================
void overloaded(int&)       { std::cout << "    收到 左值引用 int&\n"; }
void overloaded(const int&) { std::cout << "    收到 常量左值引用 const int&\n"; }
void overloaded(int&&)      { std::cout << "    收到 右值引用 int&&\n"; }

template <typename T>
void bad_forward(T&& v) { overloaded(v); }                    // 丢失右值性

template <typename T>
void good_forward(T&& v) { overloaded(std::forward<T>(v)); }  // 保持原值类别

// =============================================================================
// default / delete / override / final / 委托构造 / 继承构造
// =============================================================================
class NonCopyable {
public:
    NonCopyable() = default;                              // 显式要一个默认构造
    NonCopyable(const NonCopyable&)            = delete;  // 禁止拷贝
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&)                 = default; // 允许移动
    NonCopyable& operator=(NonCopyable&&)      = default;
};

class Widget {
public:
    Widget(int a, int b) : a_(a), b_(b) { std::cout << "    Widget(" << a << "," << b << ")\n"; }
    Widget(int a) : Widget(a, 0) {}        // 委托构造：转调另一个构造
    Widget()      : Widget(0, 0) {}
    int sum() const { return a_ + b_; }
private:
    int a_, b_;
};

struct Base {
    Base(int x) : x_(x) {}
    virtual ~Base() = default;
    virtual void f(int) { std::cout << "    Base::f(int)\n"; }
    virtual void g() { std::cout << "    Base::g()\n"; }
    int x_;
};
struct Derived : Base {
    using Base::Base;                       // 继承基类构造函数
    void f(int) override { std::cout << "    Derived::f(int)  <-- override 保证真的覆盖了\n"; }
    void g() final       { std::cout << "    Derived::g()     <-- final 禁止再被覆盖\n"; }
};

// =============================================================================
// constexpr / static_assert / using 别名
// =============================================================================
constexpr int factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }  // C++11 只能一条 return

template <typename T> using Vec  = std::vector<T>;          // 模板别名（typedef 做不到）
template <typename T> using SMap = std::map<std::string, T>;

// 用户定义字面量
constexpr long double operator"" _km(long double v) { return v * 1000.0L; }

// =============================================================================
// main
// =============================================================================
int main() {
    std::cout << "__cplusplus = " << __cplusplus << "\n";

    // =========================================================================
    demo::title("1.1 auto —— 编译期类型推导");
    // 【用在哪】迭代器、lambda、模板返回值、长得离谱的类型；
    //          不要用在「类型本身就是重要信息」的地方（比如接口返回值）。
    // =========================================================================
    {
        auto i = 42;                 // int
        auto d = 3.14;               // double
        auto s = std::string("hi");  // std::string
        SHOW(i); SHOW(d); SHOW(s);

        std::map<std::string, std::vector<int>> m{{"a", {1, 2}}, {"b", {3}}};

        demo::section("没有 auto 的年代");
        for (std::map<std::string, std::vector<int>>::const_iterator it = m.begin();
             it != m.end(); ++it) {
            std::cout << "    " << it->first << " -> " << it->second.size() << " 个元素\n";
        }

        demo::section("有了 auto");
        for (auto it = m.begin(); it != m.end(); ++it) {
            std::cout << "    " << it->first << " -> " << it->second.size() << " 个元素\n";
        }

        demo::section("auto 的推导规则（重要）");
        const int ci = 10;
        auto        a1 = ci;   // int          —— 顶层 const 被丢掉
        const auto  a2 = ci;   // const int
        auto&       a3 = ci;   // const int&   —— 引用会保留 const
        auto&&      a4 = ci;   // const int&   —— 万能引用 + 引用折叠
        (void)a1; (void)a2; (void)a3; (void)a4;
        SHOW(std::is_same<decltype(a1), int>::value);
        SHOW(std::is_same<decltype(a3), const int&>::value);

        demo::line("坑：auto x = v[i]; 遇到 vector<bool> 拿到的是代理对象，不是 bool。");
    }

    // =========================================================================
    demo::title("1.2 decltype —— 推导表达式的类型（不求值）");
    // 【用在哪】泛型代码里描述「和某某一样的类型」，尤其是返回类型。
    // =========================================================================
    {
        int x = 0;
        decltype(x)   y = 1;     // int
        decltype((x)) z = x;     // int&  —— 多一层括号就变成引用！
        z = 99;
        SHOW(x);                 // 被改成 99，说明 z 确实是引用
        SHOW(y);
        SHOW(std::is_same<decltype(x), int>::value);
        SHOW(std::is_same<decltype((x)), int&>::value);
    }

    // =========================================================================
    demo::title("1.3 右值引用 / 移动语义 —— C++11 最重要的特性");
    // 【用在哪】所有「持有资源」的类；容器扩容；函数返回大对象；
    //          std::move 把不再需要的对象的资源转走。
    // =========================================================================
    {
        demo::section("值类别速记");
        demo::line("lvalue  = 有名字、能取地址的东西            例: 变量 x");
        demo::line("prvalue = 纯右值，临时的、马上要死的        例: 42, x+1, f()返回值");
        demo::line("xvalue  = 将亡值，本来有名字但被判了死刑    例: std::move(x)");
        demo::line("能被移动的 = prvalue + xvalue = rvalue");

        demo::section("拷贝 vs 移动 —— 看输出对比开销");
        {
            Buffer a(1024, "A");
            std::cout << "  Buffer b = a;            // 拷贝\n";
            Buffer b = a;
            std::cout << "  Buffer c = std::move(a); // 移动\n";
            Buffer c = std::move(a);
            (void)b; (void)c;
            demo::line("注意：a 被移动后处于「有效但未指定」状态，只能赋值或析构，别再用它的值。");
        }

        demo::section("std::move 其实什么都没搬，它只是个类型转换");
        demo::line("template<class T> constexpr remove_reference_t<T>&& move(T&& t) noexcept");
        demo::line("{ return static_cast<remove_reference_t<T>&&>(t); }");
        demo::line("真正干活的是随后被选中的「移动构造/移动赋值」重载。");

        demo::section("vector 扩容时会调用移动构造（前提：noexcept）");
        {
            std::vector<Buffer> v;
            v.reserve(1);                       // 故意只留 1 个位置，制造扩容
            v.emplace_back(16, "元素0");
            std::cout << "  --- 触发扩容 ---\n";
            v.emplace_back(16, "元素1");
        }

        demo::section("函数返回值：RVO / 移动");
        {
            Buffer r = make_buffer(64);         // 通常连移动都省了（返回值优化 RVO）
            SHOW(r.size());
        }
    }

    // =========================================================================
    demo::title("1.4 完美转发 —— 写「转发型」函数模板必备");
    // 【用在哪】工厂函数(make_unique)、包装器、日志装饰、线程池 submit。
    // =========================================================================
    {
        int lv = 1;

        demo::section("直接调用（作为对照）");
        overloaded(lv);              // int&
        overloaded(42);              // int&&

        demo::section("不用 forward —— 右值性丢失了");
        bad_forward(lv);
        bad_forward(42);             // 期望 int&&，实际 int&  <-- BUG

        demo::section("用 std::forward —— 正确");
        good_forward(lv);
        good_forward(42);            // 正确得到 int&&

        demo::section("原理：引用折叠");
        demo::line("T&  &  -> T&      T&  && -> T&");
        demo::line("T&& &  -> T&      T&& && -> T&&");
        demo::line("传左值 int: T 推成 int& , T&& = int& && = int&");
        demo::line("传右值 42 : T 推成 int  , T&& = int&&");
        demo::line("注意：只有「模板参数 T&&」和「auto&&」才是万能引用；");
        demo::line("      void f(std::string&&) 里的 && 是纯右值引用，不是万能引用。");
    }

    // =========================================================================
    demo::title("1.5 统一初始化 {} 与 initializer_list");
    // 【用在哪】几乎所有初始化；尤其是想禁止窄化转换的场合。
    // =========================================================================
    {
        int         a{5};
        double      b{1.5};
        std::vector<int>    v{1, 2, 3};
        std::map<int, std::string> m{{1, "one"}, {2, "two"}};
        SHOW(a); SHOW(b); SHOW(v.size()); SHOW(m.size());

        struct P { int x; int y; };
        P p{1, 2};
        SHOW(p.x); SHOW(p.y);

        demo::section("花括号禁止窄化 —— 这是个好特性");
        demo::line("int narrow{3.14};   // 编译错误（= 号写法只会给个警告）");
        int ok = static_cast<int>(3.14);
        SHOW(ok);

        demo::section("经典陷阱：() 和 {} 对 vector 意义完全不同");
        std::vector<int> v1(10, 5);     // 10 个 5
        std::vector<int> v2{10, 5};     // 2 个元素：10 和 5
        SHOW(v1.size()); SHOW(v2.size());
        demo::line("规则：只要类有 initializer_list 构造，{} 会【优先】选它。");
    }

    // =========================================================================
    demo::title("1.6 Lambda 表达式");
    // 【用在哪】STL 算法的谓词、回调、局部小函数、延迟执行、线程任务。
    // =========================================================================
    {
        demo::line("语法: [捕获](参数) mutable noexcept -> 返回类型 { 函数体 }");

        int x = 10, y = 20;

        auto f1 = [](int v) { return v * 2; };            // 无捕获
        auto f2 = [x](int v) { return v + x; };           // 值捕获（副本，默认只读）
        auto f3 = [&y](int v) { y += v; return y; };      // 引用捕获
        auto f4 = [=]() { return x + y; };                // 全部按值
        auto f5 = [&]() { x += 1; y += 1; };              // 全部按引用
        auto f6 = [x]() mutable { x += 100; return x; };  // mutable 才能改值捕获的副本

        SHOW(f1(5));
        SHOW(f2(5));
        SHOW(f3(5));   SHOW(y);
        SHOW(f4());
        f5();          SHOW(x); SHOW(y);
        SHOW(f6());    SHOW(x);   // 外面的 x 没变，改的是副本

        demo::section("配合 STL 算法（lambda 最主要的用途）");
        std::vector<int> v{5, 3, 8, 1, 9, 2};
        std::sort(v.begin(), v.end(), [](int a, int b) { return a > b; });   // 降序
        std::cout << "    降序: ";
        for (int n : v) std::cout << n << " ";
        std::cout << "\n";

        auto cnt = std::count_if(v.begin(), v.end(), [](int n) { return n > 4; });
        SHOW(cnt);

        demo::section("lambda 本质是一个「匿名类的对象」");
        demo::line("[x](int a){ return a + x; }  编译器生成:");
        demo::line("  class __lambda { int x; public:");
        demo::line("    __lambda(int x_):x(x_){}");
        demo::line("    auto operator()(int a) const { return a + x; } };");
        demo::line("所以：无捕获的 lambda 可以隐式转成函数指针，有捕获的不行。");

        demo::section("危险：引用捕获的生命周期");
        demo::line("存起来/异步执行的 lambda 千万别用 [&]，因为局部变量早就没了。");
        demo::line("经验：同步立即使用 -> [&] 没问题；跨作用域存活 -> 值捕获或 [p = std::move(x)]。");

        demo::section("递归 lambda（C++11 需要 std::function）");
        std::function<int(int)> fac = [&fac](int n) { return n <= 1 ? 1 : n * fac(n - 1); };
        SHOW(fac(5));
    }

    // =========================================================================
    demo::title("1.7 nullptr / enum class / static_assert / using");
    // =========================================================================
    {
        demo::section("nullptr —— 类型安全的空指针");
        int* p = nullptr;
        SHOW(p == nullptr);
        demo::line("NULL 就是整数 0，会造成重载歧义；nullptr 的类型是 std::nullptr_t。");

        demo::section("enum class —— 强类型枚举");
        enum class Color : unsigned char { Red, Green, Blue };  // 可指定底层类型
        enum class Status { Ok, Error };
        Color c = Color::Red;
        SHOW(static_cast<int>(c));
        demo::line("必须写 Color::Red（不污染外层作用域）；不隐式转 int（避免误用）；");
        demo::line("Color 和 Status 可以有同名成员而不冲突。");

        demo::section("static_assert —— 编译期断言");
        static_assert(sizeof(int) >= 4, "本程序假设 int 至少 4 字节");
        static_assert(factorial(5) == 120, "constexpr 在编译期就算好了");
        demo::line("编译期就报错，比运行期 assert 早得多。");

        demo::section("using 类型别名");
        Vec<int>  vi{1, 2, 3};
        SMap<int> sm{{"a", 1}};
        SHOW(vi.size()); SHOW(sm.size());
        using Handler = void (*)(int, const char*);      // 函数指针，可读性远胜 typedef
        Handler h = nullptr;
        SHOW(h == nullptr);
    }

    // =========================================================================
    demo::title("1.8 constexpr —— 把计算搬到编译期");
    // 【用在哪】编译期常量、数组大小、模板参数、查找表、零开销抽象。
    // =========================================================================
    {
        constexpr int n = factorial(5);
        int arr[n];                       // 用作数组大小，说明是真·编译期常量
        SHOW(n);
        SHOW(sizeof(arr) / sizeof(arr[0]));

        demo::line("const     = 「运行期不可修改」");
        demo::line("constexpr = 「编译期可以算出来」（同时也 const）");
        demo::line("constexpr 函数也可以在运行期调用，只是那时就是普通函数。");
        int runtime_n = 5;
        SHOW(factorial(runtime_n));       // 运行期调用，合法
    }

    // =========================================================================
    demo::title("1.9 基于范围的 for");
    // =========================================================================
    {
        std::vector<std::string> v{"alpha", "beta", "gamma"};

        std::cout << "    只读 (const auto&): ";
        for (const auto& s : v) std::cout << s << " ";
        std::cout << "\n";

        for (auto& s : v) s += "!";           // 修改要用 auto&
        std::cout << "    修改后:             ";
        for (const auto& s : v) std::cout << s << " ";
        std::cout << "\n";

        demo::line("用 auto 会拷贝每个元素（string 拷贝很贵），默认写 const auto&。");
        demo::line("等价展开: auto&& __r = v; for(auto __b=begin(__r),__e=end(__r); __b!=__e; ++__b)");
        demo::line("自定义类型只要提供 begin()/end() 就能用范围 for。");
    }

    // =========================================================================
    demo::title("1.10 智能指针 —— 告别 new/delete");
    // 【用在哪】所有动态分配。unique_ptr 是默认选择，需要共享才上 shared_ptr。
    // =========================================================================
    {
        demo::section("unique_ptr：独占所有权，零运行期开销");
        {
            std::unique_ptr<Buffer> up(new Buffer(32, "unique"));
            SHOW(up->size());
            SHOW(static_cast<bool>(up));

            std::unique_ptr<Buffer> up2 = std::move(up);   // 所有权转移
            SHOW(static_cast<bool>(up));                   // 现在是空的
            SHOW(static_cast<bool>(up2));
        }   // 自动 delete

        demo::section("自定义删除器 —— 管 C 风格资源");
        {
            auto closer = [](std::FILE* f) { if (f) { std::cout << "    [关闭文件]\n"; std::fclose(f); } };
            std::unique_ptr<std::FILE, decltype(closer)> fp(std::fopen("ch01_tmp.txt", "w"), closer);
            if (fp) std::fputs("hello", fp.get());
        }
        std::remove("ch01_tmp.txt");

        demo::section("shared_ptr：引用计数共享");
        {
            std::shared_ptr<Buffer> s1(new Buffer(8, "shared"));
            SHOW(s1.use_count());
            {
                auto s2 = s1;
                SHOW(s1.use_count());     // 2
            }
            SHOW(s1.use_count());         // 1

            demo::section("weak_ptr：观察但不延长生命周期，用于打破循环引用");
            std::weak_ptr<Buffer> w = s1;
            SHOW(w.expired());
            if (auto locked = w.lock()) SHOW(locked->size());   // 提升为 shared_ptr 再用
            s1.reset();
            SHOW(w.expired());
        }

        demo::section("循环引用会内存泄漏 —— 面试高频");
        demo::line("  父 --shared_ptr--> 子");
        demo::line("  子 --shared_ptr--> 父     引用计数永远回不到 0，两个都不会析构");
        demo::line("  修正：反向那条边（子指父）改成 weak_ptr。");

        demo::section("三条经验");
        demo::line("1) 默认用 unique_ptr，只有确实需要共享所有权才用 shared_ptr");
        demo::line("2) 用 make_shared / make_unique(C++14)，不要裸 new");
        demo::line("3) 函数参数：只用不接管 -> 传裸指针或引用；接管 -> 传 unique_ptr by value");
    }

    // =========================================================================
    demo::title("1.11 变参模板");
    // 【用在哪】printf 的类型安全替代、make_xxx 工厂、tuple、事件系统。
    // =========================================================================
    {
        print_all(1, 2.5, "三", 'x', true);

        auto w = my_make_unique<Widget>(3, 4);
        SHOW(w->sum());

        demo::line("sizeof...(Args) 拿参数包大小；Args... 展开；");
        demo::line("C++11 靠递归展开，C++17 有了折叠表达式就简单多了（见第 3 章）。");
    }

    // =========================================================================
    demo::title("1.12 default / delete / override / final / 委托构造");
    // =========================================================================
    {
        demo::section("= delete 禁止拷贝（RAII 类型的常规操作）");
        NonCopyable nc1;
        NonCopyable nc2 = std::move(nc1);       // 移动可以
        // NonCopyable nc3 = nc2;               // 编译错误：拷贝被删除
        (void)nc2;
        demo::line("对比老写法：把拷贝构造声明为 private 且不实现 —— 报错信息很难看。");

        demo::section("委托构造：消除重复初始化代码");
        Widget w1(1, 2);
        Widget w2(5);
        Widget w3;
        SHOW(w1.sum()); SHOW(w2.sum()); SHOW(w3.sum());

        demo::section("override / final");
        Derived d(7);
        Base& b = d;
        b.f(1);          // 走 Derived::f
        b.g();
        SHOW(b.x_);
        demo::line("override 的价值：如果你签名写错（比如 f(long)），编译器立刻报错，");
        demo::line("而不是默默生成一个「新的虚函数」让你调试半天。");
    }

    // =========================================================================
    demo::title("1.13 多线程 —— C++11 终于有了标准并发");
    // 【用在哪】任何并行任务。注意：能不共享就不共享，共享就必须同步。
    // =========================================================================
    {
        demo::section("std::thread 基本用法");
        std::thread t([] {
            std::cout << "    子线程在跑，id=" << std::this_thread::get_id() << "\n";
        });
        std::cout << "    主线程 id=" << std::this_thread::get_id() << "\n";
        t.join();       // 必须 join 或 detach，否则 thread 析构时 std::terminate

        demo::section("数据竞争 vs 互斥锁");
        {
            int unsafe_counter = 0;
            int safe_counter   = 0;
            std::mutex mtx;
            std::atomic<int> atomic_counter{0};

            auto worker = [&] {
                for (int i = 0; i < 10000; ++i) {
                    ++unsafe_counter;                              // 数据竞争！结果不确定
                    { std::lock_guard<std::mutex> lk(mtx); ++safe_counter; }  // RAII 锁
                    atomic_counter.fetch_add(1, std::memory_order_relaxed);   // 无锁原子
                }
            };
            std::vector<std::thread> ts;
            for (int i = 0; i < 4; ++i) ts.emplace_back(worker);
            for (auto& th : ts) th.join();

            std::cout << "    期望值        = " << 4 * 10000 << "\n";
            std::cout << "    无保护 counter = " << unsafe_counter << "  <-- 很可能小于期望值\n";
            std::cout << "    mutex  counter = " << safe_counter << "\n";
            std::cout << "    atomic counter = " << atomic_counter.load() << "\n";
        }

        demo::section("condition_variable —— 生产者消费者");
        {
            std::mutex m;
            std::condition_variable cv;
            std::vector<int> queue;
            bool done = false;

            std::thread consumer([&] {
                for (;;) {
                    std::unique_lock<std::mutex> lk(m);
                    // 带谓词的 wait 能防「虚假唤醒」和「丢失通知」
                    cv.wait(lk, [&] { return !queue.empty() || done; });
                    if (queue.empty() && done) break;
                    int v = queue.back(); queue.pop_back();
                    lk.unlock();
                    std::cout << "    消费: " << v << "\n";
                }
            });

            for (int i = 1; i <= 3; ++i) {
                { std::lock_guard<std::mutex> lk(m); queue.push_back(i); }
                cv.notify_one();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            { std::lock_guard<std::mutex> lk(m); done = true; }
            cv.notify_all();
            consumer.join();
        }

        demo::section("future / async —— 要「结果」而不是要「线程」时用它");
        {
            auto fut = std::async(std::launch::async, [] {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                return 42;
            });
            std::cout << "    主线程可以先干别的...\n";
            SHOW(fut.get());        // 阻塞等结果
        }

        demo::section("promise —— 手动在一个线程里给另一个线程投递结果");
        {
            std::promise<std::string> prom;
            auto fut = prom.get_future();
            std::thread producer([&prom] { prom.set_value("来自子线程的数据"); });
            SHOW(fut.get());
            producer.join();
        }

        demo::section("内存序（先记结论，深入以后再看）");
        demo::line("relaxed  : 只保证这个变量本身是原子的，不保证和其它读写的顺序");
        demo::line("acquire  : 之后的读写不能被重排到它前面（读端用）");
        demo::line("release  : 之前的读写不能被重排到它后面（写端用）");
        demo::line("acq_rel  : 读改写操作两头都管");
        demo::line("seq_cst  : 全局单一顺序，默认值，最慢也最不容易错");
        demo::line("经验：不确定就用默认 seq_cst；只有计数器这种才敢用 relaxed。");
    }

    // =========================================================================
    demo::title("1.14 其它零碎但常用的");
    // =========================================================================
    {
        demo::section("原始字符串字面量");
        const char* re   = R"(\d+\.\d+)";                // 反斜杠不用转义
        const char* path = R"(C:\Users\test\file.txt)";
        std::cout << "    正则: " << re << "\n";
        std::cout << "    路径: " << path << "\n";
        std::cout << "    自定义定界符: " << R"json({"key": "va)lue"})json" << "\n";

        demo::section("noexcept");
        demo::line("void f() noexcept;   // 承诺不抛异常，抛了直接 terminate");
        demo::line("价值：移动构造标 noexcept，vector 扩容才敢用移动而不是拷贝。");
        SHOW(noexcept(1 + 1));

        demo::section("用户定义字面量");
        long double dist = 5.0_km;
        SHOW(static_cast<double>(dist));

        demo::section("std::array —— 定长数组，比 C 数组好用");
        std::array<int, 5> arr{1, 2, 3, 4, 5};
        SHOW(arr.size());
        SHOW(arr.at(2));
        SHOW(arr.front()); SHOW(arr.back());
        demo::line("有 size()，不会退化成指针，可以整体赋值，可以放进容器。");

        demo::section("alignas / alignof");
        SHOW(alignof(double));
        struct alignas(64) CacheLine { char pad[64]; };
        SHOW(alignof(CacheLine));
        demo::line("多线程里给热点变量按 cache line 对齐，可避免 false sharing。");

        demo::section("类内成员初始化");
        struct Config { int retries = 3; std::string host{"localhost"}; };
        Config cfg;
        SHOW(cfg.retries); SHOW(cfg.host);

        demo::section("显式转换运算符");
        struct Handle {
            bool valid = true;
            explicit operator bool() const { return valid; }
        };
        Handle hd;
        if (hd) demo::line("if (hd) 可以，但 int i = hd; 不行 —— explicit 挡住了误用");

        demo::section("<cstdint> 定宽整数（写协议/二进制格式必用）");
        SHOW(sizeof(std::int8_t));  SHOW(sizeof(std::int32_t)); SHOW(sizeof(std::int64_t));
        demo::line("int/long 的大小随平台变；写文件格式和网络协议一律用 int32_t 这种。");
    }

    std::cout << "\n第 1 章结束。\n";
    return 0;
}
