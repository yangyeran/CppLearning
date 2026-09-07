// =============================================================================
// 第 3 章 —— C++17：实用主义大丰收
//
// C++17 没有 C++11 那么颠覆，但每一条都是「日常代码里天天能用」的：
//   结构化绑定、if 初始化语句、CTAD、if constexpr、折叠表达式、
//   optional / variant / any、string_view、filesystem、并行算法。
//
// 运行： ch03_cpp17.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <optional>
#include <variant>
#include <any>
#include <tuple>
#include <array>
#include <algorithm>
#include <numeric>
#include <filesystem>
#include <type_traits>
#include <charconv>
#include <mutex>
#include <fstream>

namespace fs = std::filesystem;

// =============================================================================
// if constexpr —— 编译期分支
// =============================================================================
template <typename T>
std::string describe(const T& v) {
    if constexpr (std::is_pointer_v<T>) {
        // 只有 T 真是指针时，这一分支才会被编译
        return std::string("指针，指向的值 = ") + std::to_string(*v);
    } else if constexpr (std::is_integral_v<T>) {
        return "整数 " + std::to_string(v);
    } else if constexpr (std::is_floating_point_v<T>) {
        return "浮点 " + std::to_string(v);
    } else {
        return "其它类型";
    }
}

// 对比：C++17 之前只能用 SFINAE，写起来非常丑
template <typename T>
std::enable_if_t<std::is_integral_v<T>, std::string> old_style(T v) {
    return "整数 " + std::to_string(v);
}
template <typename T>
std::enable_if_t<!std::is_integral_v<T>, std::string> old_style(T) {
    return "非整数";
}

// =============================================================================
// 折叠表达式
// =============================================================================
template <typename... Ts>
auto sum(Ts... ts) { return (ts + ...); }                    // 一元右折叠

template <typename... Ts>
auto sum_safe(Ts... ts) { return (0 + ... + ts); }           // 二元，空包也安全

template <typename... Ts>
void print_all(Ts&&... ts) {                                 // 逗号折叠
    ((std::cout << ts << " "), ...);
    std::cout << "\n";
}

template <typename... Ts>
bool all_true(Ts... ts) { return (ts && ...); }

template <typename T, typename... Ts>
bool is_any_of(const T& v, const Ts&... candidates) {
    return ((v == candidates) || ...);
}

// =============================================================================
// CTAD 与自定义推导指引
// =============================================================================
template <typename T>
struct Wrapper {
    T value;
    explicit Wrapper(T v) : value(std::move(v)) {}
};
// 让 Wrapper("abc") 推导成 Wrapper<std::string> 而不是 Wrapper<const char*>
Wrapper(const char*) -> Wrapper<std::string>;

// =============================================================================
// variant 的 overloaded 惯用法（非常常用，值得背下来）
// =============================================================================
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// =============================================================================
// inline 变量：头文件里定义全局变量不再冲突
// =============================================================================
inline constexpr int kMaxRetries = 3;
struct Counter {
    static inline int total = 0;      // 类内静态成员就地定义，不用在 cpp 里再写一遍
};

// =============================================================================
// 嵌套命名空间简写
// =============================================================================
namespace company::project::module {
    int value() { return 42; }
}

// =============================================================================
// optional 的典型用法：可能失败的解析
// =============================================================================
std::optional<int> parse_int(std::string_view sv) {
    int result{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec != std::errc{} || ptr != sv.data() + sv.size()) return std::nullopt;
    return result;
}

// =============================================================================
// main
// =============================================================================
int main() {
    std::cout << "__cplusplus = " << __cplusplus << "\n";

    // =========================================================================
    demo::title("3.1 结构化绑定 —— 一次拆开多个值");
    // 【用在哪】遍历 map、接收 pair/tuple 返回值、拆结构体。几乎天天用。
    // =========================================================================
    {
        std::map<std::string, int> scores{{"张三", 90}, {"李四", 85}};

        demo::section("C++17 之前");
        for (const auto& kv : scores) {
            std::cout << "    " << kv.first << " : " << kv.second << "\n";
        }

        demo::section("C++17");
        for (const auto& [name, score] : scores) {
            std::cout << "    " << name << " : " << score << "\n";
        }

        demo::section("拆 tuple / pair");
        std::tuple<int, double, std::string> t{1, 2.5, "three"};
        auto [i, d, s] = t;
        SHOW(i); SHOW(d); SHOW(s);

        demo::section("拆结构体（按声明顺序，必须是公有成员）");
        struct Point { int x; int y; };
        auto [px, py] = Point{3, 4};
        SHOW(px); SHOW(py);

        demo::section("绑引用 —— 可以直接修改");
        std::vector<std::pair<std::string, int>> v{{"a", 1}, {"b", 2}};
        for (auto& [k, n] : v) n *= 10;
        for (const auto& [k, n] : v) std::cout << "    " << k << "=" << n << "\n";

        demo::section("接收 insert 的 (迭代器, 是否插入成功)");
        auto [it, inserted] = scores.insert({"王五", 70});
        SHOW(inserted);
        SHOW(it->first);
    }

    // =========================================================================
    demo::title("3.2 if / switch 带初始化语句");
    // 【用在哪】把只在分支里用的临时变量限制在分支作用域内，避免污染。
    // =========================================================================
    {
        std::map<std::string, int> m{{"a", 1}};

        demo::section("C++17 之前：it 泄漏到外层作用域");
        {
            auto it = m.find("a");
            if (it != m.end()) std::cout << "    找到 " << it->second << "\n";
            // it 在这里还活着，容易误用
        }

        demo::section("C++17");
        if (auto it = m.find("a"); it != m.end()) {
            std::cout << "    找到 " << it->second << "\n";
        } else {
            std::cout << "    没找到（else 分支里 it 也可见）\n";
        }

        if (auto [it2, ok] = m.insert({"b", 2}); ok) {
            std::cout << "    插入成功: " << it2->first << "\n";
        }

        demo::section("配合锁使用很自然");
        std::mutex mtx;
        if (std::lock_guard lk{mtx}; true) {      // 顺便演示 CTAD
            std::cout << "    临界区\n";
        }
    }

    // =========================================================================
    demo::title("3.3 CTAD 类模板参数推导");
    // 【用在哪】少打一堆尖括号。lock_guard / pair / vector / tuple 最受益。
    // =========================================================================
    {
        std::pair p{1, 2.0};                       // pair<int,double>
        std::vector v{1, 2, 3};                    // vector<int>
        std::tuple t{1, "a", 2.0};
        std::array a{1, 2, 3};                     // array<int,3>
        SHOW(p.first); SHOW(v.size()); SHOW(std::get<0>(t)); SHOW(a.size());

        demo::section("自定义推导指引");
        Wrapper w1{42};                            // Wrapper<int>
        Wrapper w2{"hello"};                       // Wrapper<std::string>（靠推导指引）
        SHOW(w1.value);
        SHOW(w2.value);
        SHOW(std::is_same_v<decltype(w2), Wrapper<std::string>>);
    }

    // =========================================================================
    demo::title("3.4 if constexpr —— 杀手级特性");
    // 【用在哪】写模板时按类型走不同逻辑；替代 SFINAE 和 tag dispatch。
    // =========================================================================
    {
        int x = 42;
        std::cout << "    " << describe(x) << "\n";
        std::cout << "    " << describe(&x) << "\n";
        std::cout << "    " << describe(3.14) << "\n";
        std::cout << "    " << describe(std::string("s")) << "\n";

        demo::section("和普通 if 的区别");
        demo::line("普通 if：两个分支都要能编译通过（哪怕运行时不走）");
        demo::line("if constexpr：不成立的分支【不实例化】，里面写什么都行");
        demo::line("所以 describe 里 *v 能通过，因为非指针时那段根本不编译。");

        demo::section("旧写法对照");
        std::cout << "    " << old_style(1) << "\n";
        std::cout << "    " << old_style(1.5) << "\n";
        demo::line("SFINAE 版需要写两个函数 + enable_if，报错信息还极难读。");
    }

    // =========================================================================
    demo::title("3.5 折叠表达式 —— 变参模板不用再递归了");
    // =========================================================================
    {
        SHOW(sum(1, 2, 3, 4, 5));
        SHOW(sum_safe());                          // 空包，返回 0
        SHOW(sum(1.5, 2.5));
        print_all("折叠打印:", 1, 2.5, 'c', true);
        SHOW(all_true(true, true, false));
        SHOW(is_any_of(3, 1, 2, 3, 4));
        SHOW(is_any_of(9, 1, 2, 3, 4));

        demo::section("四种折叠形式");
        demo::line("(E op ...)        一元右折叠  E1 op (E2 op E3)");
        demo::line("(... op E)        一元左折叠  (E1 op E2) op E3");
        demo::line("(E op ... op I)   二元右折叠  带初值，空包安全");
        demo::line("(I op ... op E)   二元左折叠  带初值，空包安全");
    }

    // =========================================================================
    demo::title("3.6 std::optional —— 「可能没有值」");
    // 【用在哪】查找、解析、配置项、可选参数。替代 「返回 -1 / 空指针 / 传出参数」。
    // =========================================================================
    {
        auto a = parse_int("12345");
        auto b = parse_int("abc");

        SHOW(a.has_value());
        SHOW(b.has_value());
        if (a) SHOW(*a);
        SHOW(a.value_or(-1));
        SHOW(b.value_or(-1));

        demo::section("常见 API");
        std::optional<std::string> os;
        SHOW(os.has_value());
        os = "hello";
        SHOW(*os);
        SHOW(os->size());
        os.reset();
        SHOW(os.has_value());
        os.emplace("world");                       // 原地构造
        SHOW(*os);

        demo::section("为什么它比返回指针好");
        demo::line("1) 值语义，不涉及所有权和生命周期问题");
        demo::line("2) 类型上就写明了「可能没有」，调用方必须处理");
        demo::line("3) 不需要额外的哨兵值（-1 / \"\" / nullptr 这类约定）");
        demo::line("注意：optional 会占 sizeof(T) + 对齐后的一个 bool，不是零成本。");
        SHOW(sizeof(int));
        SHOW(sizeof(std::optional<int>));
    }

    // =========================================================================
    demo::title("3.7 std::variant —— 类型安全的 union");
    // 【用在哪】状态机、AST 节点、解析结果、消息类型、「要么 A 要么 B」的返回值。
    // =========================================================================
    {
        std::variant<int, std::string, double> v = 42;
        SHOW(v.index());
        SHOW(std::get<int>(v));

        v = std::string("hello");
        SHOW(v.index());
        SHOW(std::get<std::string>(v));

        demo::section("安全取值");
        if (auto* p = std::get_if<std::string>(&v)) SHOW(*p);
        if (std::holds_alternative<int>(v)) demo::line("是 int");
        else demo::line("不是 int");

        demo::section("std::visit + overloaded（最常用的写法）");
        auto printer = overloaded{
            [](int i)                { std::cout << "    int    : " << i << "\n"; },
            [](const std::string& s) { std::cout << "    string : " << s << "\n"; },
            [](double d)             { std::cout << "    double : " << d << "\n"; },
        };
        std::visit(printer, v);
        v = 3.14;  std::visit(printer, v);
        v = 7;     std::visit(printer, v);

        demo::section("用 variant 建一个小状态机");
        struct Idle {};
        struct Running { int progress; };
        struct Done { std::string result; };
        using State = std::variant<Idle, Running, Done>;

        std::vector<State> states{Idle{}, Running{50}, Done{"成功"}};
        for (const auto& st : states) {
            std::visit(overloaded{
                [](Idle)              { std::cout << "    状态: 空闲\n"; },
                [](const Running& r)  { std::cout << "    状态: 运行中 " << r.progress << "%\n"; },
                [](const Done& d)     { std::cout << "    状态: 完成 " << d.result << "\n"; },
            }, st);
        }
        demo::line("优点：状态各自带自己的数据；漏处理一个分支编译期就报错。");
    }

    // =========================================================================
    demo::title("3.8 std::any —— 装任意类型（少用）");
    // =========================================================================
    {
        std::any a = 42;
        SHOW(std::any_cast<int>(a));
        a = std::string("hello");
        SHOW(std::any_cast<std::string>(a));
        SHOW(a.has_value());

        try { std::any_cast<int>(a); }
        catch (const std::bad_any_cast& e) { std::cout << "    类型不对: " << e.what() << "\n"; }

        demo::line("和 variant 的区别：variant 类型集合是【封闭且已知】的，编译期检查、无堆分配；");
        demo::line("any 是【开放】的，靠 RTTI，通常有堆分配。能用 variant 就别用 any。");
    }

    // =========================================================================
    demo::title("3.9 std::string_view —— 零拷贝字符串视图");
    // 【用在哪】所有「只读字符串参数」。是性能优化里投入产出比最高的一招。
    // =========================================================================
    {
        auto count_chars = [](std::string_view sv) { return sv.size(); };

        SHOW(count_chars("字面量，不产生任何分配"));
        std::string s = "已有的 std::string";
        SHOW(count_chars(s));                       // 不拷贝

        demo::section("substr 是 O(1) 的");
        std::string_view sv = "hello world";
        auto sub = sv.substr(0, 5);
        SHOW(sub);
        demo::line("std::string::substr 会分配新字符串；string_view::substr 只挪指针。");

        demo::section("常用操作");
        SHOW(sv.starts_with("hello"));              // C++20 起，MSVC 也支持
        SHOW(sv.find("world"));
        SHOW(sv.length());

        demo::section("三个坑（必须记住）");
        demo::line("1) 不拥有数据：源字符串销毁后 view 就悬垂了");
        demo::line("   错误示例: string_view f(){ std::string s=\"x\"; return s; }  // 悬垂!");
        demo::line("2) 不保证以 '\\0' 结尾：不能直接传给需要 C 字符串的 API");
        demo::line("   要传就 std::string(sv).c_str()");
        demo::line("3) 别用它做「成员变量」，除非你能保证被指向的字符串活得更久");
    }

    // =========================================================================
    demo::title("3.10 std::filesystem —— 终于有标准文件系统 API");
    // =========================================================================
    {
        fs::path p = fs::temp_directory_path() / "cpp17_demo";
        fs::create_directories(p / "sub");

        {
            std::ofstream(p / "a.txt")       << "hello";
            std::ofstream(p / "sub" / "b.txt") << "world!!";
        }

        SHOW(p.string());
        SHOW(fs::exists(p));
        SHOW(fs::is_directory(p));
        SHOW(fs::file_size(p / "a.txt"));

        demo::section("路径操作");
        fs::path f = p / "a.txt";
        SHOW(f.filename().string());
        SHOW(f.stem().string());
        SHOW(f.extension().string());
        SHOW(f.parent_path().string());

        demo::section("递归遍历目录");
        for (const auto& e : fs::recursive_directory_iterator(p)) {
            std::cout << "    " << (e.is_directory() ? "[D] " : "[F] ")
                      << e.path().filename().string();
            if (e.is_regular_file()) std::cout << "  " << e.file_size() << " bytes";
            std::cout << "\n";
        }

        fs::remove_all(p);
        SHOW(fs::exists(p));
        demo::line("跨平台：不用再为 Windows 的反斜杠和 Linux 的斜杠写两套代码。");
    }

    // =========================================================================
    demo::title("3.11 并行算法");
    // =========================================================================
    {
        std::vector<int> v(100000);
        std::iota(v.begin(), v.end(), 1);

        // 注意：MSVC 支持 <execution>；某些 GCC 版本需要链接 TBB，
        //       所以这里用宏保护，保证到处都能编译。
#if defined(__cpp_lib_execution) && __has_include(<execution>)
        // #include <execution> 已在支持时才有意义，这里直接用串行版演示语义
        demo::line("本编译器支持 <execution>，可写 std::sort(std::execution::par, ...)");
#else
        demo::line("本编译器未启用 <execution>（GCC 需链接 TBB）");
#endif
        auto total = std::reduce(v.begin(), v.end(), 0LL);
        SHOW(total);

        demo::section("四种执行策略");
        demo::line("seq       : 串行（和不带策略一样）");
        demo::line("par       : 多线程并行");
        demo::line("par_unseq : 并行 + 向量化（元素间不能有任何依赖和加锁）");
        demo::line("unseq     : 仅向量化（C++20）");
        demo::line("注意：并行版对小数据反而更慢（线程开销）；");
        demo::line("      传给它的函数不能抛异常、不能加锁、不能有数据竞争。");

        demo::section("std::reduce vs std::accumulate");
        demo::line("accumulate 保证【从左到右依次】累加 -> 无法并行");
        demo::line("reduce     不保证顺序 -> 可并行，但要求运算满足结合律和交换律");
        demo::line("浮点加法不满足结合律，所以 reduce 的浮点结果可能和 accumulate 有微小差异。");
    }

    // =========================================================================
    demo::title("3.12 其它 C++17 实用点");
    // =========================================================================
    {
        demo::section("inline 变量 / 类内静态成员就地定义");
        ++Counter::total; ++Counter::total;
        SHOW(Counter::total);
        SHOW(kMaxRetries);

        demo::section("嵌套命名空间简写");
        SHOW(company::project::module::value());

        demo::section("保证的拷贝省略");
        demo::line("C++17 起 `T x = T(...)` 保证不产生临时对象，");
        demo::line("所以「不可移动不可拷贝」的类型也能从函数返回了。");

        demo::section("属性");
        demo::line("[[nodiscard]]     返回值被忽略就警告（错误码、只读查询函数必加）");
        demo::line("[[maybe_unused]]  抑制「未使用」警告");
        demo::line("[[fallthrough]]   switch 里故意贯穿，告诉编译器不是忘写 break");

        demo::section("std::clamp / gcd / lcm");
        SHOW(std::clamp(15, 0, 10));
        SHOW(std::clamp(-5, 0, 10));
        SHOW(std::gcd(12, 18));
        SHOW(std::lcm(4, 6));

        demo::section("std::apply —— 把 tuple 展开成函数参数");
        auto add3 = [](int a, int b, int c) { return a + b + c; };
        SHOW(std::apply(add3, std::tuple{1, 2, 3}));

        demo::section("from_chars / to_chars —— 最快的数值转换");
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), 123456);
        std::cout << "    to_chars: " << std::string(buf, ptr) << "\n";
        demo::line("不分配内存、不看 locale、不抛异常 —— 比 stoi/sprintf 快数倍。");
        demo::line("【用在哪】日志、JSON 序列化、协议解析等热点路径。");

        demo::section("map / set 新接口");
        std::map<std::string, std::string> m;
        m.try_emplace("k", "v");                 // 键存在就什么都不做，不构造 value
        m.insert_or_assign("k", "v2");
        SHOW(m["k"]);
        demo::line("m[key] 在键不存在时会【默认构造并插入】，只读查询千万别用它。");
    }

    std::cout << "\n第 3 章结束。\n";
    return 0;
}
