# 第 6 章 · 标准库（STL）

▶ 对应程序：`ch06_stl`

## 6.1 容器选型决策树

```
需要按【键】查找吗?
 │
 ├─ 是 ──> 需要【有序遍历 / 范围查询 / 前驱后继】吗?
 │          ├─ 是 ──> std::map / std::set
 │          │          红黑树, O(log n), 迭代器稳定, 遍历有序
 │          └─ 否 ──> std::unordered_map / std::unordered_set
 │                     哈希表, 均摊 O(1), 顺序不确定, rehash 时迭代器失效
 │
 └─ 否 ──> 大小编译期已知?
            ├─ 是 ──> std::array          栈上, 零开销
            └─ 否 ──> 只在【尾部】增删?
                       ├─ 是 ──> std::vector          ← 默认就选它
                       └─ 否 ──> 【两端】都增删?
                                  ├─ 是 ──> std::deque
                                  └─ 否 ──> 需要迭代器永久有效 或 O(1) splice?
                                             ├─ 是 ──> std::list
                                             └─ 否 ──> 还是 vector（实测通常更快）
```

**最重要的一条经验：不确定就用 `vector`。**

链表的理论优势（O(1) 中间插入）在现代 CPU 上常被缓存不命中吃光。
遍历 `vector` 比遍历 `list` 快 5~10 倍是常见现象，因为 vector 内存连续、
CPU 预取器能提前把数据拉进缓存，而链表每跳一个节点就可能是一次缓存未命中（约 100 个时钟周期）。

## 6.2 复杂度与性质速查

| 容器 | 随机访问 | 头插 | 尾插 | 中间插 | 查找 | 内存 | 迭代器失效 |
|------|---------|------|------|--------|------|------|-----------|
| `vector` | O(1) | O(n) | 均摊 O(1) | O(n) | O(n) | 连续 | 扩容时全失效 |
| `deque` | O(1) | O(1) | O(1) | O(n) | O(n) | 分段连续 | 插入时迭代器失效，引用不失效 |
| `list` | ✗ | O(1) | O(1) | O(1) | O(n) | 节点 | 只有被删的失效 |
| `array` | O(1) | ✗ | ✗ | ✗ | O(n) | 栈上连续 | 不失效 |
| `map/set` | ✗ | — | — | O(log n) | O(log n) | 红黑树节点 | 只有被删的失效 |
| `unordered_*` | ✗ | — | — | 均摊 O(1) | 均摊 O(1) | 桶+链 | rehash 时全失效 |

## 6.3 vector 的几个关键点

```cpp
v.reserve(n);          // ★ 预分配，避免反复扩容 —— 最容易拿到的性能提升
v.emplace_back(a, b);  // 原位构造，比 push_back(T(a,b)) 少一次移动
v.shrink_to_fit();     // 释放多余容量（非强制）
```

`ch06_stl` 实测：20 万次 `push_back`，不 reserve vs reserve 差距明显。

**【坑】扩容会让所有迭代器/指针/引用失效**

```cpp
int* p = &v[0];
v.push_back(x);        // 可能重新分配
*p = 1;                // ❌ p 可能已经悬垂
```

**删除元素**

```cpp
std::erase_if(v, pred);                                        // C++20，一行
v.erase(std::remove_if(v.begin(), v.end(), pred), v.end());    // C++17 及以前

// 无序容器的 O(1) 删除技巧（不保序）
std::swap(v[i], v.back());
v.pop_back();
```

**注意 `std::remove_if` 本身不删元素**，它只是把要保留的元素挪到前面，
返回新的逻辑末尾 —— 必须配合 `erase`。这就是 "erase-remove 惯用法"。

## 6.4 map 的坑与技巧

```cpp
// ❌ operator[] 在键不存在时会【插入】一个默认构造的元素
if (m["key"] == 0) { }      // 这一句就把 "key" 插进去了

// ✅ 只读查询
m.contains(key);            // C++20
m.find(key) != m.end();
m.at(key);                  // 不存在抛 out_of_range

// 四种插入的区别
m[k] = v;                   // 不存在则默认构造 value 再赋值（两步）
m.insert({k, v});           // 已存在则什么都不做（但 value 已经构造好了）
m.try_emplace(k, args...);  // 已存在则【不构造 value】 ← 最省
m.insert_or_assign(k, v);   // 存在就覆盖
```

**范围查询是 map 相对 unordered_map 的独门优势**

```cpp
auto lo = m.lower_bound(150);    // 第一个 >= 150
auto hi = m.upper_bound(350);    // 第一个 > 350
for (auto it = lo; it != hi; ++it) { }
```

**自定义类型做 key**

```cpp
// map/set 需要【严格弱序】
struct Cmp { bool operator()(const K& a, const K& b) const { return a.id < b.id; } };
std::set<K, Cmp> s;

// unordered_* 需要 hash + operator==
struct Hash {
    size_t operator()(const K& k) const noexcept {
        size_t h1 = std::hash<std::string>{}(k.name);
        size_t h2 = std::hash<int>{}(k.age);
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));   // hash_combine
    }
};
std::unordered_set<K, Hash> us;
```

## 6.5 算法速查

```cpp
// ── 排序 ──
sort(b, e)                    不稳定, O(n log n)
stable_sort(b, e)             稳定，需要额外内存
partial_sort(b, mid, e)       只保证前 (mid-b) 个有序
nth_element(b, nth, e)        只保证第 n 个就位，左边都不大于它，O(n) ← 求中位数
is_sorted / reverse / rotate / shuffle

// ── 有序区间查找 O(log n) ──   ★ 前提：必须已排序
binary_search(b, e, v)        只返回 bool
lower_bound(b, e, v)          第一个 >= v
upper_bound(b, e, v)          第一个 > v
equal_range(b, e, v)          等于 v 的区间

// ── 线性查找 O(n) ──
find / find_if / find_first_of / search / count / count_if
min_element / max_element / minmax_element
all_of / any_of / none_of

// ── 修改 ──
copy / copy_if / transform / fill / generate / replace
unique          去掉【相邻】重复（先 sort）
remove_if       只移动不删除，配合 erase
rotate / reverse

// ── 划分 ──
partition / stable_partition / partition_point

// ── 集合运算 ──  ★ 两边都必须有序
set_union / set_intersection / set_difference / includes / merge

// ── 数值 <numeric> ──
accumulate(b, e, init)                     严格从左到右，不能并行
reduce(b, e, init)                         顺序不保证，可并行（要求结合律）
inner_product(b1, e1, b2, init)            点积
transform_reduce(b, e, init, op, f)        map-reduce
partial_sum / inclusive_scan               前缀和
adjacent_difference                        相邻差分
iota(b, e, start)                          填 start, start+1, ...
gcd / lcm / midpoint / lerp

// ── 堆 ──
make_heap / push_heap / pop_heap / sort_heap
```

## 6.6 字符串

```cpp
// 拼接：性能差 10 倍以上
std::string r;
for (...) r = r + "x";        // ❌ 每次都产生新字符串
r.reserve(n);
for (...) r += "x";           // ✅ 原地追加

// 数值转换：三个档次
std::to_string(42) / std::stoi(s)         // 方便，慢，会抛异常，看 locale
std::ostringstream / istringstream         // 灵活，最慢
std::to_chars / std::from_chars            // ★ 最快：不分配、不抛、不看 locale
```

**热点路径（日志、JSON、协议解析）一律用 `to_chars` / `from_chars`。**

```cpp
char buf[32];
auto [ptr, ec] = std::to_chars(buf, buf + 32, 12345);
std::string s(buf, ptr);

int v;
auto [p2, ec2] = std::from_chars(sv.data(), sv.data() + sv.size(), v);
if (ec2 != std::errc{}) { /* 解析失败 */ }
```

**分割字符串**（标准库没直接给）：

```cpp
std::vector<std::string_view> split(std::string_view sv, char delim) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= sv.size()) {
        size_t pos = sv.find(delim, start);
        if (pos == std::string_view::npos) { out.push_back(sv.substr(start)); break; }
        out.push_back(sv.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}
```

**正则 `<regex>`**：功能全，但**非常慢**（比其它语言的实现慢一个数量级），
且编译时间长。热路径别用，简单匹配手写更快。

## 6.7 时间 `<chrono>`

```cpp
using namespace std::chrono;
using namespace std::chrono_literals;

auto total = 1h + 30min + 45s;               // 类型安全，自动换算单位
duration_cast<seconds>(total).count();
duration<double>(total).count();

// ★ 测耗时一定用 steady_clock（单调递增，不受系统时间调整影响）
auto t0 = steady_clock::now();
work();
auto us = duration_cast<microseconds>(steady_clock::now() - t0).count();

// 显示日期用 system_clock（挂钟时间，可被 NTP 调整）
auto now = system_clock::now();
```

| 时钟 | 特性 | 用途 |
|------|------|------|
| `steady_clock` | 单调，绝不回退 | **测耗时、超时** |
| `system_clock` | 挂钟时间，可调整 | 显示日期、时间戳 |
| `high_resolution_clock` | 通常是上面某一个的别名 | 不推荐直接用 |

## 6.8 随机数 `<random>`

**不要用 `rand()`**：周期短、低位不随机、`rand() % n` 有偏差、线程不安全。

```cpp
std::random_device rd;              // 熵源（可能很慢，只用来播种）
std::mt19937 gen(rd());             // 引擎：产生均匀比特流

std::uniform_int_distribution<int>     dice(1, 6);      // 分布：把比特流塑形
std::uniform_real_distribution<double> unit(0.0, 1.0);
std::normal_distribution<double>       gauss(0.0, 1.0);
std::discrete_distribution<>           weighted({10, 30, 60});   // 加权

int roll = dice(gen);
std::shuffle(v.begin(), v.end(), gen);

// 多线程：每线程一个引擎，别共享
thread_local std::mt19937 tls_gen{std::random_device{}()};
```

**架构要点：引擎和分布是分开的。** 引擎产生原始随机比特，分布负责把它变成
你要的形状。这样任意引擎可以配任意分布。

## 6.9 函数对象 `<functional>`

```cpp
std::function<int(int,int)> f = [](int a, int b) { return a + b; };
```

**代价**：可能有堆分配（超过小对象优化容量时）+ 间接调用 + **无法内联**。
热点循环里慎用，那里应该用模板参数或直接传 lambda。

**【用在哪】** 回调注册表、事件总线、命令队列、任何「存起来以后调用」的场合。

```cpp
// std::bind 已过时，一律用 lambda
auto add5_old = std::bind(add, 5, std::placeholders::_1);   // ❌ 慢、难读、报错恐怖
auto add5_new = [](int b) { return add(5, b); };            // ✅

std::invoke(&S::method, obj, args...);      // 统一调用语法（成员函数/成员变量/普通可调用）
std::ref(x)                                  // 把引用放进容器或传给按值取参的模板
std::greater<>{}                             // 透明比较器，<> 表示类型自动推导
```

## 6.10 智能指针选型（再强调一次）

| | 大小 | 开销 | 用在哪 |
|---|---|---|---|
| `unique_ptr` | 8 字节（同裸指针） | **零** | **默认选它** |
| `shared_ptr` | 16 字节 | 控制块 + 原子引用计数 | 确实需要共享所有权 |
| `weak_ptr` | 16 字节 | 同上 | 打破循环引用、缓存、观察者 |

**函数参数怎么传**

```cpp
void f(const Widget& w);              // 只是用一下 ← 最常见
void f(Widget* w);                    // 只是用一下，且可以为空
void f(std::unique_ptr<Widget> w);    // 接管所有权（调用方必须 std::move）
void f(std::shared_ptr<Widget> w);    // 共享所有权
void f(const std::shared_ptr<W>& w);  // 想读 shared_ptr 本身（少见）
```

**不要**为了「看起来现代」就到处传 `shared_ptr` —— 每次拷贝都是一对原子操作。

---

<div style="page-break-after: always;"></div>
