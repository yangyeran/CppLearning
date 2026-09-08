// =============================================================================
// 第 5 章 —— C++23：打磨与补齐
//
// C++23 的主题是「把 C++20 没做完的做完」：
//   std::expected（错误处理）、std::print（输出）、ranges 补全、
//   deducing this、std::generator、std::mdspan、flat_map……
//
// 【重要】各家编译器对 C++23 的支持进度不一样。
//   本章所有特性都用 __has_include / __cpp_lib_xxx 做了检测，
//   缺失的会打印「本编译器暂不支持」而不是编译失败。
//
// 运行： ch05_cpp23.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <map>
#include <algorithm>
#include <numeric>
#include <optional>
#include <ranges>
#include <utility>
#include <cstdint>
#include <type_traits>
#include <version>          // 提供所有 __cpp_lib_xxx 特性测试宏
// ---- 特性探测 ---------------------------------------------------------------
//
// 【为什么不能只用 __has_include】
//   __has_include(<expected>) 只说明「这个头文件存在」，不说明「特性可用」。
//   libstdc++ 的 <expected> 内部还有一层 `#if __cplusplus > 202002L` 的门，
//   在 C++20 模式下（或某些 clang + libstdc++ 组合下）头文件能包含成功，
//   但里面什么都没声明 —— 于是 std::expected 未定义，编译在使用处才炸。
//   本章第一次跑 Linux CI 时就是这么失败的：
//     error: no template named 'expected' in namespace 'std'
//
//   正确做法是两级判断：
//     1. __has_include  决定能不能 #include（不存在就别包含，否则预处理报错）
//     2. __cpp_lib_xxx  决定特性是否真的可用（由 <version> 或该头文件定义）
//
//   这两个宏的分工是标准明确规定的，凡是做特性探测都应该这么写。
// -----------------------------------------------------------------------------

// std::expected / std::unexpected
#if __has_include(<expected>)
#  include <expected>
#endif
#ifdef __cpp_lib_expected
#  define HAS_EXPECTED 1
#else
#  define HAS_EXPECTED 0
#endif

// std::print / std::println
#if __has_include(<print>)
#  include <print>
#endif
#ifdef __cpp_lib_print
#  define HAS_PRINT 1
#else
#  define HAS_PRINT 0
#endif

// std::generator（协程生成器）
#if __has_include(<generator>)
#  include <generator>
#endif
#ifdef __cpp_lib_generator
#  define HAS_GENERATOR 1
#else
#  define HAS_GENERATOR 0
#endif

// std::mdspan（多维视图）
#if __has_include(<mdspan>)
#  include <mdspan>
#endif
#ifdef __cpp_lib_mdspan
#  define HAS_MDSPAN 1
#else
#  define HAS_MDSPAN 0
#endif

// std::flat_map / flat_set
#if __has_include(<flat_map>)
#  include <flat_map>
#endif
#ifdef __cpp_lib_flat_map
#  define HAS_FLATMAP 1
#else
#  define HAS_FLATMAP 0
#endif

// std::stacktrace
#if __has_include(<stacktrace>)
#  include <stacktrace>
#endif
#ifdef __cpp_lib_stacktrace
#  define HAS_STACKTRACE 1
#else
#  define HAS_STACKTRACE 0
#endif
namespace rv = std::views;

// =============================================================================
// std::expected 的示例：一个可能失败的配置解析
// =============================================================================
#if HAS_EXPECTED
enum class ParseError { Empty, NotANumber, OutOfRange };

std::string_view to_string(ParseError e) {
    switch (e) {
        case ParseError::Empty:       return "输入为空";
        case ParseError::NotANumber:  return "不是数字";
        case ParseError::OutOfRange:  return "超出范围";
    }
    return "未知错误";
}

std::expected<int, ParseError> parse_port(std::string_view sv) {
    if (sv.empty()) return std::unexpected(ParseError::Empty);
    int v = 0;
    for (char c : sv) {
        if (c < '0' || c > '9') return std::unexpected(ParseError::NotANumber);
        v = v * 10 + (c - '0');
        if (v > 65535) return std::unexpected(ParseError::OutOfRange);
    }
    return v;
}
#endif

// =============================================================================
// deducing this —— 显式对象参数
// =============================================================================
#if defined(__cpp_explicit_this_parameter) && __cpp_explicit_this_parameter >= 202110L
#  define HAS_DEDUCING_THIS 1
struct Container {
    std::vector<int> data{1, 2, 3};

    // 一个函数同时覆盖 非const / const / 右值 三种重载
    template <typename Self>
    auto&& items(this Self&& self) { return std::forward<Self>(self).data; }
};
#else
#  define HAS_DEDUCING_THIS 0
#endif

// =============================================================================
// std::generator —— 标准协程生成器
// =============================================================================
#if HAS_GENERATOR
std::generator<int> counted(int n) {
    for (int i = 0; i < n; ++i) co_yield i;
}
#endif

// =============================================================================
// if consteval
// =============================================================================
#if defined(__cpp_if_consteval)
constexpr int dual_path(int n) {
    if consteval {
        return n * 100;      // 编译期走这条
    } else {
        return n;            // 运行期走这条
    }
}
#endif

// =============================================================================
// 静态 operator()（无 this，调用更便宜）
// =============================================================================
#if defined(__cpp_static_call_operator)
struct Less {
    static bool operator()(int a, int b) { return a < b; }
};
#endif

static void unsupported(std::string_view feature) {
    std::cout << "    [跳过] 本编译器暂不支持 " << feature << "\n";
}

// =============================================================================
// main
// =============================================================================
int main() {
    std::cout << "__cplusplus = " << __cplusplus << "\n";

    // =========================================================================
    demo::title("5.1 std::expected —— 带错误信息的返回值");
    // 【用在哪】所有「可能失败并且失败原因重要」的函数。
    //          比 optional 多了错误值，比异常轻量且在签名上可见。
    // =========================================================================
    {
#if HAS_EXPECTED
        for (std::string_view s : {"8080", "", "80a0", "99999"}) {
            auto r = parse_port(s);
            if (r) {
                std::cout << "    \"" << s << "\" -> 端口 " << *r << "\n";
            } else {
                std::cout << "    \"" << s << "\" -> 失败: " << to_string(r.error()) << "\n";
            }
        }

        demo::section("常用 API");
        auto ok  = parse_port("443");
        auto bad = parse_port("x");
        SHOW(ok.has_value());
        SHOW(*ok);
        SHOW(bad.value_or(-1));
        SHOW(static_cast<int>(bad.error()));

        demo::section("单子式接口：链式处理，不用层层 if");
        auto pipeline = parse_port("8080")
                            .transform([](int p) { return p + 1; })        // 成功时变换
                            .and_then([](int p) -> std::expected<int, ParseError> {
                                return p * 2;                              // 继续可能失败的操作
                            });
        if (pipeline) SHOW(*pipeline);

        auto recovered = parse_port("bad")
                             .or_else([](ParseError) -> std::expected<int, ParseError> {
                                 return 80;                                // 失败时给个默认值
                             });
        if (recovered) SHOW(*recovered);

        demo::section("三种错误处理方式的取舍");
        demo::line("异常     : 不污染返回类型，但有栈展开开销，且调用方看不出会不会抛");
        demo::line("错误码   : 快，但容易被忽略，也带不了额外信息");
        demo::line("expected : 类型里写明了「要么值要么错」，编译器逼你处理，零动态分配");
        demo::line("经验：可预期的失败（解析、查找、IO）用 expected；");
        demo::line("      真正的异常情况（内存耗尽、不变量被破坏）用异常。");
#else
        unsupported("<expected>");
#endif
    }

    // =========================================================================
    demo::title("5.2 std::print / println —— 直接输出，不经过 iostream");
    // =========================================================================
    {
#if HAS_PRINT
        std::println("    Hello, {}!", "C++23");
        std::println("    {} 个文件，耗时 {:.2f} 秒", 42, 1.2345);
        std::print("    不换行");
        std::println("  <- 接着输出");
        demo::line("比 std::cout << std::format(...) 更短，而且实现上直接写 stdout，更快。");
#else
        unsupported("<print>");
        demo::line("替代写法: std::cout << std::format(\"...\", args) << '\\n';");
#endif
    }

    // =========================================================================
    demo::title("5.3 Ranges 补全 —— C++20 缺的几件套");
    // =========================================================================
    {
        std::vector<int>         a{1, 2, 3, 4, 5};
        std::vector<std::string> b{"一", "二", "三"};

#if defined(__cpp_lib_ranges_to_container)
        demo::section("ranges::to —— 视图终于能直接变容器了");
        auto v = a | rv::transform([](int n) { return n * n; })
                   | std::ranges::to<std::vector>();
        std::cout << "    平方: ";
        for (int n : v) std::cout << n << " ";
        std::cout << "\n";
        demo::line("C++20 得写 std::vector<int> v(r.begin(), r.end())，而且有些视图还不行。");
#else
        unsupported("ranges::to");
#endif

#if defined(__cpp_lib_ranges_enumerate)
        demo::section("views::enumerate —— 带下标遍历");
        for (auto [i, s] : b | rv::enumerate) {
            std::cout << "    [" << i << "] " << s << "\n";
        }
#else
        unsupported("views::enumerate");
#endif

#if defined(__cpp_lib_ranges_zip)
        demo::section("views::zip —— 并行遍历多个容器");
        for (auto [n, s] : rv::zip(a, b)) {
            std::cout << "    " << n << " <-> " << s << "\n";
        }
#else
        unsupported("views::zip");
#endif

#if defined(__cpp_lib_ranges_chunk)
        demo::section("views::chunk / slide —— 分块与滑动窗口");
        std::cout << "    chunk(2): ";
        for (auto blk : a | rv::chunk(2)) {
            std::cout << "[";
            for (int n : blk) std::cout << n << " ";
            std::cout << "] ";
        }
        std::cout << "\n";
#else
        unsupported("views::chunk");
#endif

        demo::line("其它 C++23 视图: stride / adjacent / chunk_by / join_with / cartesian_product");
    }

    // =========================================================================
    demo::title("5.4 deducing this —— 显式对象参数");
    // 【用在哪】消除 const / 非const / 左值 / 右值 四份重复的重载；递归 lambda。
    // =========================================================================
    {
#if HAS_DEDUCING_THIS
        Container c;
        c.items().push_back(4);
        SHOW(c.items().size());

        const Container cc;
        SHOW(cc.items().size());        // 自动得到 const 版本

        demo::section("递归 lambda 一行搞定");
        auto fac = [](this auto self, int n) -> int { return n <= 1 ? 1 : n * self(n - 1); };
        SHOW(fac(5));
        demo::line("C++11~20 要靠 std::function（有类型擦除开销）或 Y 组合子。");
#else
        unsupported("deducing this");
        demo::line("语法: template<class Self> auto&& get(this Self&& self) { ... }");
        demo::line("作用: 一个函数模板同时充当 T&, const T&, T&& 四个重载。");
#endif
    }

    // =========================================================================
    demo::title("5.5 std::generator —— 标准协程生成器");
    // =========================================================================
    {
#if HAS_GENERATOR
        std::cout << "    ";
        for (int n : counted(5)) std::cout << n << " ";
        std::cout << "\n";
        demo::line("再也不用自己手写 promise_type 了（对比第 4 章那一大坨）。");
#else
        unsupported("<generator>");
        demo::line("写法: std::generator<int> f(){ for(int i=0;i<n;++i) co_yield i; }");
#endif
    }

    // =========================================================================
    demo::title("5.6 std::mdspan 与多维下标");
    // 【用在哪】科学计算、图像处理、矩阵运算 —— 把一维缓冲当多维数组看。
    // =========================================================================
    {
#if HAS_MDSPAN && defined(__cpp_multidimensional_subscript)
        std::vector<float> data(12);
        std::iota(data.begin(), data.end(), 1.0f);
        std::mdspan m(data.data(), 3, 4);        // 3 行 4 列
        for (size_t r = 0; r < m.extent(0); ++r) {
            std::cout << "    ";
            for (size_t c = 0; c < m.extent(1); ++c) std::cout << m[r, c] << "\t";
            std::cout << "\n";
        }
        demo::line("零拷贝：mdspan 只是「看法」，底下还是那块连续内存。");
#else
        unsupported("<mdspan>");
        demo::line("语法: std::mdspan m(ptr, rows, cols);  m[r, c] = v;");
        demo::line("配套特性：多维下标运算符 operator[](size_t, size_t)");
#endif
    }

    // =========================================================================
    demo::title("5.7 flat_map / flat_set —— 缓存友好的关联容器");
    // =========================================================================
    {
#if HAS_FLATMAP
        std::flat_map<int, std::string> fm{{3, "三"}, {1, "一"}, {2, "二"}};
        for (const auto& [k, v] : fm) std::cout << "    " << k << " -> " << v << "\n";
#else
        unsupported("<flat_map>");
#endif
        demo::line("实现：内部是两个排序好的 vector（一个存 key 一个存 value）。");
        demo::line("优点：遍历和查找的缓存局部性远好于红黑树，内存占用也小。");
        demo::line("缺点：插入删除是 O(n)（要挪动元素），迭代器容易失效。");
        demo::line("【用在哪】建好之后基本只读、元素不多（几百个以内）的映射表。");
    }

    // =========================================================================
    demo::title("5.8 其它 C++23 小改进");
    // =========================================================================
    {
#if defined(__cpp_if_consteval)
        demo::section("if consteval");
        constexpr int ct = dual_path(5);
        int rt_input = 5;
        SHOW(ct);                      // 500，编译期路径
        SHOW(dual_path(rt_input));     // 5，运行期路径
#else
        unsupported("if consteval");
#endif

#if defined(__cpp_static_call_operator)
        demo::section("静态 operator()");
        std::vector<int> v{3, 1, 2};
        std::sort(v.begin(), v.end(), Less{});
        std::cout << "    排序结果: ";
        for (int n : v) std::cout << n << " ";
        std::cout << "\n";
        demo::line("无捕获的函数对象不需要 this，标 static 可省一次参数传递。");
#else
        unsupported("static operator()");
#endif

        demo::section("std::to_underlying —— enum 转底层类型");
#if defined(__cpp_lib_to_underlying)
        enum class Level : int { Low = 1, High = 9 };
        SHOW(std::to_underlying(Level::High));
        demo::line("以前要写 static_cast<std::underlying_type_t<Level>>(x)，又长又容易写错。");
#else
        unsupported("std::to_underlying");
#endif

        demo::section("std::byteswap —— 大小端转换");
#if defined(__cpp_lib_byteswap)
        SHOW(std::byteswap(std::uint32_t(0x12345678)));
        demo::line("【用在哪】网络协议、二进制文件格式。第 8 章会详细讲字节序。");
#else
        unsupported("std::byteswap");
#endif

        demo::section("string / string_view::contains");
#if defined(__cpp_lib_string_contains)
        std::string s = "hello world";
        SHOW(s.contains("world"));
        demo::line("以前要写 s.find(\"world\") != std::string::npos");
#else
        unsupported("string::contains");
#endif

        demo::section("std::stacktrace —— 标准调用栈");
#if HAS_STACKTRACE
        demo::line("std::cout << std::stacktrace::current();  // 打印当前调用栈");
        demo::line("（这里不实际打印，输出会很长）");
#else
        unsupported("<stacktrace>");
#endif

        demo::section("import std;  —— 标准库模块");
        demo::line("C++23 起标准库本身也是模块，一行 import std; 取代几十行 #include。");
        demo::line("编译速度提升显著，但需要构建系统配合，目前 MSVC 支持最好。");

        demo::section("其它");
        demo::line("std::unreachable()          告诉编译器这里不可能到达（优化用）");
        demo::line("[[assume(cond)]]            给优化器的假设");
        demo::line("std::move_only_function     能装 move-only 可调用的 std::function");
        demo::line("std::out_ptr / inout_ptr    适配 C API 的 T** 输出参数");
        demo::line("1uz                         size_t 字面量后缀");
        demo::line("多维下标 operator[](a, b)    终于不用写 operator()(a,b) 了");
    }

    // =========================================================================
    demo::title("5.9 展望 C++26（已定案的大件）");
    // =========================================================================
    {
        demo::line("反射 (Reflection)  : ^^Type 拿到元信息，编译期遍历成员 —— 序列化不用再写宏了");
        demo::line("契约 (Contracts)   : pre(x > 0) / post(r != nullptr) / contract_assert");
        demo::line("std::execution     : sender/receiver 异步框架，标准化的「结构化并发」");
        demo::line("std::hive          : 高性能的「稳定引用」容器，游戏引擎用得多");
        demo::line("std::inplace_vector: 固定容量、栈上分配的 vector");
    }

    std::cout << "\n第 5 章结束。\n";
    return 0;
}
