// =============================================================================
// 第 18 章 —— 练习题参考答案（附录 B.1 语法与 STL、B.2 设计模式）
//
// 对应练习：第 1~8 题
//
// 「参考答案」的意思是：这是一种可用的写法，不是唯一写法。
// 每题都标注了【本题考什么】和【常见错法】—— 后者往往比答案本身更有价值。
//
// 所有答案都带自检，运行后能看到每一项是否通过。
//
// 运行： ch18_solutions.exe
// =============================================================================

#include "demo.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "    [通过] " : "    [失败] ") << what << "\n";
    if (!ok) ++g_failures;
}

// =============================================================================
// 第 1 题：SafeVector<T>
//
// 要求：operator[] 带边界检查（越界抛异常），其余接口转发给内部 vector，
//       支持移动、不支持拷贝。
//
// 【本题考什么】
//   - 用「组合」而不是「继承」包装标准容器
//   - 完美转发（emplace_back 要能接受任意参数）
//   - 五法则里的「只要移动不要拷贝」怎么写
//   - const 重载
//
// 【常见错法】
//   ✗ 继承 std::vector<T>。标准容器的析构函数**不是** virtual，
//     通过基类指针删除派生对象是 UB（第 13 章 3.1）。而且会继承到
//     一堆你不想暴露的接口。
//   ✗ 只写 operator[] 的非 const 版本 -> const 对象上没法读。
//   ✗ 转发 emplace_back 时写成 (Args... args) 按值 -> 多一次拷贝。
// =============================================================================
template <typename T>
class SafeVector {
public:
    using value_type = T;
    using size_type  = std::size_t;
    using iterator       = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;

    SafeVector() = default;
    SafeVector(std::initializer_list<T> il) : data_(il) {}
    explicit SafeVector(size_type n, const T& v = T()) : data_(n, v) {}

    // 只要移动，不要拷贝 —— 这两行 delete 就是全部实现
    SafeVector(const SafeVector&)            = delete;
    SafeVector& operator=(const SafeVector&) = delete;
    SafeVector(SafeVector&&) noexcept        = default;
    SafeVector& operator=(SafeVector&&) noexcept = default;

    // 带边界检查的下标。两个版本：非 const 返回可写引用，const 返回只读引用。
    T& operator[](size_type i) {
        if (i >= data_.size()) throwOutOfRange(i);
        return data_[i];
    }
    const T& operator[](size_type i) const {
        if (i >= data_.size()) throwOutOfRange(i);
        return data_[i];
    }

    // 转发接口：用完美转发，避免多余的拷贝/移动
    template <typename... Args>
    T& emplace_back(Args&&... args) {
        return data_.emplace_back(std::forward<Args>(args)...);
    }
    void push_back(const T& v) { data_.push_back(v); }
    void push_back(T&& v)      { data_.push_back(std::move(v)); }
    void pop_back()            { data_.pop_back(); }
    void reserve(size_type n)  { data_.reserve(n); }
    void clear() noexcept      { data_.clear(); }

    size_type size()     const noexcept { return data_.size(); }
    size_type capacity() const noexcept { return data_.capacity(); }
    bool      empty()    const noexcept { return data_.empty(); }

    iterator       begin()       noexcept { return data_.begin(); }
    iterator       end()         noexcept { return data_.end(); }
    const_iterator begin() const noexcept { return data_.begin(); }
    const_iterator end()   const noexcept { return data_.end(); }

    // 逃生门：需要直接操作底层 vector 时用它，比暴露继承关系可控得多
    const std::vector<T>& raw() const noexcept { return data_; }

private:
    [[noreturn]] void throwOutOfRange(size_type i) const {
        std::ostringstream ss;
        ss << "SafeVector 下标越界: 索引 " << i << "，size = " << data_.size();
        throw std::out_of_range(ss.str());
    }

    std::vector<T> data_;
};

void solve01() {
    demo::section("第 1 题  SafeVector<T>");

    SafeVector<std::string> v{"a", "b", "c"};
    check(v.size() == 3, "初始化列表构造，size == 3");
    check(v[1] == "b", "正常下标读取");

    v[1] = "B";
    check(v[1] == "B", "非 const 下标可写");

    bool caught = false;
    try { (void)v[99]; } catch (const std::out_of_range& e) {
        caught = true;
        std::cout << "      异常信息: " << e.what() << "\n";
    }
    check(caught, "越界抛 std::out_of_range");

    // const 对象上也能读
    const SafeVector<std::string>& cv = v;
    check(cv[0] == "a", "const 对象可读");

    // 移动可用，拷贝在编译期就被禁止
    SafeVector<std::string> moved = std::move(v);
    check(moved.size() == 3, "移动构造成功");
    static_assert(!std::is_copy_constructible_v<SafeVector<int>>, "不该可拷贝");
    static_assert(std::is_move_constructible_v<SafeVector<int>>, "该可移动");
    check(true, "编译期断言：不可拷贝、可移动");

    // 完美转发验证：emplace_back 直接用参数原位构造
    SafeVector<std::pair<int, std::string>> pairs;
    pairs.emplace_back(1, "one");
    check(pairs[0].second == "one", "emplace_back 完美转发多参数");

    // 迭代器齐全 -> 标准算法直接可用
    SafeVector<int> nums{5, 3, 1, 4};
    std::sort(nums.begin(), nums.end());
    check(nums[0] == 1 && nums[3] == 5, "std::sort 直接作用于 SafeVector");
}

// =============================================================================
// 第 2 题：join —— 用折叠表达式拼字符串
//
// 【本题考什么】
//   - 变参模板 + 折叠表达式（C++17）
//   - sizeof...(Ts) 处理「最后一个不加分隔符」
//   - 完美转发
//
// 【常见错法】
//   ✗ 用递归展开（能做，但代码长，编译慢）
//   ✗ 每个元素后面都加分隔符，最后再 pop 掉 —— 空参数包时会崩
//   ✗ 用 std::string 累加 -> O(n²) 拷贝；用 ostringstream 更合适
// =============================================================================

// 写法 A：折叠 + 索引判断（最直观）
template <typename... Ts>
std::string join(const char* sep, Ts&&... args) {
    std::ostringstream oss;
    std::size_t index = 0;
    constexpr std::size_t count = sizeof...(Ts);

    // 逗号折叠：对每个参数执行一次 lambda 体
    // 注意 ((...), ...) 的括号，少一层就变成别的意思
    (
        (oss << std::forward<Ts>(args), (++index < count ? oss << sep : oss)),
        ...
    );
    return oss.str();
}

// 写法 B：把分隔符加在「除第一个之外」的每个元素前面（无需计数）
template <typename First, typename... Rest>
std::string join2(const char* sep, First&& first, Rest&&... rest) {
    std::ostringstream oss;
    oss << std::forward<First>(first);
    ((oss << sep << std::forward<Rest>(rest)), ...);   // 空包时整个折叠为空
    return oss.str();
}
inline std::string join2(const char*) { return {}; }   // 零参数重载

void solve02() {
    demo::section("第 2 题  join（折叠表达式）");

    check(join(", ", 1, 2, 3) == "1, 2, 3", R"(join(", ", 1, 2, 3) == "1, 2, 3")");
    check(join("-", "a", "b") == "a-b", R"(join("-", "a", "b") == "a-b")");
    check(join(", ", 42) == "42", "单个参数不加分隔符");
    check(join(", ") == "", "零个参数返回空串");

    // 混合类型：任何支持 operator<< 的都行
    std::string s = join(" | ", 1, 2.5, "text", 'c', true);
    std::cout << "      混合类型: " << s << "\n";
    check(s == "1 | 2.5 | text | c | 1", "混合类型拼接（bool 输出为 1）");

    check(join2(", ", 1, 2, 3) == "1, 2, 3", "写法 B 结果一致");
    check(join2(", ") == "", "写法 B 零参数");

    std::cout <<
        "      两种写法的取舍：\n"
        "        A 用 sizeof...(Ts) 计数，一个函数搞定所有情况\n"
        "        B 靠「首元素单独处理」，逻辑更简单但要多写一个零参数重载\n";
}

// =============================================================================
// 第 3 题：用 variant 实现 JSON 值类型
//
// 【本题考什么】
//   - **递归类型**怎么定义。variant 的模板参数里不能直接出现自己
//     （那会导致无限大的类型），必须通过一层「不完整类型可用」的间接。
//   - std::visit + overloaded 惯用法
//   - 递归遍历时的转义处理
//
// 【常见错法】
//   ✗ using Json = std::variant<..., std::vector<Json>, ...>;
//     直接这么写编译不过 —— Json 在定义自己的时候还不完整。
//     解法：先声明 struct Json;（此时是不完整类型），
//           std::vector<Json> 和 std::map<K, Json> 允许元素是不完整类型
//           （C++17 起标准明确保证这两个可以），
//           然后把 variant 塞进 Json 的成员里。
//   ✗ 忘记转义字符串里的引号和反斜杠 -> 生成的 JSON 非法
// =============================================================================
struct Json;   // 前置声明：此时是不完整类型

using JsonValue = std::variant<
    std::nullptr_t,                       // null
    bool,                                 // true / false
    double,                               // number
    std::string,                          // string
    std::vector<Json>,                    // array   —— 元素是不完整类型，合法
    std::map<std::string, Json>           // object  —— 同上
>;

struct Json {
    JsonValue v;

    // 一堆便利构造函数，让写测试数据舒服些
    Json()                                  : v(nullptr) {}
    Json(std::nullptr_t)                    : v(nullptr) {}
    Json(bool b)                            : v(b) {}
    Json(double d)                          : v(d) {}
    Json(int i)                             : v(static_cast<double>(i)) {}
    Json(const char* s)                     : v(std::string(s)) {}
    Json(std::string s)                     : v(std::move(s)) {}
    Json(std::vector<Json> a)               : v(std::move(a)) {}
    Json(std::map<std::string, Json> o)     : v(std::move(o)) {}
};

// overloaded 惯用法：把多个 lambda 合成一个多重载的可调用对象
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// JSON 字符串转义：必须处理 " \ 和控制字符，否则输出的不是合法 JSON
std::string escapeJson(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::ostringstream ss;
                    ss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(c);
                    out += ss.str();
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// 数字格式化：JSON 不接受 "1.000000"，整数值要输出成整数形态
std::string formatNumber(double d) {
    std::ostringstream ss;
    if (d == static_cast<double>(static_cast<long long>(d))) {
        ss << static_cast<long long>(d);          // 3.0 -> "3"
    } else {
        ss << std::setprecision(17) << d;         // 保证往返精度
    }
    return ss.str();
}

std::string to_string(const Json& j) {
    return std::visit(overloaded{
        [](std::nullptr_t)       { return std::string("null"); },
        [](bool b)               { return std::string(b ? "true" : "false"); },
        [](double d)             { return formatNumber(d); },
        [](const std::string& s) { return "\"" + escapeJson(s) + "\""; },
        [](const std::vector<Json>& arr) {
            std::string out = "[";
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (i) out += ",";
                out += to_string(arr[i]);          // 递归
            }
            return out + "]";
        },
        [](const std::map<std::string, Json>& obj) {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, val] : obj) {
                if (!first) out += ",";
                first = false;
                out += "\"" + escapeJson(k) + "\":" + to_string(val);
            }
            return out + "}";
        }
    }, j.v);
}

void solve03() {
    demo::section("第 3 题  variant 实现 JSON");

    check(to_string(Json(nullptr)) == "null", "null");
    check(to_string(Json(true)) == "true", "bool");
    check(to_string(Json(3.0)) == "3", "整数值的 double 输出为 3 而非 3.000000");
    check(to_string(Json(2.5)) == "2.5", "小数");
    check(to_string(Json("hi")) == "\"hi\"", "字符串加引号");
    check(to_string(Json("a\"b\\c\nd")) == "\"a\\\"b\\\\c\\nd\"", "转义 \" \\ 和换行");

    std::vector<Json> arr{1, 2.5, "three", true, nullptr};
    check(to_string(Json(arr)) == "[1,2.5,\"three\",true,null]", "数组");

    std::map<std::string, Json> obj{
        {"name", "Alice"},
        {"age",  30},
        {"tags", std::vector<Json>{"a", "b"}},
        {"addr", std::map<std::string, Json>{{"city", "Beijing"}}}
    };
    std::string out = to_string(Json(obj));
    std::cout << "      嵌套对象: " << out << "\n";
    // map 有序，所以键按字典序：addr, age, name, tags
    check(out == R"({"addr":{"city":"Beijing"},"age":30,"name":"Alice","tags":["a","b"]})",
          "嵌套对象 + 数组（map 保证键有序）");

    std::cout <<
        "      关键点：variant 里不能直接放自己，要靠 struct Json 包一层。\n"
        "              vector<Json> 与 map<K,Json> 允许不完整类型（C++17 起明确保证），\n"
        "              这是递归数据结构在 variant 里落地的标准做法。\n";
}

// =============================================================================
// 第 4 题：满足 std::sortable 约束的快排
//
// 【本题考什么】
//   - C++20 concepts 怎么约束迭代器算法
//   - 快排的三个工程细节：三点取中、小区间切插排、只递归较小的一半
//
// 【常见错法】
//   ✗ 直接取首元素当 pivot -> 有序输入退化成 O(n²)（第 15 章 10 讲过）
//   ✗ 两侧都递归 -> 最坏情况栈深度 O(n)，大数组直接栈溢出
//   ✗ 不加 concept 约束 -> 传 list 迭代器时报几百行模板错误
// =============================================================================

// std::sortable<I, Comp> 要求：I 可排列（permutable）且用 Comp 可比较
template <std::random_access_iterator It,
          typename Comp = std::ranges::less>
    requires std::sortable<It, Comp>
void my_quick_sort(It first, It last, Comp comp = {}) {
    constexpr std::ptrdiff_t kInsertionThreshold = 16;

    while (last - first > kInsertionThreshold) {
        // 三点取中：让有序/逆序输入也能得到好的 pivot
        It mid = first + (last - first) / 2;
        It a = first, b = mid, c = last - 1;
        if (comp(*b, *a)) std::iter_swap(a, b);
        if (comp(*c, *b)) {
            std::iter_swap(b, c);
            if (comp(*b, *a)) std::iter_swap(a, b);
        }
        auto pivot = *b;      // 取值而不是迭代器：分区过程中元素会被搬走

        // Hoare 分区
        It i = first, j = last - 1;
        while (i <= j) {
            while (comp(*i, pivot)) ++i;
            while (comp(pivot, *j)) --j;
            if (i <= j) { std::iter_swap(i, j); ++i; --j; }
        }

        // 只递归较小的一半，较大的一半用循环处理
        // -> 栈深度稳定在 O(log n)，而不是最坏 O(n)
        if ((j + 1) - first < last - i) {
            my_quick_sort(first, j + 1, comp);
            first = i;
        } else {
            my_quick_sort(i, last, comp);
            last = j + 1;
        }
    }

    // 小区间用插入排序：元素少时它比快排快，且对「几乎有序」极快
    for (It it = first + (first == last ? 0 : 1); it != last; ++it) {
        auto val = *it;
        It   pos = it;
        while (pos != first && comp(val, *(pos - 1))) {
            *pos = *(pos - 1);
            --pos;
        }
        *pos = val;
    }
}

void solve04() {
    demo::section("第 4 题  概念约束的快排");

    auto verify = [](std::vector<int> v, const char* tag) {
        auto expect = v;
        std::sort(expect.begin(), expect.end());
        my_quick_sort(v.begin(), v.end());
        check(v == expect, tag);
    };

    verify({}, "空数组");
    verify({1}, "单元素");
    verify({2, 1}, "两个元素");
    verify({5, 3, 8, 1, 9, 2, 7}, "随机小数组");
    verify(std::vector<int>(50, 7), "全部相同（考验分区不死循环）");

    // 三种最坏输入：有序、逆序、大量重复
    std::vector<int> asc(3000), desc(3000), dup(3000);
    for (int i = 0; i < 3000; ++i) { asc[i] = i; desc[i] = 3000 - i; dup[i] = i % 7; }
    verify(asc,  "已排序 3000 个（快排经典最坏输入）");
    verify(desc, "完全逆序 3000 个");
    verify(dup,  "大量重复值 3000 个");

    // 自定义比较器
    std::vector<int> v{5, 3, 8, 1};
    my_quick_sort(v.begin(), v.end(), std::ranges::greater{});
    check(std::is_sorted(v.begin(), v.end(), std::greater<>{}), "降序比较器");

    // 非算术类型
    std::vector<std::string> ss{"pear", "apple", "cherry"};
    my_quick_sort(ss.begin(), ss.end());
    check(ss[0] == "apple" && ss[2] == "pear", "字符串排序");

    std::cout <<
        "      concept 的价值：传 std::list 的迭代器进去，\n"
        "      报的是「不满足 random_access_iterator」一行人话，\n"
        "      而不是几百行模板实例化错误。\n";
}

// =============================================================================
// 第 5 题：O(1) LRU 缓存
//
// 【本题考什么】
//   - list + unordered_map 的组合（这是标准解法）
//   - 为什么用 std::list 而不是 vector：splice 是 O(1) 且**迭代器不失效**
//     （第 15 章 6 讲的 list 唯一杀手级优势，这题就是它的正当用途）
//
// 【常见错法】
//   ✗ 用 vector 存访问顺序 -> 移到队首是 O(n)
//   ✗ map 里存下标而不是迭代器 -> 链表变动后下标全错
//   ✗ 淘汰时忘了从 map 里也删掉 -> map 无限增长（内存泄漏）
//   ✗ get 命中后忘记更新顺序 -> 退化成 FIFO，不是 LRU
// =============================================================================
template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : cap_(capacity) {
        if (capacity == 0) throw std::invalid_argument("LRU 容量不能为 0");
    }

    // O(1)：哈希查找 + splice 移到队首
    std::optional<V> get(const K& key) {
        auto it = index_.find(key);
        if (it == index_.end()) { ++misses_; return std::nullopt; }
        ++hits_;
        // splice 只改指针，O(1)，且 it->second 这个迭代器**依然有效**
        order_.splice(order_.begin(), order_, it->second);
        return it->second->second;
    }

    // O(1)
    void put(const K& key, V value) {
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = std::move(value);                 // 更新值
            order_.splice(order_.begin(), order_, it->second);     // 提到队首
            return;
        }
        if (order_.size() >= cap_) evictOldest();
        order_.emplace_front(key, std::move(value));
        index_[key] = order_.begin();
    }

    bool contains(const K& key) const { return index_.count(key) != 0; }

    std::size_t size() const noexcept { return order_.size(); }
    std::size_t capacity() const noexcept { return cap_; }
    long long   hits() const noexcept { return hits_; }
    long long   misses() const noexcept { return misses_; }

    // 从最近到最久，用于测试
    std::vector<K> keysMruToLru() const {
        std::vector<K> out;
        out.reserve(order_.size());
        for (const auto& [k, v] : order_) out.push_back(k);
        return out;
    }

private:
    void evictOldest() {
        const K& oldest = order_.back().first;
        index_.erase(oldest);      // 关键：map 和 list 必须同步删
        order_.pop_back();
    }

    using Entry = std::pair<K, V>;
    std::size_t                                       cap_;
    std::list<Entry>                                  order_;   // 队首 = 最近使用
    std::unordered_map<K, typename std::list<Entry>::iterator> index_;
    long long hits_ = 0, misses_ = 0;
};

void solve05() {
    demo::section("第 5 题  O(1) LRU 缓存");

    LruCache<std::string, int> c(3);
    c.put("a", 1); c.put("b", 2); c.put("c", 3);
    check(c.size() == 3, "装满 3 个");

    // 访问 a，让它变成最近使用
    check(c.get("a").value_or(-1) == 1, "get(\"a\") 命中");
    check((c.keysMruToLru() == std::vector<std::string>{"a", "c", "b"}),
          "命中后 a 被提到队首");

    // 插入 d，应该淘汰最久未用的 b
    c.put("d", 4);
    check(!c.contains("b"), "插入 d 后 b 被淘汰（最久未用）");
    check(c.contains("a") && c.contains("c") && c.contains("d"), "其余三个都在");
    check(c.size() == 3, "容量恒定");

    // 更新已存在的键：不应该增加大小，且要提到队首
    c.put("c", 30);
    check(c.get("c").value_or(-1) == 30, "更新已有键的值");
    check(c.size() == 3, "更新不增加大小");
    check(c.keysMruToLru().front() == "c", "更新后 c 在队首");

    check(!c.get("nope").has_value(), "未命中返回 nullopt");
    std::cout << "      命中 " << c.hits() << " 次，未命中 " << c.misses() << " 次\n";

    // 压测：验证是真 O(1) 而不是隐藏的 O(n)
    LruCache<int, int> big(1000);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200000; ++i) {
        big.put(i, i);
        if (i % 3 == 0) (void)big.get(i / 2);
    }
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    check(big.size() == 1000, "20 万次操作后容量仍是 1000");
    std::cout << "      20 万次 put/get 耗时 " << us << " us"
              << "（容量恒定说明淘汰正常，耗时线性说明单次是 O(1)）\n";

    std::cout <<
        "      为什么必须用 std::list：\n"
        "        splice 是 O(1) 且**不使迭代器失效**，所以 map 里存的\n"
        "        迭代器在无数次重排后依然有效。换成 vector 或 deque 都做不到。\n"
        "        这是全书唯一一个「list 明显优于 vector」的场景。\n";
}

// =============================================================================
// 第 6 题：线程安全的事件总线
//
// 要求：subscribe<Event>(handler) / publish(Event{...})，
//       订阅者用 weak_ptr 持有，失效自动清理。
//
// 【本题考什么】
//   - 用 type_index 做「按类型分发」的运行时映射
//   - weak_ptr 观察者模式（第 13 章 4.2、第 14 章 5.2）
//   - 「不要在持锁时调用外部代码」（第 13 章 5.3、第 16 章 10）
//
// 【常见错法】
//   ✗ 持锁遍历并调用 handler -> handler 里再 subscribe/publish 就死锁
//   ✗ 用 shared_ptr 持有订阅者 -> 订阅者永远不释放（内存泄漏）
//   ✗ 只在 publish 时清理失效订阅 -> 从不 publish 的事件类型会无限累积
//   ✗ 用 typeid(E).name() 当 key -> 不同编译器格式不同，且可能重名；
//     应该用 std::type_index（它包装了 type_info 且可哈希、可比较）
// =============================================================================
class EventBus {
public:
    // 订阅句柄：析构时自动退订（RAII）。也可以 reset() 手动退订。
    class Subscription {
    public:
        Subscription() = default;
        Subscription(EventBus* bus, std::type_index type, std::uint64_t id)
            : bus_(bus), type_(type), id_(id) {}
        ~Subscription() { reset(); }

        Subscription(const Subscription&)            = delete;
        Subscription& operator=(const Subscription&) = delete;
        Subscription(Subscription&& o) noexcept
            : bus_(o.bus_), type_(o.type_), id_(o.id_) { o.bus_ = nullptr; }
        Subscription& operator=(Subscription&& o) noexcept {
            if (this != &o) { reset(); bus_ = o.bus_; type_ = o.type_; id_ = o.id_;
                              o.bus_ = nullptr; }
            return *this;
        }

        void reset() {
            if (bus_) { bus_->unsubscribe(type_, id_); bus_ = nullptr; }
        }
        bool valid() const noexcept { return bus_ != nullptr; }

    private:
        EventBus*       bus_  = nullptr;
        std::type_index type_ = std::type_index(typeid(void));
        std::uint64_t   id_   = 0;
    };

    // 方式 1：订阅者是一个 shared_ptr 管理的对象。
    //   总线只持有 weak_ptr —— 对象销毁后该订阅自动失效并被清理。
    template <typename Event, typename Owner>
    Subscription subscribe(std::shared_ptr<Owner> owner,
                           void (Owner::*method)(const Event&)) {
        auto weak = std::weak_ptr<Owner>(owner);
        return subscribeImpl<Event>(
            [weak, method](const Event& e) -> bool {
                if (auto self = weak.lock()) {      // 提升成功 = 对象还活着
                    ((*self).*method)(e);
                    return true;                     // 保留这个订阅
                }
                return false;                        // 对象已销毁 -> 请求清理
            });
    }

    // 方式 2：订阅者是一个自由函数 / lambda，生命周期由 Subscription 句柄管理
    template <typename Event, typename F>
    Subscription subscribe(F&& handler) {
        return subscribeImpl<Event>(
            [h = std::forward<F>(handler)](const Event& e) -> bool {
                h(e);
                return true;
            });
    }

    template <typename Event>
    std::size_t publish(const Event& event) {
        std::type_index type(typeid(Event));

        // 第 1 步：持锁拷贝一份订阅者列表，然后立刻解锁。
        //   绝不能持锁调用 handler —— handler 里可能再 subscribe/publish。
        std::vector<std::pair<std::uint64_t, HandlerFn>> snapshot;
        {
            std::shared_lock lk(mutex_);
            auto it = handlers_.find(type);
            if (it == handlers_.end()) return 0;
            snapshot = it->second;
        }

        // 第 2 步：不持锁地调用，收集失效的 id
        std::vector<std::uint64_t> dead;
        std::size_t delivered = 0;
        for (const auto& [id, fn] : snapshot) {
            // fn 内部会把 void* 还原成 const Event*，
            // 类型正确性由 handlers_ 的 type_index 键保证
            if (fn(&event)) ++delivered;
            else            dead.push_back(id);
        }

        // 第 3 步：回收失效订阅
        if (!dead.empty()) {
            std::unique_lock lk(mutex_);
            auto it = handlers_.find(type);
            if (it != handlers_.end()) {
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                              [&dead](const auto& p) {
                                  return std::find(dead.begin(), dead.end(), p.first)
                                         != dead.end();
                              }),
                          vec.end());
                if (vec.empty()) handlers_.erase(it);
            }
        }
        return delivered;
    }

    std::size_t subscriberCount(std::type_index type) const {
        std::shared_lock lk(mutex_);
        auto it = handlers_.find(type);
        return it == handlers_.end() ? 0 : it->second.size();
    }
    template <typename Event>
    std::size_t subscriberCount() const { return subscriberCount(std::type_index(typeid(Event))); }

private:
    // 擦除后的处理器：返回 false 表示「我失效了，请清理我」
    using HandlerFn = std::function<bool(const void*)>;

    template <typename Event, typename F>
    Subscription subscribeImpl(F&& typedHandler) {
        std::type_index type(typeid(Event));
        std::uint64_t   id = nextId_.fetch_add(1);

        // 把「强类型 handler」包装成「void* handler」，类型信息由 type_index 保证
        HandlerFn erased = [h = std::forward<F>(typedHandler)](const void* p) -> bool {
            return h(*static_cast<const Event*>(p));
        };

        std::unique_lock lk(mutex_);
        handlers_[type].emplace_back(id, std::move(erased));
        return Subscription(this, type, id);
    }

    void unsubscribe(std::type_index type, std::uint64_t id) {
        std::unique_lock lk(mutex_);
        auto it = handlers_.find(type);
        if (it == handlers_.end()) return;
        auto& vec = it->second;
        vec.erase(std::remove_if(vec.begin(), vec.end(),
                      [id](const auto& p) { return p.first == id; }),
                  vec.end());
        if (vec.empty()) handlers_.erase(it);
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::type_index,
                       std::vector<std::pair<std::uint64_t, HandlerFn>>> handlers_;
    std::atomic<std::uint64_t> nextId_{1};
};

// 测试用的事件类型
struct OrderCreated { int id; double amount; };
struct UserLoggedIn { std::string name; };

class OrderService {
public:
    std::vector<int> seen;
    void onOrder(const OrderCreated& e) { seen.push_back(e.id); }
};

void solve06() {
    demo::section("第 6 题  线程安全事件总线");

    EventBus bus;

    // lambda 订阅，生命周期由 Subscription 管
    int lambdaCount = 0;
    double totalAmount = 0;
    auto sub1 = bus.subscribe<OrderCreated>([&](const OrderCreated& e) {
        ++lambdaCount;
        totalAmount += e.amount;
    });

    // 成员函数订阅，总线只持 weak_ptr
    auto service = std::make_shared<OrderService>();
    auto sub2 = bus.subscribe<OrderCreated>(service, &OrderService::onOrder);

    check(bus.subscriberCount<OrderCreated>() == 2, "两个订阅者");

    std::size_t n = bus.publish(OrderCreated{101, 99.5});
    check(n == 2, "publish 投递给 2 个订阅者");
    check(lambdaCount == 1, "lambda 订阅者收到");
    check(service->seen == std::vector<int>{101}, "成员函数订阅者收到");

    // 不同事件类型互不干扰
    int loginCount = 0;
    auto sub3 = bus.subscribe<UserLoggedIn>([&](const UserLoggedIn&) { ++loginCount; });
    bus.publish(UserLoggedIn{"alice"});
    check(loginCount == 1 && lambdaCount == 1, "按类型分发，互不干扰");

    // 订阅者对象销毁 -> weak_ptr 失效 -> 下次 publish 自动清理
    service.reset();
    std::size_t n2 = bus.publish(OrderCreated{102, 10.0});
    check(n2 == 1, "对象销毁后只投递给剩下的 1 个");
    check(bus.subscriberCount<OrderCreated>() == 1, "失效订阅已被自动清理");

    // Subscription 析构自动退订
    { auto tmp = bus.subscribe<OrderCreated>([](const OrderCreated&) {});
      check(bus.subscriberCount<OrderCreated>() == 2, "作用域内有 2 个"); }
    check(bus.subscriberCount<OrderCreated>() == 1, "离开作用域自动退订");

    // handler 里再 publish —— 验证不会死锁
    bool nested = false;
    auto sub4 = bus.subscribe<UserLoggedIn>([&](const UserLoggedIn&) {
        bus.publish(OrderCreated{999, 1.0});      // 持锁的话这里就死锁了
        nested = true;
    });
    bus.publish(UserLoggedIn{"bob"});
    check(nested, "handler 内部再 publish 不死锁（因为调用时不持锁）");

    // 多线程并发订阅 + 发布
    {
        EventBus mtBus;
        std::atomic<int> received{0};
        std::vector<EventBus::Subscription> subs;
        std::mutex subsMutex;

        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&] {
                for (int i = 0; i < 200; ++i) {
                    auto s = mtBus.subscribe<OrderCreated>(
                        [&](const OrderCreated&) { ++received; });
                    { std::lock_guard lk(subsMutex); subs.push_back(std::move(s)); }
                    mtBus.publish(OrderCreated{i, 1.0});
                }
            });
        }
        for (auto& th : threads) th.join();
        check(received.load() > 0, "4 线程并发订阅+发布，无崩溃无死锁（收到 "
                                       + std::to_string(received.load()) + " 次）");
    }

    std::cout <<
        "      三个设计要点：\n"
        "        1. std::type_index 做 key（可哈希、可比较，跨编译器稳定）\n"
        "        2. 持锁只拷贝列表，调用 handler 时**不持锁** -> 可重入不死锁\n"
        "        3. handler 返回 bool 表示自己是否还有效 -> weak_ptr 失效自动回收\n";
}

// =============================================================================
// 第 7 题：类型擦除实现 AnyCallable（不用 std::function）
//
// 【本题考什么】
//   - 类型擦除的标准结构：抽象 Concept + 模板 Model + 持有基类指针
//   - 为什么需要 clone()：要支持拷贝，就必须让每个 Model 知道怎么复制自己
//   - 小对象优化（SBO）的思路
//
// 【常见错法】
//   ✗ 忘了虚析构 -> 通过基类指针删除派生 Model 时资源泄漏（第 13 章 3.1）
//   ✗ 拷贝构造直接拷贝 unique_ptr -> 编译错误；必须走 clone()
//   ✗ operator() 忘记加 const，或者转发时忘了 std::forward
// =============================================================================
template <typename Signature>
class AnyCallable;

template <typename R, typename... Args>
class AnyCallable<R(Args...)> {
    // ---- 抽象接口：擦除掉具体类型后，所有可调用对象长这样 ----
    struct Concept {
        virtual ~Concept() = default;                              // 必须 virtual
        virtual R                        invoke(Args&&...) const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;        // 支持拷贝的关键
        virtual const std::type_info&    target_type() const noexcept = 0;
    };

    // ---- 具体实现：每个 F 一份 ----
    template <typename F>
    struct Model final : Concept {
        F fn;
        explicit Model(F f) : fn(std::move(f)) {}

        R invoke(Args&&... args) const override {
            // 注意：这里是 const 成员函数，所以要求 F 的 operator() 也是 const
            // （lambda 默认就是 const，除非标了 mutable）
            if constexpr (std::is_void_v<R>) {
                fn(std::forward<Args>(args)...);
            } else {
                return fn(std::forward<Args>(args)...);
            }
        }
        std::unique_ptr<Concept> clone() const override {
            return std::make_unique<Model>(fn);
        }
        const std::type_info& target_type() const noexcept override { return typeid(F); }
    };

    std::unique_ptr<Concept> impl_;

public:
    AnyCallable() = default;
    AnyCallable(std::nullptr_t) {}

    // 接受任意「用 Args... 调用后能得到 R」的可调用对象。
    //
    // 这里必须用 std::decay_t 而不是 std::remove_cvref_t：
    //   传自由函数 freeFunctionDouble 时，F 推导为 int(&)(int)，
    //   remove_cvref_t 得到的是**函数类型** int(int) —— 而类的成员
    //   不能是函数类型（MSVC 报 C2207）。
    //   decay_t 会把函数衰减成函数指针 int(*)(int)，数组衰减成指针，
    //   这正是「按值存一份」需要的语义。
    template <typename F>
        requires (!std::is_same_v<std::decay_t<F>, AnyCallable>) &&
                 std::is_invocable_r_v<R, std::decay_t<F>&, Args...>
    AnyCallable(F&& f)
        : impl_(std::make_unique<Model<std::decay_t<F>>>(std::forward<F>(f))) {}

    // 拷贝：靠 clone() 深拷贝底层对象
    AnyCallable(const AnyCallable& o) : impl_(o.impl_ ? o.impl_->clone() : nullptr) {}
    AnyCallable& operator=(const AnyCallable& o) {
        if (this != &o) impl_ = o.impl_ ? o.impl_->clone() : nullptr;
        return *this;
    }

    AnyCallable(AnyCallable&&) noexcept            = default;
    AnyCallable& operator=(AnyCallable&&) noexcept = default;

    R operator()(Args... args) const {
        if (!impl_) throw std::bad_function_call();
        return impl_->invoke(std::forward<Args>(args)...);
    }

    explicit operator bool() const noexcept { return impl_ != nullptr; }

    const std::type_info& target_type() const noexcept {
        return impl_ ? impl_->target_type() : typeid(void);
    }
};

int freeFunctionDouble(int x) { return x * 2; }

struct StatefulAdder {
    int base;
    int operator()(int x) const { return base + x; }
};

void solve07() {
    demo::section("第 7 题  类型擦除的 AnyCallable");

    // 装 lambda
    AnyCallable<int(int)> f1 = [](int x) { return x + 1; };
    check(f1(41) == 42, "装 lambda");

    // 装自由函数
    AnyCallable<int(int)> f2 = freeFunctionDouble;
    check(f2(21) == 42, "装自由函数");

    // 装带状态的函数对象
    AnyCallable<int(int)> f3 = StatefulAdder{100};
    check(f3(5) == 105, "装带状态的仿函数");

    // 装捕获了东西的 lambda
    int captured = 7;
    AnyCallable<int(int)> f4 = [captured](int x) { return x * captured; };
    check(f4(6) == 42, "装值捕获的 lambda");

    // 拷贝：深拷贝，两份独立
    AnyCallable<int(int)> f5 = f3;
    check(f5(5) == 105, "拷贝构造后仍可调用");
    AnyCallable<int(int)> f6;
    f6 = f1;
    check(f6(41) == 42, "拷贝赋值");

    // 移动
    AnyCallable<int(int)> f7 = std::move(f2);
    check(f7(21) == 42, "移动构造");
    check(!static_cast<bool>(f2), "移动后源为空");

    // void 返回类型
    int sideEffect = 0;
    AnyCallable<void(int)> f8 = [&](int x) { sideEffect = x; };
    f8(99);
    check(sideEffect == 99, "void 返回类型");

    // 多参数
    AnyCallable<std::string(const std::string&, int)> f9 =
        [](const std::string& s, int n) {
            std::string out;
            for (int i = 0; i < n; ++i) out += s;
            return out;
        };
    check(f9("ab", 3) == "ababab", "多参数 + 引用参数");

    // 空对象调用抛异常
    bool caught = false;
    try { AnyCallable<int(int)> empty; (void)empty(1); }
    catch (const std::bad_function_call&) { caught = true; }
    check(caught, "空对象调用抛 std::bad_function_call");

    // target_type 能拿回原始类型信息
    check(f3.target_type() == typeid(StatefulAdder), "target_type 保留原始类型");

    std::cout <<
        "      与 std::function 的差异：\n"
        "        本实现每次都堆分配；标准库有小对象优化（SBO），\n"
        "        小的可调用对象直接存在对象内部，不分配堆内存。\n"
        "        想加 SBO：在类里放一个 alignas 的 char buffer，\n"
        "        sizeof(F) 足够小就 placement new 进去，否则才走堆。\n";
}

// =============================================================================
// 第 8 题：给 HTTP Handler 套装饰器
//
// 【本题考什么】
//   - 装饰器模式的函数式写法（返回同类型的包装函数，可无限叠加）
//   - 这与第 16 章 HTTP 中间件是同一个思路
//
// 【常见错法】
//   ✗ 用继承做装饰 -> 每加一种装饰就要一个类，且无法动态组合
//   ✗ 装饰器改变了函数签名 -> 无法叠加
//   ✗ 计时用 system_clock -> 它可能被 NTP 调整；测耗时必须用 steady_clock
// =============================================================================

// 简化的 HTTP 类型（真实版本见 ch12 / ch16）
struct HttpRequest  { std::string method, path, body; };
struct HttpResponse { int status = 200; std::string body; std::map<std::string, std::string> headers; };

using Handler = std::function<HttpResponse(const HttpRequest&)>;

// 装饰器的签名：Handler -> Handler。这保证了可以无限叠加。
using Decorator = std::function<Handler(Handler)>;

// 装饰器 1：记录耗时
Decorator withTiming(std::vector<std::string>* log) {
    return [log](Handler next) -> Handler {
        return [next, log](const HttpRequest& req) -> HttpResponse {
            auto t0 = std::chrono::steady_clock::now();     // 测耗时必须用 steady_clock
            HttpResponse resp = next(req);
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            resp.headers["X-Elapsed-Us"] = std::to_string(us);
            if (log) log->push_back(req.method + " " + req.path + " -> "
                                    + std::to_string(resp.status));
            return resp;
        };
    };
}

// 装饰器 2：捕获异常转 500
Decorator withErrorHandling() {
    return [](Handler next) -> Handler {
        return [next](const HttpRequest& req) -> HttpResponse {
            try {
                return next(req);
            } catch (const std::exception& e) {
                return HttpResponse{500, std::string("Internal Error: ") + e.what(), {}};
            }
        };
    };
}

// 装饰器 3：简易限流（每个路径最多 N 次）
Decorator withRateLimit(int maxCalls) {
    // 计数器要在装饰器实例之间共享，所以用 shared_ptr 捕获
    auto counters = std::make_shared<std::map<std::string, int>>();
    return [counters, maxCalls](Handler next) -> Handler {
        return [counters, maxCalls, next](const HttpRequest& req) -> HttpResponse {
            if (++(*counters)[req.path] > maxCalls) {
                return HttpResponse{429, "Too Many Requests", {}};
            }
            return next(req);
        };
    };
}

// 把多个装饰器套到一个 handler 上。
// 从后往前套，使得写法上「最先列出的装饰器在最外层」—— 与直觉一致。
Handler decorate(Handler h, std::initializer_list<Decorator> decorators) {
    std::vector<Decorator> ds(decorators);
    for (auto it = ds.rbegin(); it != ds.rend(); ++it) {
        h = (*it)(h);
    }
    return h;
}

void solve08() {
    demo::section("第 8 题  Handler 装饰器");

    std::vector<std::string> accessLog;

    Handler helloHandler = [](const HttpRequest& req) -> HttpResponse {
        return HttpResponse{200, "Hello, " + req.path, {}};
    };
    Handler brokenHandler = [](const HttpRequest&) -> HttpResponse {
        throw std::runtime_error("数据库连不上");
    };

    // 单个装饰器
    Handler timed = withTiming(&accessLog)(helloHandler);
    HttpResponse r1 = timed(HttpRequest{"GET", "/world", ""});
    check(r1.status == 200 && r1.body == "Hello, /world", "装饰后功能不变");
    check(r1.headers.count("X-Elapsed-Us") == 1, "装饰器加上了耗时头");
    check(accessLog.size() == 1, "访问日志被记录");
    std::cout << "      日志: " << accessLog[0]
              << "  耗时 " << r1.headers["X-Elapsed-Us"] << " us\n";

    // 叠加：异常处理在外，计时在内
    Handler safe = decorate(brokenHandler, {withErrorHandling(), withTiming(&accessLog)});
    HttpResponse r2 = safe(HttpRequest{"POST", "/boom", ""});
    check(r2.status == 500, "异常被装饰器转成 500");
    std::cout << "      500 响应体: " << r2.body << "\n";

    // 三层叠加 + 限流
    Handler full = decorate(helloHandler,
                            {withErrorHandling(), withRateLimit(2), withTiming(&accessLog)});
    check(full(HttpRequest{"GET", "/limited", ""}).status == 200, "限流第 1 次通过");
    check(full(HttpRequest{"GET", "/limited", ""}).status == 200, "限流第 2 次通过");
    check(full(HttpRequest{"GET", "/limited", ""}).status == 429, "限流第 3 次被拒（429）");
    check(full(HttpRequest{"GET", "/other", ""}).status == 200, "其它路径不受影响");

    std::cout <<
        "      顺序很关键：\n"
        "        {withErrorHandling, withTiming} -> 异常在外层，能捕获计时器内的异常，\n"
        "                                            但计时不包含异常处理本身\n"
        "        {withTiming, withErrorHandling} -> 计时在外层，能测到异常处理的耗时，\n"
        "                                            但异常若从计时器抛出就没人管了\n"
        "      一般把「异常处理」和「日志」放最外层，「鉴权」「限流」放中层，业务在最内。\n";
}

} // namespace

// =============================================================================
int main() {
    demo::title("第 18 章  练习题参考答案（B.1 语法与 STL、B.2 设计模式）");
    std::cout <<
        "  每题都标注了【本题考什么】和【常见错法】，\n"
        "  源码里的注释比这里的输出详细，建议对照 src/ch18_solutions/main.cpp 阅读。\n";

    demo::title("B.1  语法与 STL");
    solve01();
    solve02();
    solve03();
    solve04();
    solve05();

    demo::title("B.2  设计模式");
    solve06();
    solve07();
    solve08();

    demo::title("小结");
    if (g_failures == 0) {
        std::cout << "  全部自检通过。\n";
    } else {
        std::cout << "  有 " << g_failures << " 项失败。\n";
    }
    std::cout <<
        "\n  这 8 题串起来的知识点：\n"
        "    第 1 题  组合优于继承（标准容器析构非 virtual）+ 完美转发 + 五法则\n"
        "    第 2 题  折叠表达式的四种形式，以及空参数包的边界处理\n"
        "    第 3 题  variant 里放递归类型的标准做法（前置声明 + struct 包一层）\n"
        "    第 4 题  concepts 约束算法；快排的三个工程细节缺一不可\n"
        "    第 5 题  list::splice 的 O(1) 且迭代器不失效 —— list 唯一的正当用途\n"
        "    第 6 题  type_index 分发 + weak_ptr 观察者 + 不持锁调用外部代码\n"
        "    第 7 题  类型擦除三件套：virtual 析构、invoke、clone\n"
        "    第 8 题  装饰器保持签名不变才能无限叠加；顺序决定语义\n"
        "\n  网络部分（第 9~14 题）见第 19 章。\n";

    return g_failures == 0 ? 0 : 1;
}
