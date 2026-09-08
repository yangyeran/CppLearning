// =============================================================================
// 第 4 章 —— C++20：第二次革命
//
// 四大特性（俗称 "Big Four"）：Concepts / Ranges / Coroutines / Modules
// 加上一堆实用改进：<=>、std::format、std::span、jthread、位操作、日历……
//
// 运行： ch04_cpp20.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <vector>
#include <array>
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <algorithm>
#include <numeric>
#include <concepts>
#include <ranges>
#include <compare>
#include <span>
#include <bit>
#include <numbers>
#include <source_location>
#include <coroutine>
#include <thread>
#include <mutex>
#include <latch>
#include <barrier>
#include <semaphore>
#include <atomic>
#include <chrono>
#include <type_traits>
#include <cstddef>
#include <iterator>
#include <utility>

#if __has_include(<format>)
#  include <format>
#  define HAS_FORMAT 1
#else
#  define HAS_FORMAT 0
#endif

namespace rv = std::views;
namespace rng = std::ranges;

// =============================================================================
// 1) Concepts —— 给模板参数加约束
// =============================================================================

// 最简单的形式：直接组合标准概念
template <typename T>
concept Numeric = std::integral<T> || std::floating_point<T>;

// requires 表达式：描述「这个类型必须支持哪些操作」
template <typename T>
concept Addable = requires(T a, T b) {
    { a + b } -> std::convertible_to<T>;    // 复合要求：表达式合法且返回类型可转 T
    { a += b };                             // 简单要求：表达式合法即可
};

template <typename T>
concept Container = requires(T c) {
    typename T::value_type;                 // 类型要求：必须有嵌套类型
    c.begin();
    c.end();
    { c.size() } -> std::convertible_to<std::size_t>;
};

// 四种使用语法，效果完全一样
template <Numeric T>              T f1(T v) { return v * 2; }   // 模板参数位置
template <typename T> requires Numeric<T>
                                  T f2(T v) { return v * 2; }   // requires 子句
template <typename T> T f3(T v) requires Numeric<T> { return v * 2; }  // 尾置 requires
auto f4(Numeric auto v) { return v * 2; }                       // 缩写函数模板（最简洁）

// 概念还能做重载决议：更「特化」的概念优先
template <std::integral T>       std::string kind(T) { return "整数"; }
template <std::floating_point T> std::string kind(T) { return "浮点"; }

template <Container C>
void show_container(const C& c, std::string_view name) {
    std::cout << "    " << name << "(size=" << c.size() << "): ";
    for (const auto& e : c) std::cout << e << " ";
    std::cout << "\n";
}

// =============================================================================
// 2) 三向比较 <=>
// =============================================================================
struct Point {
    int x, y;
    // 一行搞定 == != < <= > >= 全部六个运算符
    auto operator<=>(const Point&) const = default;
    bool operator==(const Point&) const = default;
};

struct Version {
    int major, minor, patch;
    // 手写：先比 major，再比 minor，最后 patch
    std::strong_ordering operator<=>(const Version& o) const {
        if (auto c = major <=> o.major; c != 0) return c;
        if (auto c = minor <=> o.minor; c != 0) return c;
        return patch <=> o.patch;
    }
    bool operator==(const Version&) const = default;
};

// =============================================================================
// 3) 协程 —— 一个最小可用的 Generator
// =============================================================================
template <typename T>
struct Generator {
    struct promise_type {
        T current_value{};

        Generator get_return_object() {
            return Generator{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend()   noexcept { return {}; }
        std::suspend_always yield_value(T v) { current_value = v; return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    using handle_t = std::coroutine_handle<promise_type>;
    handle_t h{};

    explicit Generator(handle_t hh) : h(hh) {}
    ~Generator() { if (h) h.destroy(); }
    Generator(Generator&& o) noexcept : h(std::exchange(o.h, {})) {}
    Generator(const Generator&) = delete;

    // 让它能用范围 for
    struct iterator {
        handle_t h;
        bool operator==(std::default_sentinel_t) const { return !h || h.done(); }
        iterator& operator++() { h.resume(); return *this; }
        void operator++(int) { h.resume(); }
        const T& operator*() const { return h.promise().current_value; }
    };
    iterator begin() { if (h) h.resume(); return iterator{h}; }
    std::default_sentinel_t end() { return {}; }
};

Generator<int> fibonacci(int count) {
    int a = 0, b = 1;
    for (int i = 0; i < count; ++i) {
        co_yield a;                 // 挂起，把 a 交出去；下次 resume 从这里继续
        int t = a + b; a = b; b = t;
    }
}

Generator<std::string> lines_of(std::string text) {
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        co_yield text.substr(pos, nl - pos);
        pos = nl + 1;
    }
}

// =============================================================================
// 4) consteval / constinit
// =============================================================================
consteval int must_be_compile_time(int n) { return n * n; }   // 只能编译期调用
constinit int g_initialized_at_compile_time = 42;             // 保证静态初始化

// =============================================================================
// 5) source_location —— 替代 __FILE__ / __LINE__ 宏
// =============================================================================
void log_msg(std::string_view msg,
             const std::source_location& loc = std::source_location::current()) {
    std::cout << "    [" << loc.file_name() << ":" << loc.line()
              << " " << loc.function_name() << "] " << msg << "\n";
}

// =============================================================================
// main
// =============================================================================
int main() {
    std::cout << "__cplusplus = " << __cplusplus << "\n";

    // =========================================================================
    demo::title("4.1 Concepts —— 让模板报错变成人话");
    // 【用在哪】所有对外的模板接口。约束写清楚，用错的人立刻知道错在哪。
    // =========================================================================
    {
        SHOW(f1(21));
        SHOW(f2(1.5));
        SHOW(f3(10));
        SHOW(f4(2.5));
        // f1(std::string("x"));   // 编译错误，信息是「不满足 Numeric」，一行就说清楚了

        demo::section("概念参与重载决议");
        std::cout << "    kind(1)   = " << kind(1) << "\n";
        std::cout << "    kind(1.5) = " << kind(1.5) << "\n";

        demo::section("requires 表达式的四种要求");
        demo::line("简单要求  : c.begin();                    表达式合法即可");
        demo::line("类型要求  : typename T::value_type;       该类型必须存在");
        demo::line("复合要求  : { c.size() } -> convertible_to<size_t>;  合法 + 返回类型约束");
        demo::line("嵌套要求  : requires std::copyable<T>;    另一个概念也要满足");

        demo::section("约束容器");
        std::vector<int> v{1, 2, 3};
        std::set<int>    s{4, 5};
        show_container(v, "vector");
        show_container(s, "set");

        demo::section("常用标准概念");
        SHOW(std::integral<int>);
        SHOW(std::floating_point<double>);
        SHOW(std::same_as<int, int>);
        SHOW(std::convertible_to<int, double>);
        SHOW(std::copyable<std::vector<int>>);
        SHOW(std::invocable<decltype([](int){}), int>);
        SHOW(rng::range<std::vector<int>>);
    }

    // =========================================================================
    demo::title("4.2 Ranges —— 管道式的算法组合");
    // 【用在哪】任何「过滤 + 变换 + 取前 N 个」的数据处理。代码量骤减。
    // =========================================================================
    {
        std::vector<int> v{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

        demo::section("传统写法：三段代码 + 两个中间容器");
        {
            std::vector<int> evens, squares;
            std::copy_if(v.begin(), v.end(), std::back_inserter(evens),
                         [](int n) { return n % 2 == 0; });
            std::transform(evens.begin(), evens.end(), std::back_inserter(squares),
                           [](int n) { return n * n; });
            squares.resize(3);
            std::cout << "    结果: ";
            for (int n : squares) std::cout << n << " ";
            std::cout << "\n";
        }

        demo::section("Ranges 写法：一行，零中间容器，惰性求值");
        auto result = v | rv::filter([](int n) { return n % 2 == 0; })
                        | rv::transform([](int n) { return n * n; })
                        | rv::take(3);
        std::cout << "    结果: ";
        for (int n : result) std::cout << n << " ";
        std::cout << "\n";

        demo::section("惰性求值：不遍历就一次都不算");
        int compute_count = 0;
        auto lazy = v | rv::transform([&](int n) { ++compute_count; return n * 2; });
        SHOW(compute_count);                       // 0，还没算
        auto first = *lazy.begin();
        SHOW(first);
        SHOW(compute_count);                       // 1，只算了第一个

        demo::section("算法直接吃容器，不用 begin/end");
        std::vector<int> u{5, 3, 8, 1};
        rng::sort(u);
        std::cout << "    排序后: ";
        for (int n : u) std::cout << n << " ";
        std::cout << "\n";
        SHOW(rng::count_if(u, [](int n) { return n > 3; }));
        SHOW(*rng::max_element(u));

        demo::section("投影 —— 按对象的某个成员排序/查找");
        struct Person { std::string name; int age; };
        std::vector<Person> ps{{"张三", 30}, {"李四", 25}, {"王五", 35}};
        rng::sort(ps, {}, &Person::age);           // 第 3 个参数是投影
        for (const auto& p : ps) std::cout << "    " << p.name << " " << p.age << "\n";
        auto it = rng::find(ps, 30, &Person::age);
        if (it != ps.end()) SHOW(it->name);

        demo::section("常用 views");
        auto show = [](std::string_view name, auto&& r) {
            std::cout << "    " << name << ": ";
            for (auto&& e : r) std::cout << e << " ";
            std::cout << "\n";
        };
        show("iota(1,6)",      rv::iota(1, 6));
        show("reverse",        v | rv::reverse | rv::take(3));
        show("drop(7)",        v | rv::drop(7));
        show("take_while<5",   v | rv::take_while([](int n) { return n < 5; }));
        show("drop_while<5",   v | rv::drop_while([](int n) { return n < 5; }));
        show("iota 无限+take", rv::iota(1) | rv::take(5));

        std::vector<std::vector<int>> nested{{1, 2}, {3, 4}, {5}};
        show("join 摊平",      nested | rv::join);

        std::map<std::string, int> m{{"a", 1}, {"b", 2}};
        show("keys",           m | rv::keys);
        show("values",         m | rv::values);

        demo::section("组合是可以命名和复用的");
        auto even_squares = rv::filter([](int n) { return n % 2 == 0; })
                          | rv::transform([](int n) { return n * n; });
        show("复用管道", v | even_squares);
    }

    // =========================================================================
    demo::title("4.3 协程 —— 可以「暂停和恢复」的函数");
    // 【用在哪】生成器（惰性序列）、异步 IO（避免回调地狱）、状态机。
    // =========================================================================
    {
        demo::section("生成器：斐波那契");
        std::cout << "    ";
        for (int n : fibonacci(10)) std::cout << n << " ";
        std::cout << "\n";

        demo::section("生成器：按行切分（不需要一次性构造 vector<string>）");
        for (const auto& line : lines_of("第一行\n第二行\n第三行")) {
            std::cout << "    [" << line << "]\n";
        }

        demo::section("三个关键字");
        demo::line("co_yield  产出一个值并挂起          -> 生成器");
        demo::line("co_await  等待一个可等待对象并挂起  -> 异步");
        demo::line("co_return 结束协程并返回值");
        demo::line("函数体里出现任意一个，这个函数就是协程。");

        demo::section("执行流程");
        demo::line("调用协程 -> 在堆上分配 coroutine frame（保存局部变量和恢复点）");
        demo::line("         -> 构造 promise_type -> get_return_object() 交给调用者");
        demo::line("         -> initial_suspend()  决定「立刻执行」还是「先挂起」");
        demo::line("遇 co_yield v -> promise.yield_value(v) -> 挂起，控制权还给调用者");
        demo::line("h.resume()    -> 从上次挂起点继续执行");
        demo::line("协程结束      -> final_suspend() -> h.destroy() 释放 frame");

        demo::section("代价与现状");
        demo::line("代价：frame 在堆上（编译器可能优化掉），每次挂起恢复有开销。");
        demo::line("现状：标准只给了底层机制，没给现成的库。");
        demo::line("      业务里一般用 cppcoro / asio 的协程支持 / C++23 的 std::generator。");
    }

    // =========================================================================
    demo::title("4.4 三向比较 <=>");
    // 【用在哪】任何需要排序或全套比较运算符的值类型。省掉 6 个函数的样板代码。
    // =========================================================================
    {
        Point a{1, 2}, b{1, 3};
        SHOW(a < b);  SHOW(a == b);  SHOW(a != b);  SHOW(a >= b);

        Version v1{1, 2, 3}, v2{1, 3, 0};
        SHOW(v1 < v2);
        SHOW((v1 <=> v2) < 0);

        demo::section("三种比较类别");
        demo::line("strong_ordering  : 等价即可互相替换      例: int, string");
        demo::line("weak_ordering    : 等价但不完全相同      例: 忽略大小写的字符串");
        demo::line("partial_ordering : 可能【不可比较】      例: 浮点数(NaN)");
        SHOW((1 <=> 2) < 0);
        SHOW(std::is_eq(1 <=> 1));
        auto nan_cmp = (0.0 / 0.0) <=> 1.0;
        SHOW(nan_cmp == std::partial_ordering::unordered);

        demo::section("= default 干了什么");
        demo::line("按成员声明顺序逐个比较（字典序），第一个不等的决定结果。");
        demo::line("同时自动合成 <  <=  >  >=；== 和 != 需要单独 default 一下。");
    }

    // =========================================================================
    demo::title("4.5 std::span —— 连续内存的「非拥有视图」");
    // 【用在哪】函数参数。一个 span 参数同时接受 vector / array / C 数组 / 裸指针+长度。
    // =========================================================================
    {
        auto sum_span = [](std::span<const int> s) {
            return std::accumulate(s.begin(), s.end(), 0);
        };
        auto double_all = [](std::span<int> s) { for (int& x : s) x *= 2; };

        std::vector<int> v{1, 2, 3, 4};
        int carr[]{5, 6, 7};
        std::array<int, 2> arr{8, 9};

        SHOW(sum_span(v));
        SHOW(sum_span(carr));
        SHOW(sum_span(arr));
        SHOW(sum_span({v.data(), 2}));           // 只看前两个

        double_all(v);
        std::cout << "    double_all 之后: ";
        for (int x : v) std::cout << x << " ";
        std::cout << "\n";

        demo::section("子视图");
        std::span<int> s{v};
        auto sub = s.subspan(1, 2);
        std::cout << "    subspan(1,2): ";
        for (int x : sub) std::cout << x << " ";
        std::cout << "\n";
        SHOW(s.front()); SHOW(s.back()); SHOW(s.size()); SHOW(s.size_bytes());

        demo::line("对比老写法 void f(int* p, size_t n) —— span 把两个参数绑成一个，不会传错。");
        demo::line("和 string_view 一样：不拥有数据，注意生命周期。");
    }

    // =========================================================================
    demo::title("4.6 std::format —— 类型安全的格式化");
    // =========================================================================
    {
#if HAS_FORMAT
        std::cout << "    " << std::format("{} + {} = {}", 1, 2, 3) << "\n";
        std::cout << "    " << std::format("[{:>10}]", "右对齐") << "\n";
        std::cout << "    " << std::format("[{:<10}]", "左对齐") << "\n";
        std::cout << "    " << std::format("[{:^10}]", "居中") << "\n";
        std::cout << "    " << std::format("{:.3f}", 3.1415926) << "\n";
        std::cout << "    " << std::format("{:#x}  {:#b}  {:#o}", 255, 5, 8) << "\n";
        std::cout << "    " << std::format("{:08.2f}", 3.1) << "\n";
        std::cout << "    " << std::format("{1} 在 {0} 之后", "A", "B") << "\n";
        std::cout << "    " << std::format("{:{}}|", "宽度动态", 12) << "\n";
        std::cout << "    " << std::format("{:+d} {:+d}", 5, -5) << "\n";

        demo::section("为什么比 printf / iostream 好");
        demo::line("printf   : 类型不匹配是未定义行为，%d 配 double 直接崩");
        demo::line("iostream : 类型安全但极其啰嗦，setw/setprecision 还是全局状态");
        demo::line("format   : 类型安全 + 简洁 + 编译期检查格式串 + 比两者都快");
#else
        demo::line("当前编译器没有 <format>，跳过。");
#endif
    }

    // =========================================================================
    demo::title("4.7 并发新工具");
    // =========================================================================
    {
        demo::section("jthread —— 自动 join + 可取消");
        {
            std::jthread jt([](std::stop_token st) {
                int i = 0;
                while (!st.stop_requested() && i < 1000) {
                    ++i;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::cout << "    子线程收到停止请求，循环了 " << i << " 次\n";
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }   // 析构自动 request_stop() + join()，不用手写
        demo::line("对比 std::thread：忘了 join 会 std::terminate，jthread 不会。");

        demo::section("latch —— 一次性倒数门闩");
        {
            std::latch done{3};
            for (int i = 0; i < 3; ++i) {
                std::thread([&done, i] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5 * (i + 1)));
                    std::cout << "    任务 " << i << " 完成\n";
                    done.count_down();
                }).detach();
            }
            done.wait();
            std::cout << "    全部完成\n";
        }

        demo::section("barrier —— 可重复使用的同步点（分阶段计算）");
        {
            const int kThreads = 3;
            // 完成回调必须是 noexcept，标准强制要求
            std::barrier bar{kThreads, []() noexcept {
                std::cout << "    --- 本阶段全部到齐 ---\n";
            }};
            std::vector<std::jthread> ts;
            for (int i = 0; i < kThreads; ++i) {
                ts.emplace_back([&bar, i] {
                    for (int phase = 0; phase < 2; ++phase) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2 * i));
                        bar.arrive_and_wait();
                    }
                });
            }
        }

        demo::section("counting_semaphore —— 限流");
        {
            std::counting_semaphore<4> sem{2};       // 同时最多 2 个
            std::atomic<int> concurrent{0}, peak{0};
            std::vector<std::jthread> ts;
            for (int i = 0; i < 5; ++i) {
                ts.emplace_back([&] {
                    sem.acquire();
                    int now = ++concurrent;
                    int old = peak.load();
                    while (now > old && !peak.compare_exchange_weak(old, now)) {}
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    --concurrent;
                    sem.release();
                });
            }
            for (auto& t : ts) t.join();
            std::cout << "    并发峰值 = " << peak.load() << "（信号量限制为 2）\n";
        }

        demo::section("atomic 的 wait / notify —— 轻量级等待，不用条件变量");
        {
            std::atomic<int> flag{0};
            std::jthread waiter([&] {
                flag.wait(0);                        // 阻塞直到值不再是 0
                std::cout << "    等待者被唤醒，flag = " << flag.load() << "\n";
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            flag.store(1);
            flag.notify_all();
        }
    }

    // =========================================================================
    demo::title("4.8 其它 C++20 实用点");
    // =========================================================================
    {
        demo::section("指定初始化器（从 C 借来的）");
        struct Config { int width = 800; int height = 600; bool fullscreen = false; };
        Config c{.width = 1920, .height = 1080};     // 必须按声明顺序，可跳过
        SHOW(c.width); SHOW(c.height); SHOW(c.fullscreen);
        demo::line("比 Builder 模式轻量得多，配置类结构体首选。");

        demo::section("using enum");
        enum class Color { Red, Green, Blue };
        {
            using enum Color;
            Color col = Red;                          // 不用写 Color::Red
            SHOW(static_cast<int>(col));
        }

        demo::section("consteval / constinit");
        constexpr int ct = must_be_compile_time(5);
        SHOW(ct);
        SHOW(g_initialized_at_compile_time);
        demo::line("consteval : 强制编译期求值，运行期调用直接编译错误");
        demo::line("constinit : 保证【静态初始化】，避开「静态初始化顺序灾难」");

        demo::section("模板 lambda");
        auto tl = []<typename T>(const std::vector<T>& v) { return v.size(); };
        SHOW(tl(std::vector<int>{1, 2, 3}));
        auto first_of = []<typename T>(const std::vector<T>& v) -> T { return v.front(); };
        SHOW(first_of(std::vector<std::string>{"a", "b"}));

        demo::section("位操作 <bit>");
        unsigned x = 0b1011'0000u;
        SHOW(std::popcount(x));            // 1 的个数
        SHOW(std::countl_zero(x));         // 前导 0
        SHOW(std::countr_zero(x));         // 末尾 0
        SHOW(std::has_single_bit(16u));    // 是不是 2 的幂
        SHOW(std::bit_ceil(100u));         // 向上取到 2 的幂
        SHOW(std::bit_width(255u));
        SHOW(std::rotl(0b1000'0001u, 1));
        SHOW(std::endian::native == std::endian::little);
        demo::line("以前要写一堆位操作 hack，现在编译器直接映射到 CPU 指令（popcnt/lzcnt）。");

        demo::section("数学常量 <numbers>");
        SHOW(std::numbers::pi);
        SHOW(std::numbers::e);
        SHOW(std::numbers::sqrt2);

        demo::section("source_location —— 替代 __FILE__/__LINE__ 宏");
        log_msg("这条日志自动带上了位置信息");

        demo::section("日历与时区 <chrono>");
        using namespace std::chrono;
        auto today = year_month_day{floor<days>(system_clock::now())};
        std::cout << "    今天: " << int(today.year()) << "-"
                  << unsigned(today.month()) << "-" << unsigned(today.day()) << "\n";
        auto d = 2026y / September / 7;
        auto next_week = sys_days{d} + days{7};
        auto nw = year_month_day{next_week};
        std::cout << "    2026-09-07 加 7 天 = " << int(nw.year()) << "-"
                  << unsigned(nw.month()) << "-" << unsigned(nw.day()) << "\n";

        demo::section("容器新接口");
        std::map<std::string, int> m{{"a", 1}};
        SHOW(m.contains("a"));                      // 比 find != end 直观
        std::string s = "prefix_body.txt";
        SHOW(s.starts_with("prefix"));
        SHOW(s.ends_with(".txt"));

        demo::section("统一擦除 erase / erase_if");
        std::vector<int> v{1, 2, 3, 4, 5, 6};
        std::erase_if(v, [](int n) { return n % 2 == 0; });
        std::cout << "    删掉偶数后: ";
        for (int n : v) std::cout << n << " ";
        std::cout << "\n";
        demo::line("以前必须写 erase-remove 惯用法：v.erase(std::remove_if(...), v.end())");

        demo::section("[[likely]] / [[unlikely]]");
        demo::line("if (ptr) [[likely]] { fast(); } else [[unlikely]] { slow(); }");
        demo::line("给分支预测提示。只在真正的热点循环里用，乱标反而变慢。");

        demo::section("constexpr 大扩展");
        demo::line("C++20 起 constexpr 函数里可以 new/delete、try/catch、调虚函数，");
        demo::line("std::vector 和 std::string 也能在编译期用了（前提：分配的内存要在编译期释放）。");

        demo::section("std::ssize —— 有符号长度");
        std::vector<int> vv{1, 2, 3};
        SHOW(std::ssize(vv));
        demo::line("v.size() 是无符号的，写 for(int i=0;i<v.size();++i) 会有符号比较警告。");
    }

    std::cout << "\n第 4 章结束。\n";
    return 0;
}
