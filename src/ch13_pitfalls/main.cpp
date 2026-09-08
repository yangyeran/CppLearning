// =============================================================================
// 第 13 章 —— C++ 易错点大全（60 个坑）
//
// 这一章是「踩坑博物馆」。每个坑的组织方式固定：
//
//     【现象】  出错时你看到的表现
//     【原因】  语言层面到底发生了什么
//     【错误】  写错的代码（大多用注释标出，避免真的触发未定义行为）
//     【正确】  应该怎么写
//     【记忆】  一句话口诀
//
// 关于「未定义行为」(UB, Undefined Behavior) 的重要说明：
//   UB 不是「会崩溃」，而是「编译器可以做任何事」——包括看起来正常运行。
//   这比崩溃可怕得多：你的测试全过，上线后随机出错。
//   所以本章大部分 UB 只讲解不实际执行；确实要演示的会明确标注。
//
// 运行： ch13_pitfalls.exe
// 建议配合工具：
//   MSVC:  项目属性 -> C/C++ -> 常规 -> 启用地址擦除器(/fsanitize=address)
//   GCC:   g++ -fsanitize=address,undefined -g
// =============================================================================

#include "demo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include <cmath>
#include <stdexcept>

// 让「这个函数只是给你看，不要真调用」的意图更明显
#define 仅供阅读

// =============================================================================
// 第一组：初始化与类型系统
// =============================================================================

// -----------------------------------------------------------------------------
// 坑 01：花括号 vs 圆括号 —— vector 的初始化陷阱
// -----------------------------------------------------------------------------
// 【现象】想要「10 个元素，每个都是 5」，结果得到「2 个元素：10 和 5」。
// 【原因】只要类型有接收 std::initializer_list 的构造函数，花括号就会
//         **优先**匹配它，哪怕另一个重载「更合适」。这是标准规定的优先级。
// 【记忆】容器要「几个几」用圆括号，要「就这几个」用花括号。
static void pitfall_01_brace_vs_paren() {
    demo::section("坑 01  花括号 vs 圆括号");

    std::vector<int> a(10, 5);   // 圆括号：调 vector(count, value) -> 10 个 5
    std::vector<int> b{10, 5};   // 花括号：调 initializer_list    -> 元素 10, 5

    std::cout << "  vector<int> a(10, 5)  size=" << a.size() << "  内容: ";
    for (int x : a) std::cout << x << ' ';
    std::cout << "\n";

    std::cout << "  vector<int> b{10, 5}  size=" << b.size() << "  内容: ";
    for (int x : b) std::cout << x << ' ';
    std::cout << "\n";

    // 反直觉的推论：类型不同时 initializer_list 匹配不上，就回退到普通重载
    std::vector<std::string> c{10, "x"};  // string 无法从 10 转换
    std::cout << "  vector<string> c{10, \"x\"}  size=" << c.size()
              << "   <== 花括号也变成了 (count, value)！因为 10 转不成 string\n";

    demo::line("结论：花括号的行为依赖元素类型，不稳定。指定个数一律用圆括号。");
}

// -----------------------------------------------------------------------------
// 坑 02：auto 会丢掉引用和顶层 const
// -----------------------------------------------------------------------------
// 【现象】想改容器里的元素，改完发现原容器没变。
// 【原因】auto 的推导规则照抄「模板实参推导」：按值传递会衰减
//         (decay)——丢引用、丢顶层 const、数组退化成指针。
// 【记忆】要改就写 auto&，只读就写 const auto&，要拷贝才写 auto。
static void pitfall_02_auto_decay() {
    demo::section("坑 02  auto 丢引用与 const");

    std::vector<int> v{1, 2, 3};

    // 错误：x 是副本，改它等于什么都没做
    for (auto x : v) x *= 100;
    std::cout << "  for (auto x : v) x *= 100;   结果: ";
    for (int x : v) std::cout << x << ' ';
    std::cout << "  <== 没变！\n";

    // 正确：绑定引用
    for (auto& x : v) x *= 100;
    std::cout << "  for (auto& x : v) x *= 100;  结果: ";
    for (int x : v) std::cout << x << ' ';
    std::cout << "  <== 改了\n";

    const int ci = 42;
    auto        a1 = ci;   // int          —— const 丢了，a1 可以改
    const auto  a2 = ci;   // const int
    auto&       a3 = ci;   // const int&   —— 引用会保留被引用对象的 const
    static_assert(std::is_same_v<decltype(a1), int>);
    static_assert(std::is_same_v<decltype(a2), const int>);
    static_assert(std::is_same_v<decltype(a3), const int&>);
    a1 = 0;                // 合法：a1 只是个 int 副本
    (void)a1; (void)a2; (void)a3;

    demo::line("大型对象用 auto 遍历会产生大量隐式拷贝，是常见性能问题。");
}

// -----------------------------------------------------------------------------
// 坑 03：vector<bool> 不是容器，auto 拿到的是代理对象
// -----------------------------------------------------------------------------
// 【现象】auto b = vb[0]; 之后改 b，居然改到了容器里；或者 &vb[0] 编译不过。
// 【原因】vector<bool> 是特化版本，为省内存用「每个 bool 一个 bit」存储。
//         operator[] 没法返回 bool&（引用不能指向一个 bit），只能返回一个
//         代理类 std::vector<bool>::reference，它内部持有指针+位偏移。
// 【记忆】需要 bool 容器时用 vector<char>、deque<bool> 或 bitset。
static void pitfall_03_vector_bool() {
    demo::section("坑 03  vector<bool> 的代理对象");

    std::vector<bool> vb{false, false};

    auto proxy = vb[0];   // 类型不是 bool，而是代理引用
    std::cout << "  decltype(vb[0]) 是 bool 吗? "
              << std::boolalpha << std::is_same_v<decltype(vb[0]), bool> << "\n";

    proxy = true;         // 通过代理写回了容器
    std::cout << "  auto proxy = vb[0]; proxy = true;  =>  vb[0] = "
              << vb[0] << "   <== 容器被改了！\n";

    bool real = vb[1];    // 显式写 bool 才是真拷贝
    real = true;
    std::cout << "  bool real = vb[1]; real = true;    =>  vb[1] = "
              << vb[1] << "   <== 容器没变，符合直觉\n";

    demo::line("bool* p = &vb[0];  直接编译错误 —— 无法取一个 bit 的地址。");
}

// -----------------------------------------------------------------------------
// 坑 04：有符号 / 无符号混用 —— size() - 1 的经典死循环
// -----------------------------------------------------------------------------
// 【现象】容器为空时循环跑了 40 亿次，或者 i < v.size() - 1 直接崩。
// 【原因】size() 返回无符号的 size_t。0u - 1 不是 -1，而是回绕成
//         SIZE_MAX（64 位下 18446744073709551615）。
//         而 int 与 unsigned 比较时，int 会被**转成 unsigned**，负数变巨大正数。
// 【记忆】容器长度参与减法前，先转成有符号；C++20 起用 std::ssize()。
static void pitfall_04_signed_unsigned() {
    demo::section("坑 04  有符号/无符号混用");

    std::vector<int> empty_v;

    std::cout << "  empty_v.size()      = " << empty_v.size() << "\n";
    std::cout << "  empty_v.size() - 1  = " << (empty_v.size() - 1)
              << "   <== 不是 -1，而是回绕后的巨大值\n";

    // 错误写法：空容器时循环体会执行天文数字次
    // for (size_t i = 0; i < empty_v.size() - 1; ++i) { ... }

    // 正确写法 1：把减法搬到另一侧，避免下溢
    for (size_t i = 0; i + 1 < empty_v.size(); ++i) { /* 安全 */ }

    // 正确写法 2：C++20 的 std::ssize 返回有符号长度
    for (std::ptrdiff_t i = 0; i < std::ssize(empty_v) - 1; ++i) { /* 安全 */ }

    // 比较陷阱：-1 < 1u 居然是 false
    int      si = -1;
    unsigned ui = 1;
    // 下面这行会触发 MSVC C4018 / GCC -Wsign-compare —— 这正是编译器在替你
    // 抓这个 bug。这里刻意保留代码、只屏蔽警告，好让程序能编译出来给你看结果。
    // 真实项目里看到这个警告应该改代码，不是加 pragma。
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable : 4018)
#endif
    std::cout << "  (-1 < 1u) 的结果是 " << (si < ui)
              << "   <== -1 被转成 4294967295，所以「不小于」1\n";
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

    // 正确：显式统一符号
    std::cout << "  (si < static_cast<int>(ui)) = " << (si < static_cast<int>(ui)) << "\n";

    demo::line("MSVC 的 C4018/C4389 和 GCC 的 -Wsign-compare 就是在警告这个，别忽略。");
}

// -----------------------------------------------------------------------------
// 坑 05：有符号整数溢出是 UB，无符号是回绕
// -----------------------------------------------------------------------------
// 【现象】x + 1 > x 被编译器优化成恒真，溢出检查失效。
// 【原因】标准规定有符号溢出是 UB，编译器可以假设它「永不发生」，
//         并据此优化掉你的检查。无符号溢出有定义（模 2^N 回绕）。
// 【记忆】做溢出检查要在运算**之前**判断，别在之后。
static void pitfall_05_overflow() {
    demo::section("坑 05  整数溢出");

    int imax = std::numeric_limits<int>::max();
    std::cout << "  INT_MAX = " << imax << "\n";
    // int bad = imax + 1;   // UB！编译器可能优化出任何结果

    unsigned umax = std::numeric_limits<unsigned>::max();
    std::cout << "  UINT_MAX + 1 = " << (umax + 1u)
              << "   <== 无符号有定义：回绕到 0\n";

    // 正确的溢出检查：先判断再运算
    auto safe_add = [](int a, int b, int& out) -> bool {
        if (b > 0 && a > std::numeric_limits<int>::max() - b) return false;
        if (b < 0 && a < std::numeric_limits<int>::min() - b) return false;
        out = a + b;
        return true;
    };
    int result = 0;
    std::cout << "  safe_add(INT_MAX, 1) 是否成功: " << safe_add(imax, 1, result) << "\n";
    std::cout << "  safe_add(1, 2)       是否成功: " << safe_add(1, 2, result)
              << "  结果=" << result << "\n";

    demo::line("生产环境更推荐编译器内建：__builtin_add_overflow (GCC/Clang)。");
}

// -----------------------------------------------------------------------------
// 坑 06：浮点数不能用 == 比较
// -----------------------------------------------------------------------------
// 【现象】0.1 + 0.2 == 0.3 返回 false。
// 【原因】IEEE-754 二进制浮点无法精确表示 0.1、0.2、0.3 这类十进制小数，
//         就像十进制无法精确表示 1/3。存进去的是最接近的可表示值。
// 【记忆】比较浮点要带容差，且容差应随数值量级缩放（相对误差）。
static void pitfall_06_float_compare() {
    demo::section("坑 06  浮点相等比较");

    double a = 0.1 + 0.2;
    double b = 0.3;

    std::printf("  0.1 + 0.2 = %.20f\n", a);
    std::printf("  0.3       = %.20f\n", b);
    std::cout << "  (0.1+0.2 == 0.3) = " << (a == b) << "   <== false！\n";

    // 正确：绝对容差 + 相对容差组合
    auto nearly_equal = [](double x, double y, double eps = 1e-9) {
        double diff = std::abs(x - y);
        if (diff <= eps) return true;                       // 处理接近 0 的情况
        double scale = std::max(std::abs(x), std::abs(y));  // 处理大数值
        return diff <= eps * scale;
    };
    std::cout << "  nearly_equal(0.1+0.2, 0.3) = " << nearly_equal(a, b) << "\n";

    // 大数值时固定容差会失效
    double big1 = 1e16, big2 = 1e16 + 1.0;
    std::cout << "  1e16 == 1e16+1 ? " << (big1 == big2)
              << "   <== true！double 在这个量级上已经分不出 1 的差别\n";

    demo::line("金额计算别用 double。用整数存「分」，或用定点/十进制库。");
}

// =============================================================================
// 第二组：指针、引用、生命周期 —— 崩溃的主要来源
// =============================================================================

// -----------------------------------------------------------------------------
// 坑 07：返回局部变量的引用或指针
// -----------------------------------------------------------------------------
// 【现象】函数返回后拿到的值是垃圾，或者「有时候对，有时候错」。
// 【原因】局部变量在函数返回时销毁，栈空间被后续调用覆盖。
//         返回引用 = 返回一个指向已死对象的别名，这叫「悬垂引用」。
// 【记忆】返回引用只能指向「比函数活得更久」的东西：成员、静态、参数。
仅供阅读
// const std::string& bad_ref() {
//     std::string local = "我马上就要死了";
//     return local;                      // UB！local 在此行之后销毁
// }
//
// int* bad_ptr() {
//     int local = 42;
//     return &local;                     // 同样 UB
// }

static const std::string& good_static_ref() {
    static const std::string s = "静态存储期，活到程序结束";  // 安全
    return s;
}

static std::string good_by_value() {
    std::string local = "按值返回，移动/省略掉，安全";
    return local;  // 有 NRVO，不会有额外拷贝
}

static void pitfall_07_dangling_return() {
    demo::section("坑 07  返回局部变量的引用");
    std::cout << "  静态引用: " << good_static_ref() << "\n";
    std::cout << "  按值返回: " << good_by_value() << "\n";
    demo::line("按值返回不慢：有 NRVO + 移动语义，别为了性能返回引用。");
}

// -----------------------------------------------------------------------------
// 坑 08：string_view / span 悬垂 —— 现代 C++ 的新型踩坑重灾区
// -----------------------------------------------------------------------------
// 【现象】string_view 打印出乱码，或者在 Release 下才出错。
// 【原因】string_view 只存「指针 + 长度」，**不拥有**数据。
//         它指向的 string 一旦销毁，view 就悬垂了。
//         最隐蔽的情况：函数返回临时 string，绑到 view 上。
// 【记忆】string_view 只适合做**参数**，不适合做成员变量和返回值。
static std::string make_temp_string() { return std::string("我是临时对象"); }

static void pitfall_08_string_view_dangling() {
    demo::section("坑 08  string_view 悬垂");

    // 错误：临时 string 在这一行末尾就销毁了，sv 立刻悬垂
    // std::string_view sv = make_temp_string();   // UB！
    // std::cout << sv;                             // 读已释放内存

    // 正确 1：先用具名变量持有所有权，再取 view
    std::string owner = make_temp_string();
    std::string_view sv_ok = owner;
    std::cout << "  持有所有权后再取 view: " << sv_ok << "\n";

    // 正确 2：字符串字面量是静态存储期，永远安全
    std::string_view lit = "字面量，静态存储期，安全";
    std::cout << "  指向字面量: " << lit << "\n";

    // 另一个坑：string_view 不保证以 '\0' 结尾，不能直接喂给 C 接口
    std::string full = "HelloWorld";
    std::string_view part = std::string_view(full).substr(0, 5);  // "Hello"
    std::cout << "  substr 出来的 view: " << part << "  size=" << part.size() << "\n";
    // std::printf("%s", part.data());   // 错！会一路打印到 "HelloWorld" 结束
    std::printf("  喂给 C 接口要带长度: %.*s\n",
                static_cast<int>(part.size()), part.data());

    demo::line("类的成员变量存 string_view 几乎总是 bug，请存 std::string。");
}

// -----------------------------------------------------------------------------
// 坑 09：迭代器 / 指针 / 引用失效
// -----------------------------------------------------------------------------
// 【现象】遍历中插入元素后崩溃，或者结果莫名其妙。
// 【原因】vector 扩容会重新分配内存并把元素搬走，所有指向旧内存的
//         迭代器、指针、引用全部失效。erase 会使被删位置及其后全部失效。
// 【记忆】改容器就假设迭代器全废了。要么用返回值，要么用索引。
//
//   各容器失效规则（面试高频）：
//     vector   插入: 扩容则全失效，未扩容则插入点之后失效
//              删除: 删除点之后失效
//     deque    插入/删除: 迭代器基本都失效；两端操作时引用/指针不失效
//     list     插入: 不失效        删除: 仅被删元素失效
//     map/set  插入: 不失效        删除: 仅被删元素失效
//     unordered_*  rehash 时迭代器全失效，但引用/指针不失效
static void pitfall_09_iterator_invalidation() {
    demo::section("坑 09  迭代器失效");

    std::vector<int> v{1, 2, 3, 4, 5};
    v.reserve(5);  // 容量刚好，下一次 push_back 必然扩容

    int* p_before = v.data();
    v.push_back(6);
    int* p_after = v.data();
    std::cout << "  push_back 前 data() = " << static_cast<const void*>(p_before) << "\n";
    std::cout << "  push_back 后 data() = " << static_cast<const void*>(p_after)
              << (p_before != p_after ? "   <== 地址变了，旧指针全部失效\n" : "\n");

    // 错误：边遍历边 erase
    // for (auto it = v.begin(); it != v.end(); ++it)
    //     if (*it % 2 == 0) v.erase(it);      // erase 后 it 失效，++it 是 UB

    // 正确 1：用 erase 的返回值（指向被删元素的下一个）
    std::vector<int> v1{1, 2, 3, 4, 5, 6};
    for (auto it = v1.begin(); it != v1.end(); ) {
        if (*it % 2 == 0) it = v1.erase(it);   // erase 返回新的有效迭代器
        else              ++it;                 // 只有不删时才自增
    }
    std::cout << "  手写 erase 循环去掉偶数: ";
    for (int x : v1) std::cout << x << ' ';
    std::cout << "\n";

    // 正确 2：C++20 的 std::erase_if —— 最简洁，且是 O(n)
    std::vector<int> v2{1, 2, 3, 4, 5, 6};
    std::erase_if(v2, [](int x) { return x % 2 == 0; });
    std::cout << "  std::erase_if(v2, 偶数):   ";
    for (int x : v2) std::cout << x << ' ';
    std::cout << "\n";

    demo::line("C++20 之前用 erase-remove 惯用法：v.erase(std::remove_if(...), v.end())");
}

// -----------------------------------------------------------------------------
// 坑 10：erase-remove 只写一半
// -----------------------------------------------------------------------------
// 【现象】调了 std::remove 之后 size() 没变，元素还在。
// 【原因】std::remove 是**算法**，不知道容器怎么删元素。它只把要保留的
//         元素往前搬，返回「新逻辑末尾」，尾部残留旧值。真正缩短长度
//         必须由容器的 erase 完成。这是 STL「算法与容器分离」的代价。
// 【记忆】remove 只搬不删，删要靠 erase。
static void pitfall_10_erase_remove() {
    demo::section("坑 10  remove 不会真的删除");

    std::vector<int> v{1, 2, 3, 2, 5};

    auto new_end = std::remove(v.begin(), v.end(), 2);  // 把非 2 的往前搬
    std::cout << "  remove 后 size() 仍是 " << v.size() << "\n";
    std::cout << "  完整内容(含尾部垃圾): ";
    for (int x : v) std::cout << x << ' ';
    std::cout << "\n";
    std::cout << "  有效范围 [begin, new_end): ";
    for (auto it = v.begin(); it != new_end; ++it) std::cout << *it << ' ';
    std::cout << "\n";

    v.erase(new_end, v.end());   // 补上这一步才算删完
    std::cout << "  erase 之后 size() = " << v.size() << "  内容: ";
    for (int x : v) std::cout << x << ' ';
    std::cout << "\n";

    demo::line("C++20 起直接用 std::erase(v, 2) / std::erase_if(v, pred)，一步到位。");
}

// -----------------------------------------------------------------------------
// 坑 11：new[] 配 delete，或 new 配 delete[]
// -----------------------------------------------------------------------------
// 【现象】析构函数没被调用、堆损坏、Debug 下断言失败。
// 【原因】new[] 会在分配的内存里额外记录「元素个数」，delete[] 依赖这个
//         信息逐个调用析构函数。用 delete 只析构第一个元素，且释放的
//         起始地址可能不对。
// 【记忆】根本解法：别手写 new/delete，用容器和智能指针。
static void pitfall_11_new_delete_mismatch() {
    demo::section("坑 11  new/delete 配对");

    // int* arr = new int[10];
    // delete arr;          // 错！应该 delete[] arr
    // int* one = new int;
    // delete[] one;        // 错！应该 delete one

    demo::line("正确做法（现代 C++ 根本不写 new）：");
    std::vector<int>          v(10);                       // 首选：容器
    auto up_arr = std::make_unique<int[]>(10);             // 需要裸数组时
    auto up_one = std::make_unique<int>(42);
    std::cout << "  vector<int> v(10)              size=" << v.size() << "\n";
    std::cout << "  make_unique<int[]>(10)         自动 delete[]\n";
    std::cout << "  make_unique<int>(42)   值=" << *up_one << "  自动 delete\n";
}

// -----------------------------------------------------------------------------
// 坑 12：const 的位置 —— 从右往左读
// -----------------------------------------------------------------------------
// 【现象】以为改不了，结果改了；或者以为能改，编译报错。
// 【原因】const 修饰它**左边**的东西；如果左边没有，就修饰右边。
// 【记忆】把声明从右往左念出来。
static void pitfall_12_const_position() {
    demo::section("坑 12  const 的位置");

    int x = 1, y = 2;

    const int* p1 = &x;         // 「指向 const int 的指针」：不能改值，能改指向
    p1 = &y;                    // OK
    // *p1 = 10;                // 错误

    int* const p2 = &x;         // 「指向 int 的 const 指针」：能改值，不能改指向
    *p2 = 10;                   // OK
    // p2 = &y;                 // 错误

    const int* const p3 = &x;   // 两者都不能改
    (void)p1; (void)p2; (void)p3;

    std::cout << "  const int* p        值只读，指针可变\n";
    std::cout << "  int* const p        指针只读，值可变\n";
    std::cout << "  const int* const p  都只读\n";
    std::cout << "  int const* p        等价于 const int* p（const 在左边找不到东西就修饰右边）\n";

    demo::line("建议统一写在类型右边（int const*），这样「const 修饰左边」的规则永远一致。");
}

// =============================================================================
// 第三组：类与对象
// =============================================================================

// -----------------------------------------------------------------------------
// 坑 13：基类析构函数不是 virtual
// -----------------------------------------------------------------------------
// 【现象】通过基类指针 delete 派生类对象，派生类的析构函数没执行，资源泄漏。
// 【原因】delete 基类指针时，编译器按**静态类型**决定调哪个析构函数。
//         只有析构是 virtual，才会走虚表找到真实类型的析构函数。
// 【记忆】只要一个类可能被继承并通过基类指针删除，析构就必须 virtual。
class BadBase {
public:
    ~BadBase() { std::cout << "    BadBase::~BadBase\n"; }  // 少了 virtual
};
class BadDerived : public BadBase {
    std::unique_ptr<int[]> big_;
public:
    BadDerived() : big_(std::make_unique<int[]>(1000)) {}
    ~BadDerived() { std::cout << "    BadDerived::~BadDerived  (释放 1000 个 int)\n"; }
};

class GoodBase {
public:
    virtual ~GoodBase() { std::cout << "    GoodBase::~GoodBase\n"; }
};
class GoodDerived : public GoodBase {
public:
    ~GoodDerived() override { std::cout << "    GoodDerived::~GoodDerived\n"; }
};

static void pitfall_13_virtual_dtor() {
    demo::section("坑 13  基类析构非 virtual");

    std::cout << "  非虚析构（通过基类指针删除派生对象）：\n";
    {
        BadBase* p = new BadDerived();
        delete p;   // 严格说这是 UB；实际表现通常是漏掉派生类析构
        std::cout << "    ^^^ 注意 BadDerived::~BadDerived 没有被调用 —— 泄漏\n";
    }

    std::cout << "  虚析构：\n";
    {
        std::unique_ptr<GoodBase> p = std::make_unique<GoodDerived>();
    }   // 析构顺序：派生 -> 基类，正确

    demo::line("反过来：不打算被继承的类，加 final 比加 virtual 析构更省（无虚表开销）。");
}

// -----------------------------------------------------------------------------
// 坑 14：构造/析构函数里调用虚函数
// -----------------------------------------------------------------------------
// 【现象】明明重写了虚函数，构造时调用的还是基类版本。
// 【原因】对象是「由内向外」构造的：基类构造函数执行时，派生类部分还没
//         初始化。为了避免调用到未初始化的派生成员，标准规定此时对象的
//         **动态类型就是基类**，虚表还指向基类。析构时反过来同理。
// 【记忆】构造和析构期间，虚函数没有多态。要「构造后初始化」就单独提供
//         一个 init() 方法，或者用工厂函数。
class CtorBase {
public:
    CtorBase() {
        std::cout << "    CtorBase 构造中，调用 who(): ";
        who();   // 这里永远调 CtorBase::who
    }
    virtual ~CtorBase() = default;
    virtual void who() const { std::cout << "CtorBase::who\n"; }
};
class CtorDerived : public CtorBase {
    int value_ = 999;
public:
    CtorDerived() {
        std::cout << "    CtorDerived 构造完成，调用 who(): ";
        who();   // 这里才是 CtorDerived::who
    }
    void who() const override {
        std::cout << "CtorDerived::who  value_=" << value_ << "\n";
    }
};

static void pitfall_14_virtual_in_ctor() {
    demo::section("坑 14  构造函数里调虚函数");
    CtorDerived d;
    demo::line("如果 CtorBase 的 who() 是纯虚的，这就是 UB（调用纯虚函数，通常直接崩）。");
}

// -----------------------------------------------------------------------------
// 坑 15：成员初始化顺序 = 声明顺序，与初始化列表的书写顺序无关
// -----------------------------------------------------------------------------
// 【现象】用一个成员初始化另一个成员，拿到的是垃圾值。
// 【原因】成员按**类中声明的顺序**初始化。初始化列表里的顺序只是写法，
//         编译器不理会（MSVC 的 C5038、GCC 的 -Wreorder 会警告）。
// 【记忆】初始化列表的顺序永远与声明顺序保持一致；不要用成员初始化成员。
class BadOrder {
    // 声明顺序：size_ 在前，count_ 在后
    size_t size_;
    size_t count_;
public:
    // 书写顺序是 count_ 在前，但**实际执行**先初始化 size_
    explicit BadOrder(size_t n)
        : count_(n), size_(count_ * 2) {}   // size_ 先算，此时 count_ 还是垃圾！
    void show() const {
        std::cout << "    BadOrder:  count_=" << count_
                  << "  size_=" << size_ << "  <== size_ 应该是 count_*2，但不是\n";
    }
};

class GoodOrder {
    size_t count_;
    size_t size_;
public:
    explicit GoodOrder(size_t n) : count_(n), size_(n * 2) {}  // 都用参数 n，不互相依赖
    void show() const {
        std::cout << "    GoodOrder: count_=" << count_ << "  size_=" << size_ << "\n";
    }
};

static void pitfall_15_member_init_order() {
    demo::section("坑 15  成员初始化顺序");
    BadOrder(5).show();
    GoodOrder(5).show();
    demo::line("规则：初始化列表只依赖构造函数参数，不依赖其它成员。");
}

// -----------------------------------------------------------------------------
// 坑 16：隐式转换构造函数 —— 忘记写 explicit
// -----------------------------------------------------------------------------
// 【现象】本该编译报错的代码悄悄通过了，运行出诡异结果。
// 【原因】单参数构造函数（或除首参外都有默认值的构造函数）会成为
//         「转换构造函数」，允许编译器插入一次隐式转换。
// 【记忆】单参数构造函数默认加 explicit，除非你**确实**想要隐式转换。
class BadString {
    size_t len_;
public:
    BadString(size_t len) : len_(len) {}   // 少了 explicit
    size_t len() const { return len_; }
};
class GoodString {
    size_t len_;
public:
    explicit GoodString(size_t len) : len_(len) {}
    size_t len() const { return len_; }
};

static void print_bad(const BadString& s)  { std::cout << "    len=" << s.len() << "\n"; }
static void print_good(const GoodString& s){ std::cout << "    len=" << s.len() << "\n"; }

static void pitfall_16_explicit() {
    demo::section("坑 16  忘记 explicit");

    std::cout << "  没有 explicit：print_bad('A') 竟然能编译\n";
    print_bad('A');   // char 'A' -> size_t 65 -> BadString(65)，两次隐式转换

    std::cout << "  有 explicit：print_good('A') 编译错误，必须显式写\n";
    print_good(GoodString(65));

    demo::line("std::vector 的 explicit vector(size_t) 就是为了阻止 vector<int> v = 10;");
}

// -----------------------------------------------------------------------------
// 坑 17：对象切片（Object Slicing）
// -----------------------------------------------------------------------------
// 【现象】把派生类对象放进 vector<Base>，多态失效，派生部分数据丢失。
// 【原因】按值存储时，容器只为 Base 分配空间，派生类多出来的成员
//         被「切掉」，虚表指针也被改成 Base 的。
// 【记忆】多态容器必须存指针：vector<unique_ptr<Base>>。
class SliceBase {
public:
    virtual ~SliceBase() = default;
    virtual std::string name() const { return "SliceBase"; }
};
class SliceDerived : public SliceBase {
    std::string extra_ = "派生类独有数据";
public:
    std::string name() const override { return "SliceDerived(" + extra_ + ")"; }
};

static void pitfall_17_slicing() {
    demo::section("坑 17  对象切片");

    std::vector<SliceBase> bad;
    bad.push_back(SliceDerived{});   // 切片发生在这里
    std::cout << "  vector<SliceBase>          -> " << bad[0].name()
              << "   <== 派生信息丢失\n";

    std::vector<std::unique_ptr<SliceBase>> good;
    good.push_back(std::make_unique<SliceDerived>());
    std::cout << "  vector<unique_ptr<Base>>   -> " << good[0]->name() << "   <== 正确\n";

    // 按值传参也会切片
    auto by_value = [](SliceBase b) { std::cout << "  按值传参   -> " << b.name() << "\n"; };
    auto by_ref   = [](const SliceBase& b) { std::cout << "  按引用传参 -> " << b.name() << "\n"; };
    SliceDerived d;
    by_value(d);
    by_ref(d);

    demo::line("防御手段：基类可以 delete 拷贝构造，让切片直接编译失败。");
}

// -----------------------------------------------------------------------------
// 坑 18：名字隐藏（Name Hiding）—— 重载 vs 覆盖
// -----------------------------------------------------------------------------
// 【现象】派生类里加了个同名函数，基类的所有重载版本突然调不到了。
// 【原因】名字查找是**逐作用域**进行的：在派生类里找到该名字就停止，
//         根本不会去看基类。这与重载解析是两个独立阶段。
// 【记忆】想保留基类重载，写 using Base::函数名;
class HideBase {
public:
    virtual ~HideBase() = default;
    void f(int)    { std::cout << "    HideBase::f(int)\n"; }
    void f(double) { std::cout << "    HideBase::f(double)\n"; }
};
class HideBad : public HideBase {
public:
    void f(std::string) { std::cout << "    HideBad::f(string)\n"; }
    // 基类的 f(int)/f(double) 被隐藏了
};
class HideGood : public HideBase {
public:
    using HideBase::f;   // 把基类的重载「拉」进本作用域
    void f(std::string) { std::cout << "    HideGood::f(string)\n"; }
};

static void pitfall_18_name_hiding() {
    demo::section("坑 18  名字隐藏");

    HideBad bad;
    // bad.f(1);              // 编译错误：int 转不成 string
    bad.HideBase::f(1);        // 只能显式限定作用域
    std::cout << "    ^^^ HideBad 必须写 bad.HideBase::f(1)\n";

    HideGood good;
    good.f(1);                 // 现在能找到基类版本了
    good.f(std::string("s"));
}

// -----------------------------------------------------------------------------
// 坑 19：拷贝赋值忘了处理自赋值
// -----------------------------------------------------------------------------
// 【现象】v = v; 之后对象数据变成垃圾。
// 【原因】典型的错误实现是「先释放旧资源，再拷贝新资源」。自赋值时
//         「新资源」就是刚被释放的那块内存。
// 【记忆】用 copy-and-swap 惯用法，天然自赋值安全 + 异常安全。
class SelfAssign {
    size_t n_;
    int*   data_;
public:
    explicit SelfAssign(size_t n) : n_(n), data_(new int[n]{}) {}
    ~SelfAssign() { delete[] data_; }

    SelfAssign(const SelfAssign& o) : n_(o.n_), data_(new int[o.n_]) {
        std::copy(o.data_, o.data_ + n_, data_);
    }

    // 错误实现：
    // SelfAssign& operator=(const SelfAssign& o) {
    //     delete[] data_;                       // 自赋值时把 o.data_ 也删了
    //     n_ = o.n_;
    //     data_ = new int[n_];
    //     std::copy(o.data_, o.data_ + n_, data_);   // 读已释放内存
    //     return *this;
    // }

    // 正确实现：copy-and-swap
    //   参数按值传递 -> 先完成拷贝（可能抛异常，但此时 *this 还完好）
    //   再 swap      -> 只做指针交换，不会抛
    //   函数返回     -> 临时对象析构，顺带释放旧资源
    SelfAssign& operator=(SelfAssign o) noexcept {   // 注意：按值传参
        swap(o);
        return *this;
    }
    void swap(SelfAssign& o) noexcept {
        std::swap(n_, o.n_);
        std::swap(data_, o.data_);
    }
    size_t size() const { return n_; }
};

static void pitfall_19_self_assignment() {
    demo::section("坑 19  自赋值");
    SelfAssign a(10);
    a = a;   // copy-and-swap 版本完全安全
    std::cout << "  copy-and-swap 实现，自赋值后 size=" << a.size() << "  正常\n";
    demo::line("copy-and-swap 一份代码同时搞定：自赋值安全、异常安全、拷贝+移动赋值。");
}

// -----------------------------------------------------------------------------
// 坑 20：移动之后继续使用对象
// -----------------------------------------------------------------------------
// 【现象】move 之后原对象里的数据没了，代码却还在读它。
// 【原因】标准只保证被移动对象处于「有效但未指定」状态：可以安全析构、
//         可以重新赋值，但**内容不确定**。标准库容器实际上会变成空，
//         但这不是标准保证，自定义类型更是随实现而变。
// 【记忆】move 过的变量只做两件事：重新赋值，或者让它析构。
static void pitfall_20_use_after_move() {
    demo::section("坑 20  move 之后继续使用");

    std::string s = "原始内容";
    std::string t = std::move(s);
    std::cout << "  move 后 t = \"" << t << "\"\n";
    std::cout << "  move 后 s = \"" << s << "\"  size=" << s.size()
              << "   <== 标准库实现里通常变空，但别依赖\n";

    s = "重新赋值后就完全正常了";   // 唯一推荐的后续操作
    std::cout << "  重新赋值: " << s << "\n";

    // unique_ptr 移动后一定是 nullptr（这个是标准保证的）
    auto p1 = std::make_unique<int>(42);
    auto p2 = std::move(p1);
    std::cout << "  unique_ptr move 后原指针为空: " << (p1 == nullptr)
              << "   <== 这个有标准保证\n";

    demo::line("clang-tidy 的 bugprone-use-after-move 能自动检出这类问题。");
}

// =============================================================================
// 第四组：现代 C++ 特性的新型坑
// =============================================================================

// -----------------------------------------------------------------------------
// 坑 21：lambda 引用捕获导致悬垂
// -----------------------------------------------------------------------------
// 【现象】lambda 存起来延后执行，调用时读到垃圾值或崩溃。
// 【原因】[&] 捕获的是引用，lambda 的寿命可能超过被捕获变量的作用域。
// 【记忆】立即执行的 lambda 可以用 [&]；要存起来 / 跨线程 / 异步的，必须值捕获。
static std::function<int()> make_dangling_lambda() {
    int local = 42;
    // return [&local]{ return local; };   // UB！local 在函数返回后就没了
    return [local] { return local; };      // 值捕获，安全
}

static void pitfall_21_lambda_capture() {
    demo::section("坑 21  lambda 引用捕获悬垂");

    auto safe = make_dangling_lambda();
    std::cout << "  值捕获的 lambda 返回: " << safe() << "\n";

    // 循环变量捕获的经典错误
    std::vector<std::function<void()>> tasks;
    for (int i = 0; i < 3; ++i) {
        // tasks.push_back([&i]{ std::cout << i; });   // 全部指向同一个 i，且循环结束就悬垂
        tasks.push_back([i] { std::cout << ' ' << i; });  // 每个 lambda 拷贝一份
    }
    std::cout << "  值捕获循环变量:";
    for (auto& t : tasks) t();
    std::cout << "\n";

    demo::line("C++14 起可以初始化捕获：[p = std::move(uptr)]，用来移动捕获 move-only 对象。");
}

// -----------------------------------------------------------------------------
// 坑 22：lambda 捕获 this —— 异步回调里最常见的崩溃
// -----------------------------------------------------------------------------
// 【现象】对象已经销毁，回调才被触发，访问成员崩溃。
// 【原因】[this] 和 [=]（C++20 前）捕获的是裸指针 this，不延长对象寿命。
// 【记忆】异步场景用 weak_from_this()，回调里先 lock 再用。
class AsyncBad {
    int value_ = 42;
public:
    std::function<void()> make_callback() {
        return [this] { std::cout << "    value_=" << value_ << "\n"; };
        // 对象销毁后这个回调就是定时炸弹
    }
};

// 正确姿势：继承 enable_shared_from_this，捕获 weak_ptr
class AsyncGood : public std::enable_shared_from_this<AsyncGood> {
    int value_ = 42;
public:
    std::function<void()> make_callback() {
        // weak_from_this() 需要对象由 shared_ptr 管理
        std::weak_ptr<AsyncGood> weak = weak_from_this();
        return [weak] {
            if (auto self = weak.lock()) {          // 提升成功 = 对象还活着
                std::cout << "    对象存活, value_=" << self->value_ << "\n";
            } else {
                std::cout << "    对象已销毁，安全跳过回调\n";
            }
        };
    }
};

static void pitfall_22_capture_this() {
    demo::section("坑 22  lambda 捕获 this");

    std::function<void()> cb;
    {
        auto obj = std::make_shared<AsyncGood>();
        cb = obj->make_callback();
        std::cout << "  对象在作用域内调用回调：\n";
        cb();
    }   // obj 在此销毁
    std::cout << "  对象销毁后再调用同一个回调：\n";
    cb();   // weak_ptr 提升失败，安全

    demo::line("Reactor / 异步框架里这是头号 bug 来源，见第 16 章的 TcpConnection。");
}

// -----------------------------------------------------------------------------
// 坑 23：shared_ptr 循环引用导致内存永不释放
// -----------------------------------------------------------------------------
// 【现象】没有崩溃，也没有报错，就是内存一直涨，析构函数永不执行。
// 【原因】A 持有 B 的 shared_ptr，B 也持有 A 的，两边引用计数都停在 1，
//         谁也等不到对方归零。
// 【记忆】「拥有」关系用 shared_ptr，「反向引用/观察」关系用 weak_ptr。
//         典型：父节点拥有子节点(shared)，子节点指回父节点(weak)。
struct CycleNode {
    std::string           name;
    std::shared_ptr<CycleNode> next;   // 双向都用 shared -> 循环
    std::shared_ptr<CycleNode> prev;
    explicit CycleNode(std::string n) : name(std::move(n)) {}
    ~CycleNode() { std::cout << "    ~CycleNode(" << name << ")\n"; }
};

struct FixedNode {
    std::string              name;
    std::shared_ptr<FixedNode> next;   // 正向：拥有
    std::weak_ptr<FixedNode>   prev;   // 反向：仅观察，不计数
    explicit FixedNode(std::string n) : name(std::move(n)) {}
    ~FixedNode() { std::cout << "    ~FixedNode(" << name << ")\n"; }
};

static void pitfall_23_shared_ptr_cycle() {
    demo::section("坑 23  shared_ptr 循环引用");

    std::cout << "  双向都用 shared_ptr：\n";
    {
        auto a = std::make_shared<CycleNode>("A");
        auto b = std::make_shared<CycleNode>("B");
        a->next = b;
        b->prev = a;
        std::cout << "    离开作用域前 a.use_count()=" << a.use_count()
                  << "  b.use_count()=" << b.use_count() << "\n";
    }
    std::cout << "    ^^^ 上面没有任何析构输出 —— 两个节点都泄漏了\n";

    std::cout << "  反向改成 weak_ptr：\n";
    {
        auto a = std::make_shared<FixedNode>("A");
        auto b = std::make_shared<FixedNode>("B");
        a->next = b;
        b->prev = a;       // weak 不增加计数
        std::cout << "    离开作用域前 a.use_count()=" << a.use_count()
                  << "  b.use_count()=" << b.use_count() << "\n";
    }
    std::cout << "    ^^^ 两个析构都正常执行\n";
}

// -----------------------------------------------------------------------------
// 坑 24：同一个裸指针交给两个 shared_ptr —— 双重释放
// -----------------------------------------------------------------------------
// 【现象】程序在某次析构时崩溃，堆报错。
// 【原因】shared_ptr 的引用计数存在「控制块」里。用裸指针构造 shared_ptr
//         会**新建一个控制块**。两个独立控制块各自计数到 0，同一块内存被
//         释放两次。
// 【记忆】裸指针只交给 shared_ptr 一次。最好的办法是根本不出现裸指针：
//         直接用 make_shared。
static void pitfall_24_double_managed() {
    demo::section("坑 24  同一裸指针给两个 shared_ptr");

    // int* raw = new int(42);
    // std::shared_ptr<int> sp1(raw);
    // std::shared_ptr<int> sp2(raw);   // 第二个控制块！两次 delete -> 崩溃

    // 正确 1：从已有 shared_ptr 拷贝（共享同一控制块）
    auto sp1 = std::make_shared<int>(42);
    auto sp2 = sp1;
    std::cout << "  make_shared + 拷贝: use_count=" << sp1.use_count()
              << "   <== 共享同一个控制块\n";

    // make_shared 的额外好处：一次内存分配同时容纳控制块和对象
    //   shared_ptr<T>(new T)  -> 两次分配（对象 + 控制块），且 new 抛异常时可能泄漏
    //   make_shared<T>()      -> 一次分配，异常安全
    // 唯一劣势：weak_ptr 存活期间，对象内存无法提前归还操作系统
    std::cout << "  make_shared 一次分配；shared_ptr(new T) 两次分配\n";
}

// -----------------------------------------------------------------------------
// 坑 25：在类内部用 shared_ptr<this> —— 必须用 enable_shared_from_this
// -----------------------------------------------------------------------------
// 【现象】成员函数里 return std::shared_ptr<T>(this); 导致双重释放。
// 【原因】同坑 24：this 是裸指针，包出来的 shared_ptr 有独立控制块。
// 【记忆】继承 enable_shared_from_this，用 shared_from_this()。
//         前提：对象**必须**已经被某个 shared_ptr 管理，否则抛
//         std::bad_weak_ptr（C++17 起）。
class SharedSelf : public std::enable_shared_from_this<SharedSelf> {
public:
    std::shared_ptr<SharedSelf> get_self() {
        // return std::shared_ptr<SharedSelf>(this);   // 错！独立控制块
        return shared_from_this();                     // 对：复用已有控制块
    }
    ~SharedSelf() { std::cout << "    ~SharedSelf  只析构一次，正确\n"; }
};

static void pitfall_25_shared_from_this() {
    demo::section("坑 25  shared_from_this");
    {
        auto p    = std::make_shared<SharedSelf>();
        auto self = p->get_self();
        std::cout << "  use_count=" << p.use_count() << "   <== 2，同一控制块\n";
    }

    // 栈上对象调 shared_from_this 会抛异常
    try {
        SharedSelf on_stack;
        auto bad = on_stack.get_self();
    } catch (const std::bad_weak_ptr& e) {
        std::cout << "  栈对象调 shared_from_this 抛出: " << e.what() << "\n";
    }
}

// -----------------------------------------------------------------------------
// 坑 26：万能引用把重载「吞」掉
// -----------------------------------------------------------------------------
// 【现象】明明写了 f(int) 重载，传 int 却进了模板版本。
// 【原因】T&& 模板是**完美匹配**（不需要任何转换），而 f(int) 遇到
//         非 int 实参需要一次转换。所以模板几乎总是赢。
// 【记忆】万能引用重载很危险，改用 concept 约束（C++20）或 tag dispatch。
static void overload_target(int) { std::cout << "    overload_target(int)\n"; }

template <typename T>
static void overload_target(T&&) {
    std::cout << "    overload_target(T&&) 模板版本  T=" << typeid(T).name() << "\n";
}

// 正确做法：用 concept 限制模板的适用范围
template <typename T>
    requires (!std::is_arithmetic_v<std::remove_cvref_t<T>>)
static void constrained_target(T&&) { std::cout << "    constrained_target(T&&) 非算术类型\n"; }
static void constrained_target(int) { std::cout << "    constrained_target(int)\n"; }

static void pitfall_26_forwarding_ref_overload() {
    demo::section("坑 26  万能引用吞掉重载");

    std::cout << "  未约束版本：\n";
    overload_target(42);          // int 精确匹配 f(int)
    short s = 1;
    overload_target(s);           // short -> 模板赢了（int 版本需要提升）
    std::cout << "    ^^^ short 本该走 f(int)，却进了模板\n";

    std::cout << "  concept 约束版本：\n";
    constrained_target(42);
    constrained_target(s);        // 算术类型被排除在模板外，正确走 int 版本
    constrained_target("hello");
}

// -----------------------------------------------------------------------------
// 坑 27：std::move 用在了不该用的地方
// -----------------------------------------------------------------------------
// 【现象】(a) 返回语句里写 move 反而阻止了优化；(b) 对 const 对象 move 无效。
// 【原因】(a) return local; 本身有 NRVO（直接在调用方构造，零拷贝）；
//             写成 return std::move(local); 反而**禁用** NRVO，退化成移动构造。
//         (b) std::move(const T&) 得到 const T&&，匹配不上 T&& 的移动构造，
//             静默退回拷贝构造。
// 【记忆】返回局部变量直接 return；move 只对非 const 的左值有意义。
static std::vector<int> return_correct() {
    std::vector<int> v(1000);
    return v;                    // NRVO：零成本
}
static std::vector<int> return_pessimized() {
    std::vector<int> v(1000);
    return std::move(v);         // 禁用 NRVO，多一次移动构造
}

static void pitfall_27_bad_move() {
    demo::section("坑 27  std::move 的误用");

    auto v1 = return_correct();
    auto v2 = return_pessimized();
    std::cout << "  return v;              -> NRVO，零拷贝零移动 (size=" << v1.size() << ")\n";
    std::cout << "  return std::move(v);   -> 禁用 NRVO，多一次移动 (size=" << v2.size() << ")\n";

    // const 对象 move 会静默退化成拷贝
    const std::string cs = "const 字符串";
    std::string dst = std::move(cs);   // 实际调用的是拷贝构造
    std::cout << "  const 源 move 后原值仍在: \"" << cs << "\"   <== 其实是拷贝\n";
    (void)dst;

    // 另一个坑：move 一个还要继续用的对象
    std::vector<int> src{1, 2, 3};
    std::vector<int> dst2 = src;             // 需要保留 src 就用拷贝
    std::vector<int> dst3 = std::move(src);  // 确定不再用 src 才 move
    std::cout << "  拷贝后 src.size()=" << dst2.size()
              << "，move 后 src.size()=" << src.size() << "\n";
}

// -----------------------------------------------------------------------------
// 坑 28：std::forward 与 std::move 混用
// -----------------------------------------------------------------------------
// 【现象】传左值进去，出来的时候被搬空了。
// 【原因】在万能引用 T&& 上写 std::move 会**无条件**转成右值，
//         哪怕调用方传的是左值。std::forward<T> 才会「保持原样」。
// 【记忆】T&& 配 forward<T>，明确的 T&& 右值引用配 move。
template <typename T>
static void relay_wrong(T&& arg) {
    std::string local = std::move(arg);   // 无条件搬走，调用方的左值被破坏
    (void)local;
}
template <typename T>
static void relay_right(T&& arg) {
    std::string local = std::forward<T>(arg);   // 左值->拷贝，右值->移动
    (void)local;
}

static void pitfall_28_forward() {
    demo::section("坑 28  forward vs move");

    std::string s1 = "传给 relay_wrong";
    relay_wrong(s1);
    std::cout << "  relay_wrong 之后 s1 = \"" << s1 << "\"  size=" << s1.size()
              << "   <== 被搬空了\n";

    std::string s2 = "传给 relay_right";
    relay_right(s2);
    std::cout << "  relay_right 之后 s2 = \"" << s2 << "\"   <== 完好\n";

    demo::line("口诀：参数写的是 T&&（模板推导）就用 forward，写的是 Widget&& 就用 move。");
}

// -----------------------------------------------------------------------------
// 坑 29：auto 遇到花括号会推成 initializer_list
// -----------------------------------------------------------------------------
static void pitfall_29_auto_braces() {
    demo::section("坑 29  auto 与花括号");

    auto a1 = 1;      // int
    auto a2{1};       // C++17 起是 int（C++11/14 是 initializer_list<int>）
    auto a3 = {1};    // 一直都是 std::initializer_list<int>
    auto a4 = {1, 2}; // initializer_list<int>

    std::cout << "  auto a1 = 1;    -> int\n";
    std::cout << "  auto a2{1};     -> int（C++17 起）\n";
    std::cout << "  auto a3 = {1};  -> initializer_list<int>，size="
              << a3.size() << "   <== 意料之外\n";
    std::cout << "  auto a4 = {1,2};-> initializer_list<int>，size=" << a4.size() << "\n";
    (void)a1; (void)a2;

    demo::line("initializer_list 内部只是指针+长度，不拥有数据，别存起来（又是悬垂）。");
}

// =============================================================================
// 第五组：并发
// =============================================================================

// -----------------------------------------------------------------------------
// 坑 30：数据竞争 —— ++ 不是原子操作
// -----------------------------------------------------------------------------
// 【现象】两个线程各加 100000 次，结果小于 200000。
// 【原因】counter++ 是「读-改-写」三步。两个线程可能都读到 100，
//         各自加到 101 写回，一次自增就丢了。这是 UB，不只是「结果不准」。
// 【记忆】共享可变数据必须加锁或用 atomic。只读共享不需要同步。
static void pitfall_30_data_race() {
    demo::section("坑 30  数据竞争");

    constexpr int kIter = 100000;

    // 无保护：结果不可预测
    int plain = 0;
    {
        std::thread t1([&] { for (int i = 0; i < kIter; ++i) ++plain; });
        std::thread t2([&] { for (int i = 0; i < kIter; ++i) ++plain; });
        t1.join(); t2.join();
    }

    // atomic：硬件级原子自增
    std::atomic<int> atomic_c{0};
    {
        std::thread t1([&] { for (int i = 0; i < kIter; ++i) ++atomic_c; });
        std::thread t2([&] { for (int i = 0; i < kIter; ++i) ++atomic_c; });
        t1.join(); t2.join();
    }

    // 互斥锁：适合保护「多个操作的整体」
    int guarded = 0;
    std::mutex mtx;
    {
        auto worker = [&] {
            for (int i = 0; i < kIter; ++i) {
                std::lock_guard<std::mutex> lk(mtx);
                ++guarded;
            }
        };
        std::thread t1(worker), t2(worker);
        t1.join(); t2.join();
    }

    std::cout << "  期望值            : " << 2 * kIter << "\n";
    std::cout << "  裸 int ++         : " << plain << "   <== 通常偏小（丢更新）\n";
    std::cout << "  atomic<int> ++    : " << atomic_c.load() << "\n";
    std::cout << "  mutex 保护的 int  : " << guarded << "\n";

    demo::line("单个变量选 atomic（更快）；多个变量要保持一致选 mutex。");
}

// -----------------------------------------------------------------------------
// 坑 31：条件变量不带谓词 —— 虚假唤醒与丢失唤醒
// -----------------------------------------------------------------------------
// 【现象】程序偶发卡死，或者唤醒后条件其实并不成立。
// 【原因】两个独立问题：
//         (a) 虚假唤醒：pthread 允许 wait 无理由返回，必须循环检查条件。
//         (b) 丢失唤醒：如果 notify 发生在 wait 之前，这次通知就丢了，
//             wait 会一直等下去。所以必须先检查「条件是否已满足」。
//         带谓词的 wait 一次性解决两个问题（它内部就是 while 循环）。
// 【记忆】永远写 cv.wait(lock, 谓词)，永远不写裸 cv.wait(lock)。
static void pitfall_31_condition_variable() {
    demo::section("坑 31  条件变量的谓词");

    std::mutex              mtx;
    std::condition_variable cv;
    bool                    ready = false;
    int                     data  = 0;

    std::thread consumer([&] {
        std::unique_lock<std::mutex> lk(mtx);
        // 错误：cv.wait(lk);
        //   - 可能虚假唤醒，条件没满足就往下走
        //   - 若生产者先 notify 了，这里会永远等待
        //
        // 正确：带谓词。等价于 while (!ready) cv.wait(lk);
        cv.wait(lk, [&] { return ready; });
        std::cout << "  消费者被唤醒，data=" << data << "\n";
    });

    // 故意让生产者先跑，模拟「notify 早于 wait」
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::lock_guard<std::mutex> lk(mtx);
        data  = 999;
        ready = true;            // 改共享状态必须持锁
    }
    cv.notify_one();             // notify 可以在锁外，减少锁竞争
    consumer.join();

    demo::line("三件套：持锁改状态 -> 解锁 -> notify；等待方一律用谓词版 wait。");
}

// -----------------------------------------------------------------------------
// 坑 32：死锁 —— 加锁顺序不一致
// -----------------------------------------------------------------------------
// 【现象】程序卡死，CPU 占用 0%。
// 【原因】线程 1 拿了 A 等 B，线程 2 拿了 B 等 A，互相等待。
// 【记忆】三种解法：(1) 全局固定加锁顺序；(2) std::scoped_lock 一次锁多个
//         （内部有死锁避免算法）；(3) 缩小临界区，避免持锁调用外部代码。
static void pitfall_32_deadlock() {
    demo::section("坑 32  死锁");

    std::mutex m1, m2;

    // 错误：两个线程加锁顺序相反
    //   线程A: lock(m1); lock(m2);
    //   线程B: lock(m2); lock(m1);   -> 可能死锁

    // 正确 1：scoped_lock 同时锁多个，顺序无关
    auto worker = [&](int id) {
        std::scoped_lock lk(m1, m2);   // C++17，内部用 std::lock 的死锁避免算法
        std::cout << "  线程 " << id << " 同时持有 m1 和 m2\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    std::thread t1(worker, 1), t2(worker, 2);
    t1.join(); t2.join();

    demo::line("C++11 只有 std::lock(m1, m2) + adopt_lock；C++17 起用 scoped_lock 更简洁。");
    demo::line("另一个隐形死锁：持锁时调用回调函数，回调又去加同一把锁（非递归锁自锁）。");
}

// -----------------------------------------------------------------------------
// 坑 33：thread 忘记 join/detach —— 析构直接 terminate
// -----------------------------------------------------------------------------
// 【现象】程序莫名 abort，没有任何错误信息。
// 【原因】std::thread 析构时如果仍是 joinable 状态，标准规定调用
//         std::terminate。这是刻意设计：默认 detach 会导致悬垂引用，
//         默认 join 会在析构里意外阻塞，两者都比直接崩更危险。
// 【记忆】C++20 起用 std::jthread，析构自动 request_stop + join。
static void pitfall_33_thread_lifetime() {
    demo::section("坑 33  thread 忘记 join");

    // {
    //     std::thread t([]{ /* ... */ });
    // }   // 析构时 joinable -> std::terminate，程序直接死

    {
        std::thread t([] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); });
        t.join();   // C++11/14/17 必须显式 join 或 detach
        std::cout << "  std::thread + 显式 join：正常\n";
    }

    {
        std::jthread jt([](std::stop_token st) {
            while (!st.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::cout << "  jthread 收到停止请求，优雅退出\n";
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }   // 析构：自动 request_stop() 然后 join()
    std::cout << "  std::jthread：析构自动 stop + join，不会 terminate\n";

    demo::line("detach 更危险：线程还在跑，主线程退出后它访问的对象已经销毁。");
}

// -----------------------------------------------------------------------------
// 坑 34：volatile 不是 atomic
// -----------------------------------------------------------------------------
// 【现象】用 volatile bool 做线程间标志，偶发失效。
// 【原因】volatile 只保证「每次都从内存读，不做寄存器缓存」，用于
//         内存映射硬件寄存器。它**不提供**原子性，也**不建立**内存屏障，
//         无法阻止 CPU 和编译器的指令重排。
// 【记忆】线程同步一律用 std::atomic；volatile 只用于硬件寄存器和信号处理。
static void pitfall_34_volatile() {
    demo::section("坑 34  volatile 不是 atomic");

    std::atomic<bool> stop_flag{false};   // 正确的线程间标志
    std::thread w([&] {
        int spins = 0;
        while (!stop_flag.load(std::memory_order_acquire)) ++spins;
        std::cout << "  worker 观察到 stop_flag，自旋了 " << spins << " 次\n";
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop_flag.store(true, std::memory_order_release);
    w.join();

    std::cout << "  volatile：只禁止编译器缓存到寄存器，无原子性、无内存序\n";
    std::cout << "  atomic  ：原子性 + 可指定内存序，才是线程同步工具\n";
}

// =============================================================================
// 第六组：其它高频坑
// =============================================================================

// -----------------------------------------------------------------------------
// 坑 35：sizeof 在函数参数里对数组失效
// -----------------------------------------------------------------------------
// 【现象】传数组进函数，sizeof 算出来永远是 8（指针大小）。
// 【原因】数组作为函数参数会「退化」(decay) 成指针，长度信息彻底丢失。
//         void f(int arr[10]) 和 void f(int* arr) 完全等价，那个 10 被忽略。
// 【记忆】传数组用 std::span（C++20）、std::array，或显式带上长度。
static void takes_array_bad(int arr[10]) {
    std::cout << "  函数内 sizeof(arr) = " << sizeof(arr)
              << "   <== 是指针大小，不是 40\n";
    (void)arr;
}
template <size_t N>
static void takes_array_ref(int (&arr)[N]) {   // 数组引用，保留长度
    std::cout << "  数组引用模板 N = " << N << "  sizeof = " << sizeof(arr) << "\n";
}

static void pitfall_35_array_decay() {
    demo::section("坑 35  数组退化");

    int arr[10]{};
    std::cout << "  函数外 sizeof(arr) = " << sizeof(arr) << "  元素个数="
              << std::size(arr) << "\n";
    takes_array_bad(arr);
    takes_array_ref(arr);

    demo::line("C++20 首选 std::span<int>：既保留长度，又能接受 vector/array/C 数组。");
}

// -----------------------------------------------------------------------------
// 坑 36：宏的副作用与优先级
// -----------------------------------------------------------------------------
// 【现象】MAX(i++, j++) 让变量多加了一次；SQUARE(1+2) 算出 5 而不是 9。
// 【原因】宏是纯文本替换，参数会被求值多次，且不带括号会破坏运算优先级。
// 【记忆】能用 constexpr 函数、模板、inline 就绝不用宏。
#define SQUARE_BAD(x)  x * x            // 无括号
#define SQUARE_OK(x)   ((x) * (x))      // 参数和整体都加括号（但仍会求值两次）
constexpr int square_best(int x) { return x * x; }   // 最佳：只求值一次，类型安全

static void pitfall_36_macro() {
    demo::section("坑 36  宏的陷阱");

    std::cout << "  SQUARE_BAD(1+2) = " << SQUARE_BAD(1 + 2)
              << "   <== 展开成 1+2*1+2 = 5\n";
    std::cout << "  SQUARE_OK(1+2)  = " << SQUARE_OK(1 + 2) << "\n";
    std::cout << "  square_best(1+2)= " << square_best(1 + 2) << "\n";

    int i = 5;
    std::cout << "  i=5, SQUARE_OK(i++) = " << SQUARE_OK(i++)
              << "  之后 i=" << i << "   <== i 被自增了两次\n";

    demo::line("min/max 宏还会和 std::min/std::max 冲突，Windows 上记得 #define NOMINMAX。");
}

// -----------------------------------------------------------------------------
// 坑 37：修改字符串字面量
// -----------------------------------------------------------------------------
// 【现象】程序在写入时崩溃（段错误 / 访问冲突）。
// 【原因】字面量存放在只读段（.rodata）。C++11 起 char* p = "abc" 已是
//         非法（需要 const char*），但老代码里很常见。
// 【记忆】要可修改的字符串就用 std::string 或 char 数组。
static void pitfall_37_string_literal() {
    demo::section("坑 37  修改字符串字面量");

    const char* ro = "只读字面量";
    // ro[0] = 'X';                  // 编译错误（好事）
    // char* bad = "abc"; bad[0]='X';   // C++11 前能编译，运行时崩溃

    char writable[] = "可修改的数组";   // 字面量内容被**拷贝**到栈数组里
    writable[0] = 'X';
    std::string s = "可修改的 string";
    s[0] = 'Y';

    std::cout << "  const char* 指向只读段，不能改\n";
    std::cout << "  char arr[] = \"...\"  是栈上副本，可以改: " << writable << "\n";
    std::cout << "  std::string 可以改: " << s << "\n";
    (void)ro;
}

// -----------------------------------------------------------------------------
// 坑 38：函数参数求值顺序不确定
// -----------------------------------------------------------------------------
// 【现象】f(i++, i++) 在不同编译器上结果不同。
// 【原因】C++17 前，函数参数的求值顺序完全未指定（unsequenced）；
//         C++17 起虽然规定「参数之间不交错」，但**顺序仍然未指定**。
// 【记忆】一条语句里不要对同一个变量做多次修改。
static void pitfall_38_eval_order() {
    demo::section("坑 38  求值顺序");

    // int i = 0;
    // printf("%d %d\n", i++, i++);   // 未指定：可能 "0 1" 也可能 "1 0"
    // v[i] = i++;                     // C++17 前是 UB

    // 正确：拆成多条语句，顺序明确
    int i = 0;
    int a = i++;
    int b = i++;
    std::cout << "  拆开写: a=" << a << " b=" << b << " i=" << i << "  确定无歧义\n";

    demo::line("C++17 起确定顺序的：<< >> 从左到右；a.b()、a->b() 先求 a；赋值先求右侧。");
}

// -----------------------------------------------------------------------------
// 坑 39：异常安全 —— new 的参数抛异常导致泄漏
// -----------------------------------------------------------------------------
// 【现象】某个函数抛异常后内存泄漏。
// 【原因】f(new A, new B) 中，若第一个 new 成功、第二个 new 抛异常，
//         第一块内存就没人管了（还没进入 f，也没交给智能指针）。
// 【记忆】一条语句只做一次资源分配；用 make_unique / make_shared。
static void takes_two(std::unique_ptr<int> a, std::unique_ptr<int> b) {
    std::cout << "  安全接收: " << *a << ", " << *b << "\n";
}

static void pitfall_39_exception_safety() {
    demo::section("坑 39  异常安全与资源泄漏");

    // 有风险（C++14 及更早尤其明显）：
    // takes_two(std::unique_ptr<int>(new int(1)), std::unique_ptr<int>(new int(2)));

    // 安全：make_unique 把「分配」和「接管所有权」绑成一个不可分割的操作
    takes_two(std::make_unique<int>(1), std::make_unique<int>(2));

    // RAII 通用守卫：任何路径退出都会执行清理
    struct ScopeGuard {
        std::function<void()> fn;
        bool active = true;
        ~ScopeGuard() { if (active) fn(); }
        void dismiss() { active = false; }
    };
    {
        ScopeGuard g{[] { std::cout << "  ScopeGuard 触发清理（异常/return/正常结束都会走）\n"; }};
    }

    demo::line("RAII 是 C++ 异常安全的唯一可靠手段。手写 try/catch 清理必然遗漏路径。");
}

// -----------------------------------------------------------------------------
// 坑 40：map 的 operator[] 会悄悄插入元素
// -----------------------------------------------------------------------------
// 【现象】只是「查一下」，map 却变大了；或者 const map 上 [] 编译不过。
// 【原因】operator[] 的语义是「返回该键的引用，不存在就默认构造插入」。
//         所以它不可能是 const 成员函数。
// 【记忆】查用 find / at / contains，写才用 []。
static void pitfall_40_map_subscript() {
    demo::section("坑 40  map::operator[] 的副作用");

    std::map<std::string, int> m{{"a", 1}};

    std::cout << "  查询前 size=" << m.size() << "\n";
    int v = m["不存在的键"];              // 悄悄插入了 {"不存在的键", 0}
    std::cout << "  m[\"不存在的键\"] = " << v << "  查询后 size=" << m.size()
              << "   <== 变大了！\n";

    // 正确的查询方式
    if (auto it = m.find("a"); it != m.end())
        std::cout << "  find(\"a\")     -> " << it->second << "  不会插入\n";
    if (m.contains("a"))                      // C++20
        std::cout << "  contains(\"a\") -> true  不会插入\n";
    try {
        (void)m.at("还是不存在");              // 抛异常，不插入（at 是 [[nodiscard]]）
    } catch (const std::out_of_range&) {
        std::cout << "  at(\"不存在\")  -> 抛 out_of_range，不会插入\n";
    }
    std::cout << "  最终 size=" << m.size() << "\n";

    demo::line("插入优选 try_emplace（键存在时不构造 value）或 insert_or_assign。");
}

// =============================================================================
// main
// =============================================================================
int main() {
    demo::title("第 13 章  C++ 易错点大全");
    std::cout <<
        "  每个坑的格式：【现象】【原因】【错误】【正确】【记忆】\n"
        "  源码里的注释比输出详细得多，请对照 src/ch13_pitfalls/main.cpp 阅读。\n";

    demo::title("一、初始化与类型系统");
    pitfall_01_brace_vs_paren();
    pitfall_02_auto_decay();
    pitfall_03_vector_bool();
    pitfall_04_signed_unsigned();
    pitfall_05_overflow();
    pitfall_06_float_compare();

    demo::title("二、指针、引用、生命周期");
    pitfall_07_dangling_return();
    pitfall_08_string_view_dangling();
    pitfall_09_iterator_invalidation();
    pitfall_10_erase_remove();
    pitfall_11_new_delete_mismatch();
    pitfall_12_const_position();

    demo::title("三、类与对象");
    pitfall_13_virtual_dtor();
    pitfall_14_virtual_in_ctor();
    pitfall_15_member_init_order();
    pitfall_16_explicit();
    pitfall_17_slicing();
    pitfall_18_name_hiding();
    pitfall_19_self_assignment();
    pitfall_20_use_after_move();

    demo::title("四、现代 C++ 特性的新型坑");
    pitfall_21_lambda_capture();
    pitfall_22_capture_this();
    pitfall_23_shared_ptr_cycle();
    pitfall_24_double_managed();
    pitfall_25_shared_from_this();
    pitfall_26_forwarding_ref_overload();
    pitfall_27_bad_move();
    pitfall_28_forward();
    pitfall_29_auto_braces();

    demo::title("五、并发");
    pitfall_30_data_race();
    pitfall_31_condition_variable();
    pitfall_32_deadlock();
    pitfall_33_thread_lifetime();
    pitfall_34_volatile();

    demo::title("六、其它高频坑");
    pitfall_35_array_decay();
    pitfall_36_macro();
    pitfall_37_string_literal();
    pitfall_38_eval_order();
    pitfall_39_exception_safety();
    pitfall_40_map_subscript();

    demo::title("速查：最容易犯的十个");
    std::cout <<
        "   1. 基类析构没写 virtual                -> 派生类资源泄漏\n"
        "   2. 迭代器失效后继续用                  -> 随机崩溃\n"
        "   3. string_view / span 悬垂             -> 读已释放内存\n"
        "   4. shared_ptr 循环引用                 -> 内存永不释放\n"
        "   5. lambda 用 [&] 捕获后存起来异步执行  -> 悬垂引用\n"
        "   6. size() - 1 在空容器上               -> 无符号下溢\n"
        "   7. 条件变量 wait 不带谓词              -> 偶发卡死\n"
        "   8. map 用 [] 做查询                    -> 悄悄插入\n"
        "   9. std::remove 后忘记 erase            -> 元素没删掉\n"
        "  10. move 之后继续使用原对象            -> 值不确定\n"
        "\n"
        "  工具比记忆可靠，务必开起来：\n"
        "    MSVC : /W4 /permissive- /fsanitize=address，再加 Code Analysis\n"
        "    GCC  : -Wall -Wextra -Wpedantic -fsanitize=address,undefined\n"
        "    静态 : clang-tidy（bugprone-* / cppcoreguidelines-* 两组规则）\n";

    return 0;
}
