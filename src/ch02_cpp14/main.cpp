// =============================================================================
// 第 2 章 —— C++14：给 C++11 打补丁
//
// C++14 是个「小版本」，没有革命性特性，但补上的几个洞非常实用：
//   泛型 lambda、初始化捕获（移动捕获）、返回类型推导、make_unique、
//   放宽的 constexpr、变量模板。
//
// 运行： ch02_cpp14.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <map>
#include <set>
#include <algorithm>
#include <utility>
#include <type_traits>
#include <cassert>
#include <functional>
#include <initializer_list>

// =============================================================================
// 1) 返回类型推导：函数也能用 auto 了（C++11 只有 lambda 能）
// =============================================================================
auto square(int n) { return n * n; }            // 返回 int

// 递归函数用 auto 返回也可以，但第一个 return 必须能定型
auto fib(int n) -> int {                        // 递归时建议还是写出来
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

// decltype(auto)：完整保留引用与 const（auto 会退化掉）
struct Holder {
    std::vector<int> data{1, 2, 3};
    std::vector<int>& get() { return data; }
};

auto           get_by_auto(Holder& h)     { return h.get(); }   // 返回 vector（拷贝!）
decltype(auto) get_by_decltype(Holder& h) { return h.get(); }   // 返回 vector&（引用）

// =============================================================================
// 2) 变量模板 —— 标准库里的 xxx_v 后缀就是靠它
// =============================================================================
template <typename T>
constexpr T pi = T(3.1415926535897932385L);

// 自己实现一个 is_integral_v 感受一下
template <typename T>
constexpr bool my_is_integral_v = std::is_integral<T>::value;

// =============================================================================
// 3) 放宽的 constexpr —— 现在可以写循环、局部变量、多条语句了
// =============================================================================
constexpr int fib_iter(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; ++i) {       // C++11 里这是非法的
        int t = a + b;
        a = b;
        b = t;
    }
    return a;
}

// 编译期生成查找表 —— constexpr 的经典用途
constexpr int kTableSize = 16;
struct SquareTable {
    int values[kTableSize]{};
    constexpr SquareTable() {
        for (int i = 0; i < kTableSize; ++i) values[i] = i * i;
    }
};
constexpr SquareTable kSquares{};

// =============================================================================
// 4) 泛型 lambda 的一个实用场景：通用的「打印任何容器」
// =============================================================================
auto print_container = [](const auto& c, const char* name) {
    std::cout << "    " << name << " = [ ";
    for (const auto& e : c) std::cout << e << " ";
    std::cout << "]\n";
};

// =============================================================================
// main
// =============================================================================
int main() {
    std::cout << "__cplusplus = " << __cplusplus << "\n";

    // =========================================================================
    demo::title("2.1 泛型 lambda —— 参数写 auto");
    // 【用在哪】写一次比较器/转换器给多种类型用；替代大量小模板函数。
    // =========================================================================
    {
        auto plus = [](auto a, auto b) { return a + b; };
        SHOW(plus(1, 2));
        SHOW(plus(1.5, 2.5));
        SHOW(plus(std::string("hello "), "world"));

        demo::section("原理：编译器生成的闭包类里 operator() 是模板");
        demo::line("class __lambda { public:");
        demo::line("  template<class A, class B> auto operator()(A a, B b) const { return a+b; } };");

        demo::section("实用：一个 lambda 打印任何容器");
        std::vector<int>         v{1, 2, 3};
        std::set<std::string>    s{"a", "b"};
        print_container(v, "vector<int>");
        print_container(s, "set<string>");

        demo::section("完美转发版泛型 lambda");
        auto forwarder = [](auto&& f, auto&&... args) {
            return f(std::forward<decltype(args)>(args)...);
        };
        SHOW(forwarder([](int a, int b) { return a * b; }, 6, 7));
    }

    // =========================================================================
    demo::title("2.2 初始化捕获 —— 终于能「移动捕获」了");
    // 【用在哪】把 unique_ptr / 大对象搬进 lambda；异步任务；线程池。
    // =========================================================================
    {
        demo::section("C++11 的痛点：只能拷贝捕获或引用捕获");
        demo::line("unique_ptr 不能拷贝 -> C++11 里根本没法把它捕获进 lambda");

        demo::section("C++14 的解法");
        auto up = std::make_unique<std::vector<int>>(std::initializer_list<int>{1, 2, 3});
        auto task = [p = std::move(up)]() {          // 移动进闭包，闭包成为所有者
            return p->size();
        };
        SHOW(static_cast<bool>(up));                 // 已经被移空
        SHOW(task());

        demo::section("还能捕获「新算出来的值」");
        int x = 10;
        auto f = [doubled = x * 2, msg = std::string("计算结果")]() {
            std::cout << "    " << msg << " = " << doubled << "\n";
        };
        f();

        demo::section("捕获 this 的成员副本（避免悬垂 this）");
        demo::line("[copy = this->member]  // 只带走需要的那个成员，不带整个 this");
    }

    // =========================================================================
    demo::title("2.3 函数返回类型推导 与 decltype(auto)");
    // =========================================================================
    {
        SHOW(square(7));
        SHOW(fib(10));

        Holder h;
        demo::section("auto 会丢引用 -> 产生拷贝");
        auto copy = get_by_auto(h);
        copy.push_back(999);
        SHOW(h.data.size());          // 还是 3，说明改的是副本

        demo::section("decltype(auto) 保留引用 -> 真的改到原对象");
        decltype(auto) ref = get_by_decltype(h);
        ref.push_back(999);
        SHOW(h.data.size());          // 变 4

        demo::line("规律：auto 按「值语义」推导；decltype(auto) 按「表达式原样」推导。");
        demo::line("写泛型转发函数的返回类型时用 decltype(auto)。");
    }

    // =========================================================================
    demo::title("2.4 变量模板");
    // =========================================================================
    {
        SHOW(pi<float>);
        SHOW(pi<double>);
        SHOW(static_cast<double>(pi<long double>));

        SHOW(my_is_integral_v<int>);
        SHOW(my_is_integral_v<double>);

        demo::line("标准库从 C++17 起大量提供 _v 后缀：");
        demo::line("  std::is_integral<T>::value   ->   std::is_integral_v<T>");
        demo::line("  少打 ::value 七个字符，而且报错信息更短。");
    }

    // =========================================================================
    demo::title("2.5 放宽的 constexpr");
    // =========================================================================
    {
        static_assert(fib_iter(10) == 55, "编译期就算好了");
        SHOW(fib_iter(20));

        demo::section("编译期查找表");
        static_assert(kSquares.values[7] == 49, "");
        SHOW(kSquares.values[7]);
        SHOW(kSquares.values[15]);
        demo::line("运行期一次乘法都不用做，表在 .rdata 段里已经躺好了。");
        demo::line("【用在哪】CRC 表、三角函数表、状态机跳转表、编译期字符串哈希。");
    }

    // =========================================================================
    demo::title("2.6 std::make_unique");
    // =========================================================================
    {
        auto p = std::make_unique<std::string>("hello");
        SHOW(*p);

        auto arr = std::make_unique<int[]>(5);
        arr[0] = 42;
        SHOW(arr[0]);

        demo::section("为什么要用 make_unique 而不是 unique_ptr<T>(new T)");
        demo::line("1) 不用写两遍类型名");
        demo::line("2) 异常安全：f(unique_ptr<A>(new A), g()) 中，如果 g() 抛异常，");
        demo::line("   而 new A 已经执行但还没交给 unique_ptr，就泄漏了。");
        demo::line("   make_unique 把「分配 + 接管」打包成一步，不存在这个窗口。");
        demo::line("3) make_shared 还能把控制块和对象合并成一次分配（性能更好）。");
    }

    // =========================================================================
    demo::title("2.7 二进制字面量 与 数字分隔符");
    // =========================================================================
    {
        int flags = 0b1010'1100;
        long big  = 1'000'000'000;
        SHOW(flags);
        SHOW(big);
        SHOW(0xFF'FF);

        demo::line("【用在哪】寄存器位定义、协议标志位、掩码 —— 二进制字面量比 0xAC 直观。");
    }

    // =========================================================================
    demo::title("2.8 异构查找（Transparent Comparator）");
    // =========================================================================
    {
        // 默认 std::less<std::string> 只接受 std::string，
        // 传 const char* 会先构造一个临时 std::string（一次堆分配！）
        std::set<std::string> normal{"apple", "banana"};

        // std::less<> 是「透明比较器」，允许异构比较，不构造临时对象
        std::set<std::string, std::less<>> transparent{"apple", "banana"};

        SHOW(normal.find("apple") != normal.end());
        SHOW(transparent.find("apple") != transparent.end());   // 无临时 string

        demo::line("【用在哪】高频用字面量查 map<string, T> 的热点代码。");
        demo::line("C++20 起 unordered_map 也支持（需要透明 hash + equal）。");
    }

    // =========================================================================
    demo::title("2.9 其它小工具");
    // =========================================================================
    {
        demo::section("std::exchange —— 设新值，返回旧值");
        int x = 1;
        int old = std::exchange(x, 99);
        SHOW(old); SHOW(x);
        demo::line("【用在哪】移动构造里最常用：data_(std::exchange(o.data_, nullptr))");

        demo::section("[[deprecated]]");
        demo::line("[[deprecated(\"请改用 new_api\")]] void old_api();");
        demo::line("调用处会得到编译警告，是渐进式重构大工程的利器。");

        demo::section("std::integer_sequence —— 编译期整数序列");
        demo::line("配合 index_sequence 可以在编译期展开 tuple（C++17 有 std::apply 就不用手写了）");

        demo::section("std::quoted —— 带引号的字符串 IO");
        demo::line("std::cout << std::quoted(s);  // 自动加引号并转义");
    }

    std::cout << "\n第 2 章结束。\n";
    return 0;
}
