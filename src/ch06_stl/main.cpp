// =============================================================================
// 第 6 章 —— 标准库（STL）实战
//
// 内容：容器怎么选、算法怎么用、字符串、时间、随机数、函数对象、类型特征。
// 这一章是「查手册式」的，跑一遍留个印象，用的时候回来翻。
//
// 运行： ch06_stl.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <deque>
#include <list>
#include <forward_list>
#include <array>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <stack>
#include <queue>
#include <string>
#include <string_view>
#include <algorithm>
#include <numeric>
#include <functional>
#include <chrono>
#include <random>
#include <charconv>
#include <sstream>
#include <fstream>
#include <type_traits>
#include <utility>
#include <memory>
#include <stdexcept>

using namespace std::chrono;

// 自定义类型：演示自定义哈希、比较器、排序
struct Person {
    std::string name;
    int         age;
    bool operator<(const Person& o) const { return age < o.age; }
    bool operator==(const Person& o) const { return name == o.name && age == o.age; }
};
std::ostream& operator<<(std::ostream& os, const Person& p) {
    return os << p.name << "(" << p.age << ")";
}
struct PersonHash {
    std::size_t operator()(const Person& p) const noexcept {
        // 组合哈希的常见做法（boost::hash_combine 的简化版）
        std::size_t h1 = std::hash<std::string>{}(p.name);
        std::size_t h2 = std::hash<int>{}(p.age);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

template <typename C>
void dump(std::string_view name, const C& c) {
    std::cout << "    " << std::left << std::setw(22) << name << ": [ ";
    for (const auto& e : c) std::cout << e << " ";
    std::cout << "]\n";
}

// 简单计时器
template <typename F>
long long time_us(F&& f) {
    auto t0 = steady_clock::now();
    f();
    return duration_cast<microseconds>(steady_clock::now() - t0).count();
}

int main() {
    std::cout << "__cplusplus = " << __cplusplus << "\n";

    // =========================================================================
    demo::title("6.1 容器怎么选 —— 决策树");
    // =========================================================================
    {
        demo::line("需要按【键】查找吗?");
        demo::line("  是 -> 需要【有序遍历 / 范围查询】吗?");
        demo::line("        是 -> map / set            (红黑树, O(log n), 迭代器稳定)");
        demo::line("        否 -> unordered_map / set  (哈希表, 均摊 O(1))");
        demo::line("  否 -> 大小编译期已知?");
        demo::line("        是 -> array                (栈上, 零开销)");
        demo::line("        否 -> 只在【尾部】增删?");
        demo::line("              是 -> vector          <== 默认就选它");
        demo::line("              否 -> 【两端】都增删?");
        demo::line("                    是 -> deque");
        demo::line("                    否 -> 需要【迭代器永久有效】或 O(1) splice?");
        demo::line("                          是 -> list");
        demo::line("                          否 -> 还是 vector（实测通常更快）");
        demo::line("");
        demo::line("经验法则：不确定就用 vector。链表的理论优势在现代 CPU 上");
        demo::line("常常被缓存不命中吃光 —— 遍历 vector 比遍历 list 快 5~10 倍很常见。");
    }

    // =========================================================================
    demo::title("6.2 序列容器");
    // =========================================================================
    {
        demo::section("vector —— 90% 的场合");
        std::vector<int> v{3, 1, 4, 1, 5};
        v.push_back(9);
        v.emplace_back(2);                     // 原位构造，比 push_back(T(...)) 少一次移动
        dump("vector", v);
        SHOW(v.size());
        SHOW(v.capacity());
        SHOW(v.front()); SHOW(v.back());
        SHOW(v[0]);
        SHOW(v.at(0));                          // at 有边界检查，越界抛异常

        demo::section("reserve —— 最容易拿到的性能提升");
        {
            auto without = time_us([] {
                std::vector<int> t;
                for (int i = 0; i < 200000; ++i) t.push_back(i);
            });
            auto with = time_us([] {
                std::vector<int> t;
                t.reserve(200000);
                for (int i = 0; i < 200000; ++i) t.push_back(i);
            });
            std::cout << "    不 reserve: " << without << " us\n";
            std::cout << "    先 reserve: " << with    << " us\n";
        }

        demo::section("扩容会让所有迭代器 / 指针 / 引用失效");
        {
            std::vector<int> t{1, 2, 3};
            int* p = &t[0];
            SHOW(*p);
            t.push_back(4);                     // 可能重新分配
            demo::line("此时 p 可能已经悬垂，再解引用就是未定义行为。");
            (void)p;
        }

        demo::section("删除元素");
        std::vector<int> w{1, 2, 3, 4, 5, 6};
        std::erase_if(w, [](int n) { return n % 2 == 0; });   // C++20 一行
        dump("删掉偶数", w);
        demo::line("C++17 及以前: w.erase(std::remove_if(w.begin(),w.end(),pred), w.end());");
        demo::line("注意 remove_if 本身【不删元素】，它只是把要保留的挪到前面并返回新逻辑末尾。");

        demo::section("无序容器里的 O(1) 删除技巧");
        std::vector<int> u{10, 20, 30, 40};
        size_t idx = 1;
        std::swap(u[idx], u.back());
        u.pop_back();
        dump("swap-and-pop", u);
        demo::line("不保序，但从 O(n) 变 O(1)。游戏和实时系统里非常常用。");

        demo::section("deque —— 两端都要操作");
        std::deque<int> d{2, 3};
        d.push_front(1);
        d.push_back(4);
        dump("deque", d);
        demo::line("分段连续存储：随机访问只比 vector 慢一点，但两端插入都是 O(1)。");
        demo::line("注意：deque 内存不连续，不能用 &d[0] 当数组指针传给 C API。");

        demo::section("list —— 双向链表");
        std::list<int> l1{1, 2, 3}, l2{7, 8};
        auto it = l1.begin(); ++it;
        l1.splice(it, l2);                     // O(1) 把 l2 整个搬进来
        dump("splice 之后 l1", l1);
        SHOW(l2.size());
        demo::line("list 的真正优势：splice（O(1) 转移节点）和「迭代器永不失效」。");
        demo::line("如果你只是想在中间插入，vector 通常仍然更快。");

        demo::section("array —— 栈上定长");
        std::array<int, 5> a{1, 2, 3, 4, 5};
        dump("array", a);
        SHOW(a.size());
        demo::line("相比 C 数组：有 size()、不会退化成指针、可以整体赋值、可以进容器。");
    }

    // =========================================================================
    demo::title("6.3 关联容器");
    // =========================================================================
    {
        demo::section("map —— 有序键值");
        std::map<std::string, int> m{{"banana", 2}, {"apple", 1}, {"cherry", 3}};
        for (const auto& [k, v] : m) std::cout << "    " << k << " -> " << v << "\n";
        demo::line("遍历自动按键有序（红黑树中序遍历）。");

        demo::section("四种插入方式的区别");
        m["date"] = 4;                          // 不存在则默认构造 + 赋值
        m.insert({"elder", 5});                 // 已存在则什么都不做
        m.try_emplace("fig", 6);                // 已存在则【不构造 value】—— 最省
        m.insert_or_assign("apple", 100);       // 存在就覆盖
        SHOW(m["apple"]);
        SHOW(m.size());

        demo::section("operator[] 的陷阱");
        std::map<std::string, int> counter;
        SHOW(counter.size());
        if (counter["not_exist"] == 0) {}       // 这一句【插入了】一个元素！
        SHOW(counter.size());
        demo::line("只读查询请用 find / contains / at，别用 []。");
        SHOW(counter.contains("x"));            // C++20

        demo::section("范围查询 —— map 相对 unordered_map 的独门优势");
        std::map<int, std::string> events{{100, "a"}, {200, "b"}, {300, "c"}, {400, "d"}};
        auto lo = events.lower_bound(150);      // 第一个 >= 150
        auto hi = events.upper_bound(350);      // 第一个 > 350
        std::cout << "    [150, 350] 区间: ";
        for (auto i = lo; i != hi; ++i) std::cout << i->first << " ";
        std::cout << "\n";

        demo::section("multimap —— 一键多值");
        std::multimap<std::string, int> mm{{"a", 1}, {"a", 2}, {"b", 3}};
        auto [b, e] = mm.equal_range("a");
        std::cout << "    key=a 的所有值: ";
        for (auto i = b; i != e; ++i) std::cout << i->second << " ";
        std::cout << "\n";

        demo::section("unordered_map —— 哈希表");
        std::unordered_map<std::string, int> um;
        um.reserve(100);                        // 预留桶，减少 rehash
        um["x"] = 1; um["y"] = 2;
        SHOW(um.size());
        SHOW(um.bucket_count());
        SHOW(um.load_factor());
        demo::line("平均 O(1)，但最坏 O(n)（哈希冲突严重时）。遍历顺序不确定。");

        demo::section("自定义类型做 key");
        std::set<Person> ps{{"张三", 30}, {"李四", 25}};       // 需要 operator< 或比较器
        dump("set<Person>", ps);
        std::unordered_set<Person, PersonHash> ups{{"王五", 40}};  // 需要 hash + ==
        dump("unordered_set", ups);
        demo::line("map/set 需要【严格弱序】的比较；unordered 需要【hash + 相等】。");

        demo::section("map vs unordered_map 怎么选");
        demo::line("要有序遍历 / 范围查询 / 前驱后继      -> map");
        demo::line("只做单点查找，追求速度                -> unordered_map");
        demo::line("键很少（< 30 个）                     -> 排好序的 vector 往往最快");
        demo::line("延迟要稳定（不能容忍 rehash 卡顿）    -> map");
    }

    // =========================================================================
    demo::title("6.4 容器适配器");
    // =========================================================================
    {
        std::stack<int> st;
        st.push(1); st.push(2); st.push(3);
        std::cout << "    stack 弹出顺序: ";
        while (!st.empty()) { std::cout << st.top() << " "; st.pop(); }
        std::cout << "\n";

        std::queue<int> q;
        q.push(1); q.push(2); q.push(3);
        std::cout << "    queue 弹出顺序: ";
        while (!q.empty()) { std::cout << q.front() << " "; q.pop(); }
        std::cout << "\n";

        std::priority_queue<int> maxh;
        for (int n : {3, 1, 4, 1, 5}) maxh.push(n);
        std::cout << "    大顶堆:         ";
        while (!maxh.empty()) { std::cout << maxh.top() << " "; maxh.pop(); }
        std::cout << "\n";

        std::priority_queue<int, std::vector<int>, std::greater<>> minh;
        for (int n : {3, 1, 4, 1, 5}) minh.push(n);
        std::cout << "    小顶堆:         ";
        while (!minh.empty()) { std::cout << minh.top() << " "; minh.pop(); }
        std::cout << "\n";
        demo::line("【用在哪】priority_queue: Dijkstra、任务调度、Top-K、定时器堆。");
    }

    // =========================================================================
    demo::title("6.5 算法 —— 排序 / 查找 / 变换 / 归约");
    // =========================================================================
    {
        std::vector<int> v{5, 3, 8, 1, 9, 2, 7};

        demo::section("排序家族");
        auto t = v; std::sort(t.begin(), t.end());                       dump("sort", t);
        t = v; std::sort(t.begin(), t.end(), std::greater<>{});          dump("sort 降序", t);
        t = v; std::partial_sort(t.begin(), t.begin() + 3, t.end());     dump("partial_sort 前3", t);
        t = v; std::nth_element(t.begin(), t.begin() + 3, t.end());      dump("nth_element", t);
        demo::line("nth_element: 只保证第 n 个元素就位，左边都不大于它 —— O(n)，求中位数用它。");
        t = v; std::stable_sort(t.begin(), t.end());                     dump("stable_sort", t);
        demo::line("stable_sort 保持相等元素的原有相对顺序，代价是需要额外内存。");

        demo::section("按对象成员排序");
        std::vector<Person> ps{{"张三", 30}, {"李四", 25}, {"王五", 35}};
        std::sort(ps.begin(), ps.end(), [](const Person& a, const Person& b) {
            return a.age < b.age;
        });
        dump("按年龄排序", ps);

        demo::section("有序区间上的二分查找 —— O(log n)");
        std::vector<int> s{1, 2, 3, 5, 7, 8, 9};
        SHOW(std::binary_search(s.begin(), s.end(), 5));
        SHOW(*std::lower_bound(s.begin(), s.end(), 5));   // 第一个 >= 5
        SHOW(*std::upper_bound(s.begin(), s.end(), 5));   // 第一个 > 5
        demo::line("前提：区间必须已排序！没排序结果是未定义的。");

        demo::section("线性查找");
        SHOW(*std::find(v.begin(), v.end(), 8));
        SHOW(*std::find_if(v.begin(), v.end(), [](int n) { return n > 6; }));
        SHOW(std::count_if(v.begin(), v.end(), [](int n) { return n % 2; }));
        SHOW(std::all_of(v.begin(), v.end(), [](int n) { return n > 0; }));
        SHOW(std::any_of(v.begin(), v.end(), [](int n) { return n > 8; }));
        SHOW(std::none_of(v.begin(), v.end(), [](int n) { return n > 100; }));

        demo::section("最值");
        SHOW(*std::min_element(v.begin(), v.end()));
        SHOW(*std::max_element(v.begin(), v.end()));
        auto [mn, mx] = std::minmax_element(v.begin(), v.end());
        std::cout << "    minmax: " << *mn << " / " << *mx << "\n";

        demo::section("变换");
        std::vector<int> out(v.size());
        std::transform(v.begin(), v.end(), out.begin(), [](int n) { return n * n; });
        dump("平方", out);
        std::vector<int> sum2(v.size());
        std::transform(v.begin(), v.end(), out.begin(), sum2.begin(), std::plus<>{});
        dump("二元 transform", sum2);

        demo::section("去重（必须先排序）");
        std::vector<int> dup{1, 1, 2, 3, 3, 3, 4};
        dup.erase(std::unique(dup.begin(), dup.end()), dup.end());
        dump("unique", dup);
        demo::line("unique 只去掉【相邻】的重复项，所以一般先 sort。");

        demo::section("划分");
        auto p = v;
        auto mid = std::partition(p.begin(), p.end(), [](int n) { return n % 2 == 0; });
        std::cout << "    偶数在前: ";
        for (auto i = p.begin(); i != mid; ++i) std::cout << *i << " ";
        std::cout << "| ";
        for (auto i = mid; i != p.end(); ++i) std::cout << *i << " ";
        std::cout << "\n";

        demo::section("集合运算（要求两边都有序）");
        std::vector<int> A{1, 2, 3, 4, 5}, B{3, 4, 5, 6, 7}, R;
        R.clear(); std::set_union(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(R));
        dump("并集", R);
        R.clear(); std::set_intersection(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(R));
        dump("交集", R);
        R.clear(); std::set_difference(A.begin(), A.end(), B.begin(), B.end(), std::back_inserter(R));
        dump("差集 A-B", R);

        demo::section("数值算法 <numeric>");
        SHOW(std::accumulate(v.begin(), v.end(), 0));
        SHOW(std::accumulate(v.begin(), v.end(), 1, std::multiplies<>{}));
        SHOW(std::inner_product(A.begin(), A.end(), B.begin(), 0));      // 点积
        SHOW(std::reduce(v.begin(), v.end()));                           // 可并行版
        SHOW(std::transform_reduce(v.begin(), v.end(), 0, std::plus<>{},
                                   [](int n) { return n * n; }));        // map-reduce

        std::vector<int> psum(v.size());
        std::partial_sum(v.begin(), v.end(), psum.begin());
        dump("前缀和", psum);

        std::vector<int> diff(v.size());
        std::adjacent_difference(v.begin(), v.end(), diff.begin());
        dump("相邻差分", diff);

        std::vector<int> io(6);
        std::iota(io.begin(), io.end(), 10);
        dump("iota 从10开始", io);

        demo::section("accumulate vs reduce");
        demo::line("accumulate 严格从左到右 -> 不能并行，但浮点结果确定");
        demo::line("reduce     顺序不保证   -> 能并行，要求运算满足结合律");

        demo::section("堆算法");
        auto h = v;
        std::make_heap(h.begin(), h.end());
        dump("make_heap", h);
        std::pop_heap(h.begin(), h.end());          // 把最大值挪到末尾
        int top = h.back(); h.pop_back();
        SHOW(top);
    }

    // =========================================================================
    demo::title("6.6 字符串");
    // =========================================================================
    {
        std::string s = "hello world";
        SHOW(s.size());
        SHOW(s.substr(0, 5));
        SHOW(s.find("world"));
        SHOW(s.find("xyz") == std::string::npos);
        SHOW(s.starts_with("hello"));
        SHOW(s.ends_with("world"));

        demo::section("拼接性能");
        {
            auto slow = time_us([] {
                std::string r;
                for (int i = 0; i < 20000; ++i) r = r + "x";      // 每次都产生新字符串
            });
            auto fast = time_us([] {
                std::string r;
                r.reserve(20000);
                for (int i = 0; i < 20000; ++i) r += "x";         // 原地追加
            });
            std::cout << "    r = r + \"x\"  : " << slow << " us\n";
            std::cout << "    r += \"x\" (预留): " << fast << " us\n";
        }

        demo::section("数值转换：三种方式");
        SHOW(std::to_string(42));
        SHOW(std::stoi("42"));
        SHOW(std::stod("3.14"));

        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), 12345);
        std::cout << "    to_chars: " << std::string(buf, ptr) << "\n";
        int parsed{};
        std::string_view src = "9876";
        std::from_chars(src.data(), src.data() + src.size(), parsed);
        SHOW(parsed);
        demo::line("stoi/stod : 会抛异常、看 locale、慢");
        demo::line("to_chars  : 不分配、不抛、不看 locale —— 热点路径首选（C++17）");

        demo::section("字符串分割（标准库没直接给，两种常用写法）");
        auto split = [](std::string_view sv, char delim) {
            std::vector<std::string_view> out;
            size_t start = 0;
            while (start <= sv.size()) {
                size_t pos = sv.find(delim, start);
                if (pos == std::string_view::npos) { out.push_back(sv.substr(start)); break; }
                out.push_back(sv.substr(start, pos - start));
                start = pos + 1;
            }
            return out;
        };
        auto parts = split("a,b,c,dd", ',');
        std::cout << "    split: ";
        for (auto p : parts) std::cout << "[" << p << "] ";
        std::cout << "\n";

        demo::section("stringstream —— 方便但不快");
        std::ostringstream oss;
        oss << "x=" << 1 << ", y=" << 2.5;
        SHOW(oss.str());
        std::istringstream iss("10 20 30");
        int a, b, c; iss >> a >> b >> c;
        std::cout << "    解析: " << a << " " << b << " " << c << "\n";
        demo::line("【用在哪】不追求性能的拼装与解析。热点路径用 format / to_chars。");
    }

    // =========================================================================
    demo::title("6.7 时间 <chrono>");
    // =========================================================================
    {
        using namespace std::chrono_literals;

        demo::section("duration —— 时长");
        auto total = 1h + 30min + 45s;
        SHOW(duration_cast<seconds>(total).count());
        SHOW(duration_cast<minutes>(total).count());
        SHOW(duration<double>(total).count());
        demo::line("类型安全：hours + minutes 会自动换算，不会像 int 那样单位搞混。");

        demo::section("三种时钟");
        demo::line("steady_clock         单调递增，绝不回退  -> 【测耗时用它】");
        demo::line("system_clock         挂钟时间，可被调整  -> 显示日期、时间戳");
        demo::line("high_resolution_clock 通常就是上面某一个 -> 不推荐直接用");

        auto t0 = steady_clock::now();
        volatile long long acc = 0;
        for (int i = 0; i < 1000000; ++i) acc += i;
        auto dt = steady_clock::now() - t0;
        std::cout << "    循环 100 万次耗时: "
                  << duration_cast<microseconds>(dt).count() << " us\n";

        demo::section("字面量");
        auto timeout = 500ms;
        SHOW(timeout.count());
    }

    // =========================================================================
    demo::title("6.8 随机数 <random>");
    // =========================================================================
    {
        demo::line("不要用 rand()：周期短、低位不随机、取模有偏差、线程不安全。");

        std::random_device rd;                  // 熵源（可能很慢，只用来播种）
        std::mt19937 gen(rd());                 // 梅森旋转引擎

        std::uniform_int_distribution<int>     dice(1, 6);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::normal_distribution<double>       gauss(0.0, 1.0);

        std::cout << "    掷骰子 10 次: ";
        for (int i = 0; i < 10; ++i) std::cout << dice(gen) << " ";
        std::cout << "\n";
        std::cout << "    [0,1) 均匀:   " << unit(gen) << "\n";
        std::cout << "    正态(0,1):    " << gauss(gen) << "\n";

        demo::section("加权随机");
        std::discrete_distribution<> weighted({10, 30, 60});
        int cnt[3]{};
        for (int i = 0; i < 1000; ++i) ++cnt[weighted(gen)];
        std::cout << "    权重 10:30:60 抽 1000 次 -> " << cnt[0] << " " << cnt[1] << " " << cnt[2] << "\n";

        demo::section("洗牌");
        std::vector<int> deck(10);
        std::iota(deck.begin(), deck.end(), 1);
        std::shuffle(deck.begin(), deck.end(), gen);
        dump("shuffle", deck);

        demo::line("引擎和分布是分开的：引擎产生均匀比特流，分布把它塑形。");
        demo::line("多线程：每个线程一个 thread_local 引擎，别共享。");
    }

    // =========================================================================
    demo::title("6.9 函数对象 <functional>");
    // =========================================================================
    {
        demo::section("std::function —— 类型擦除的可调用对象");
        std::function<int(int, int)> f;
        f = [](int a, int b) { return a + b; };      SHOW(f(1, 2));
        f = std::multiplies<int>{};                  SHOW(f(3, 4));
        f = [](int a, int b) { return a - b; };      SHOW(f(10, 3));
        SHOW(static_cast<bool>(f));
        demo::line("代价：可能有堆分配 + 间接调用，无法内联。热点循环里慎用。");
        demo::line("【用在哪】回调注册表、事件总线、命令队列、需要「存起来以后调用」的场合。");

        demo::section("std::bind vs lambda");
        auto add = [](int a, int b) { return a + b; };
        auto add5_bind   = std::bind(add, 5, std::placeholders::_1);
        auto add5_lambda = [add](int b) { return add(5, b); };
        SHOW(add5_bind(3));
        SHOW(add5_lambda(3));
        demo::line("现代 C++ 一律用 lambda：更快、更清晰、报错更友好。bind 只在读老代码时需要认识。");

        demo::section("透明函数对象");
        std::vector<int> v{3, 1, 2};
        std::sort(v.begin(), v.end(), std::greater<>{});     // <> 表示类型自动推导
        dump("greater<> 降序", v);

        demo::section("std::invoke —— 统一调用语法");
        struct S { int m = 7; int get() const { return m; } };
        S s;
        SHOW(std::invoke(&S::get, s));      // 成员函数
        SHOW(std::invoke(&S::m, s));        // 成员变量
        SHOW(std::invoke(add, 1, 2));       // 普通可调用

        demo::section("std::ref —— 把引用放进容器 / 传给按值取参的模板");
        int x = 1;
        std::vector<std::reference_wrapper<int>> refs{std::ref(x)};
        refs[0].get() = 99;
        SHOW(x);
    }

    // =========================================================================
    demo::title("6.10 类型特征 <type_traits>");
    // =========================================================================
    {
        demo::section("查询（都有 _v 后缀的变量模板版）");
        SHOW(std::is_integral_v<int>);
        SHOW(std::is_floating_point_v<double>);
        SHOW(std::is_pointer_v<int*>);
        SHOW(std::is_same_v<int, int>);
        SHOW(std::is_base_of_v<std::exception, std::runtime_error>);
        SHOW(std::is_trivially_copyable_v<int>);                  // 能否 memcpy
        SHOW(std::is_trivially_copyable_v<std::string>);
        SHOW(std::is_nothrow_move_constructible_v<std::vector<int>>);

        demo::section("变换（都有 _t 后缀的别名版）");
        SHOW(std::is_same_v<std::remove_reference_t<int&>, int>);
        SHOW(std::is_same_v<std::remove_cv_t<const int>, int>);
        SHOW(std::is_same_v<std::decay_t<const int&>, int>);
        SHOW(std::is_same_v<std::conditional_t<true, int, double>, int>);
        SHOW(std::is_same_v<std::common_type_t<int, double>, double>);

        demo::line("【用在哪】写泛型库时做编译期判断；C++20 起大多可以用 concepts 代替，更清晰。");
    }

    // =========================================================================
    demo::title("6.11 智能指针的选型");
    // =========================================================================
    {
        demo::line("unique_ptr : 独占。零开销（和裸指针一样大一样快）。默认选它。");
        demo::line("shared_ptr : 共享。控制块 + 原子引用计数，有开销。真需要共享才用。");
        demo::line("weak_ptr   : 观察者。打破循环引用；缓存；「对象还活着吗」。");
        demo::line("");
        demo::line("函数参数怎么传：");
        demo::line("  只是用一下，不管生命周期    -> const T&  或  T*");
        demo::line("  要接管所有权                -> std::unique_ptr<T>（按值传）");
        demo::line("  要共享所有权                -> std::shared_ptr<T>（按值传）");
        demo::line("  只是想读 shared_ptr 指的东西 -> 还是传 const T&，别传 shared_ptr");
        SHOW(sizeof(int*));
        SHOW(sizeof(std::unique_ptr<int>));
        SHOW(sizeof(std::shared_ptr<int>));
    }

    std::cout << "\n第 6 章结束。\n";
    return 0;
}
