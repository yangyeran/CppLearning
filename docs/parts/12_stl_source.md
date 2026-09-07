# 第 15 章 · STL 源码剖析

▶ 对应程序：`ch15_stl_source`

读标准库源码最大的障碍不是算法难，而是它为了「通用 + 极致性能 + 异常安全」被层层包裹：满屏 `_Ty`、`__niter_wrap`、`_STD_BEGIN`、`_NODISCARD`、SFINAE。

所以本章的做法是**把每个组件的核心机制单独重写一遍**，去掉所有工程包装，只留下"为什么必须这么设计"的那部分代码，并与标准库对照验证。

## 1. 六大组件与设计哲学

STL 不是"一堆好用的类"，而是一套**正交分解**：

| 组件 | 职责 | 例子 |
|---|---|---|
| 容器 Container | 只管"怎么存" | vector / list / map |
| 算法 Algorithm | 只管"怎么算" | sort / find / accumulate |
| 迭代器 Iterator | 连接容器与算法 | 容器提供，算法消费 |
| 仿函数 Functor | 把"行为"参数化 | `less<>` / `plus<>` / lambda |
| 适配器 Adapter | 改造已有组件的接口 | stack / reverse_iterator |
| 分配器 Allocator | 把"内存来源"参数化 | allocator / 自定义内存池 |

**为什么要这么拆？** 如果不拆，M 个容器 × N 个算法 = M×N 份实现。拆开之后是 M + N 份：任何算法自动支持任何满足要求的容器。

```cpp
auto count_gt2 = [](auto first, auto last) {
    return std::count_if(first, last, [](int x) { return x > 2; });
};
// 同一份逻辑，作用于 vector / list / set / C 数组
```

**代价也很明确，它解释了很多让人困惑的 API：**

- 算法拿不到容器本身，只有迭代器区间 → 所以 `std::remove` **删不掉**元素（第 13 章 2.4）。它根本不知道容器长什么样。
- `list` 有自己的 `sort()` 成员，因为 `std::sort` 要求随机访问迭代器。
- 算法用**半开区间** `[first, last)` 而不是闭区间：这样 `last` 可以是"尾后位置"，空区间自然表示成 `first == last`，且元素个数 = `last - first`。

## 2. 迭代器与 iterator_traits

**核心问题**：算法怎么知道"这个迭代器能不能 `+n`""它指向的元素是什么类型"？

答案是靠迭代器自己声明的 5 个关联类型，通过 `iterator_traits` 统一查询：`value_type`、`difference_type`、`pointer`、`reference`、`iterator_category`。

`iterator_traits` 是个**间接层**：既能查询自定义迭代器内部声明的 typedef，也能通过偏特化支持裸指针（裸指针没法在内部声明 typedef）。

### 五种迭代器能力

```
input_iterator          只读，单向，一次性（读过不能回头）   istream_iterator
     ↓
forward_iterator        可读写，单向，可多次遍历            forward_list
     ↓
bidirectional_iterator  ++ 和 --                          list / map / set
     ↓
random_access_iterator  +n、-n、[]、迭代器相减（O(1) 跳转） vector / deque / 数组
     ↓
contiguous_iterator     元素在内存里物理连续（C++20 新增）  vector / array / span
```

分这么细是为了让算法**据此选择不同实现**：

| 操作 | 随机访问 | 其它 |
|---|---|---|
| `std::advance(it, 1000)` | `it += 1000`，一步到位 | 循环 `++it` 1000 次 |
| `std::distance(a, b)` | `b - a`，O(1) | 边走边数，O(n) |

### 编译期分派的三代写法

**第一代：tag dispatch（C++11/14）** —— 用"重载 + 空标签类型"在编译期选择实现，零运行时开销：

```cpp
template <typename It, typename D>
void advance_impl(It& it, D n, std::random_access_iterator_tag) { it += n; }
template <typename It, typename D>
void advance_impl(It& it, D n, std::bidirectional_iterator_tag) { while (n--) ++it; }

template <typename It, typename D>
void advance(It& it, D n) {
    advance_impl(it, n, typename std::iterator_traits<It>::iterator_category{});
}
```

**第二代：if constexpr（C++17）** —— 更直观：

```cpp
template <typename It, typename D>
void advance17(It& it, D n) {
    using Cat = typename std::iterator_traits<It>::iterator_category;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, Cat>) {
        it += n;
    } else {
        while (n-- > 0) ++it;
    }
}
```

关键：**未选中的分支不参与编译**。所以 `it += n` 出现在这里也不会让 list 迭代器编译失败 —— 这正是 `if constexpr` 取代 tag dispatch 的原因。

**第三代：concepts（C++20）** —— 报错信息最友好，概念更特化者优先：

```cpp
template <std::random_access_iterator It> void advance20(It& it, std::ptrdiff_t n) { it += n; }
template <std::input_iterator It>         void advance20(It& it, std::ptrdiff_t n) { while (n-- > 0) ++it; }
```

## 3. allocator 与内存池

**为什么 STL 要把内存分配抽象成 allocator，而不是直接 new？**

**理由 1：分离「分配内存」与「构造对象」。**

```cpp
std::allocator<std::string> alloc;
std::string* raw = alloc.allocate(3);      // 只要内存，**不构造** string 对象
std::construct_at(raw, "第一个");           // C++20；等价于 placement new
std::destroy_at(raw);                       // 调析构，不释放内存
alloc.deallocate(raw, 3);                   // 还内存
```

这就是 `vector::reserve(1000)` 不会构造 1000 个对象的原因。如果用 `new T[1000]`，就会强制调用 1000 次构造函数。

**理由 2：可替换内存来源** —— 共享内存、GPU 显存、栈上 buffer、内存池。

**理由 3：小对象频繁分配时，malloc 的开销（加锁 + 元数据 + 碎片）占比极高。**

### 内存池的核心技巧

SGI STL 的经典方案是「二级分配器」：>128 字节直接 malloc；≤128 字节按 8 字节对齐分成 16 条自由链表，从内存池切块。

本章实现的 `PoolAllocator` 展示了最关键的技巧：

```cpp
union Slot {
    Slot* next;                              // 空闲时：链表节点
    alignas(T) unsigned char storage[sizeof(T)];   // 占用时：对象
};
```

**slot 与对象共用同一块内存** —— 链表指针不占额外空间。分配 = 从链表头摘一个（O(1)，无锁无系统调用），释放 = 挂回链表头。

实测给 `std::list<int>` 换上这个分配器，插入 200 个节点只触发了 **4 次**真正的系统调用（每 chunk 64 个 slot）。

### C++17 起用 PMR 更好

```cpp
char buf[8192];
std::pmr::monotonic_buffer_resource pool{buf, sizeof buf};
std::pmr::vector<int> v{&pool};      // 直接在栈 buffer 上分配，零系统调用
```

PMR 用**运行时多态**替代模板参数，所以 `pmr::vector<int>` 是同一个类型，不会因为分配器不同而类型不兼容 —— 这是老式 allocator 的最大痛点。

## 4. vector

### 4.1 三个指针就是全部状态

```
begin_          end_              cap_
  ↓               ↓                 ↓
┌───┬───┬───┬───┬───┬───┬───┬───┐
│ 1 │ 2 │ 3 │ 4 │   │   │   │   │
└───┴───┴───┴───┴───┴───┴───┴───┘
|<---- size() 4 ---->|
|<---------- capacity() 8 -------->|
```

`size() = end_ - begin_`，`capacity() = cap_ - begin_`。

### 4.2 为什么扩容是乘法而不是加法

- 每次 **+k**：插入 n 个元素总搬移量 = k + 2k + … = O(n²/k)，**二次复杂度**
- 每次 **×2**：搬移量 = 1 + 2 + 4 + … + n < 2n，**均摊 O(1)**

这就是"均摊常数时间"的来源。实测扩容轨迹：`1 2 4 8 16 32 64` —— 33 次 `push_back` 只触发 6 次重新分配。

**为什么 MSVC 用 1.5 倍而 GCC/Clang 用 2 倍？**

- 2 倍：新块永远比"之前所有释放块的总和"还大（1+2+4 < 8），**永远无法复用**之前释放的空间，堆碎片更多
- 1.5 倍：增长几次后新块可以放进之前释放的空隙里（内存复用性更好），但搬移次数更多

两种都是合理取舍，没有绝对优劣。

### 4.3 reserve 里藏着异常安全的全部要点

```cpp
void reserve(size_type new_cap) {
    if (new_cap <= capacity()) return;

    // 1) 先在新内存上完成所有工作，**旧数据保持完好**
    T* new_begin = alloc(new_cap);
    T* new_end   = new_begin;
    try {
        for (T* p = begin_; p != end_; ++p) {
            if constexpr (std::is_nothrow_move_constructible_v<T>) {
                construct(new_end, std::move(*p));
            } else {
                construct(new_end, *p);          // 拷贝，抛异常可回滚
            }
            ++new_end;
        }
    } catch (...) {
        // 2) 出错：销毁新内存上已构造的部分，还掉新内存，旧状态一点没动
        destroy_range(new_begin, new_end);
        dealloc(new_begin, new_cap);
        throw;                                    // 调用方看到的是「什么都没发生」
    }
    // 3) 全部成功，才切换到新内存
    ...
}
```

### 4.4 移动构造必须 noexcept —— 实测的代价

那个 `if constexpr` 不是优化，是**正确性要求**：

如果移动构造可能抛异常，搬到第 5 个元素时抛了，前 4 个已经被搬空，旧内存里是"被移动过"的残骸，**无法回滚** —— vector 就废了。拷贝失败则可以直接丢弃新内存，旧数据完好，能给出**强异常保证**。

这就是"为什么移动构造函数一定要标 noexcept"的真正原因。实测（用探针类型统计）：

| 移动构造是否 noexcept | 扩容时的移动次数 | 拷贝次数 |
|---|---|---|
| 是 | 7 | 0 |
| **否** | **0** | **7** |

**少写一个 `noexcept`，vector 扩容就退化成全量深拷贝。**

### 4.5 reserve 与 emplace_back 的实测收益

| 操作 | 构造 | 拷贝 | 移动 | 析构 |
|---|---|---|---|---|
| 8 次 `emplace_back`，不 reserve | 8 | 0 | **7** | 15 |
| 先 `reserve(8)` 再插入 | 8 | 0 | **0** | 8 |
| `push_back(Probe{1})` | 1 | 0 | **1** | 2 |
| `emplace_back(1)` | 1 | 0 | **0** | 1 |

`push_back(T{args})` 要先构造临时对象再移动进容器；`emplace_back(args)` 直接在目标位置构造，省掉一次移动和一次析构。

**已知元素个数就一定要 reserve** —— 这是最廉价的优化。Release 实测：20 万次 `push_back`，加 reserve 后快 **5~6 倍**。

## 5. string 与 SSO

**问题**：`std::string("hi")` 如果总是堆分配，那"短字符串满天飞"的程序（键名、标签、路径片段）就会被 malloc 拖死。

**方案**：SSO (Small String Optimization)。用一个 union 把两种布局叠在一起：

```
长字符串模式（heap）：            短字符串模式（栈内联）：
┌──────────────────┐             ┌──────────────────┐
│ char* data       │ 8 字节       │ char buf[23]     │ 直接存字符
│ size_t size      │ 8 字节       │                  │
│ size_t capacity  │ 8 字节       │ uint8_t 标志+长度 │ 最高位当模式标志
└──────────────────┘ = 24         └──────────────────┘ = 24
```

用"本来就要占的那几十个字节"换掉了一次堆分配。

各实现的差异：

| 实现 | `sizeof(std::string)` | SSO 阈值 |
|---|---|---|
| libc++ | 24 | 22 |
| libstdc++ | 32 | 15 |
| MSVC (Release) | 32 | 15 |
| MSVC (Debug) | 40 | 15 |

MSVC Debug 多出的 8 字节是迭代器调试信息（`_ITERATOR_DEBUG_LEVEL=2`），不是 SSO 缓冲区。

**代价**：string 的移动**不是**纯指针搬运 —— 短字符串模式下数据就在对象内部，没有指针可偷，只能逐字节拷贝：

```cpp
MyString(MyString&& o) noexcept {
    if (o.is_heap()) {
        heap_ = o.heap_;        // 偷指针
        o.set_small_size(0);    // 源变成空短串
    } else {
        small_ = o.small_;      // 只能拷贝
    }
}
```

**实用推论：**

- 短字符串当值传递/返回是廉价的，别为此改用 `const char*`
- `reserve` 对 string 同样有效（大量 `+=` 时）
- `string_view` 的价值主要在**长**字符串的子串上（短的本来就不分配）

## 6. list 与哨兵节点

双向链表的实现难点全在**边界处理**：插入第一个元素、删除最后一个元素、空链表……如果 head/tail 是裸指针，每个操作都要写一堆 `if (head == nullptr)`。

STL 的解法是**哨兵节点**（sentinel / dummy head）—— 一个不存数据的节点，让链表永远成环：

```
     ┌──────────────────────────────────────┐
     ↓                                      │
┌─────────┐    ┌───────┐    ┌───────┐      │
│ 哨兵    │───►│  1    │───►│  2    │──────┘
│ (end()) │◄───│       │◄───│       │
└─────────┘    └───────┘    └───────┘
     ↑
   begin() = 哨兵->next
```

三个直接好处：

1. 空链表也是合法的环（哨兵自己指自己），**没有 nullptr 分支**
2. `end()` 就是哨兵本身，天然满足"end 是尾后位置"且可以 `--end()`
3. 插入/删除只需改 4 个指针，永远不用判断"是否是头/尾"

```cpp
// 所有插入都归结到这一个函数 —— 数一下，没有任何 if
iterator emplace(iterator pos, Args&&... args) {
    Node* node = new Node(std::forward<Args>(args)...);
    NodeBase* next = pos.p_;
    NodeBase* prev = next->prev;
    node->prev = prev;  node->next = next;
    prev->next = node;  next->prev = node;
    ++size_;
    return iterator(node);
}
```

注意哨兵用的是 `NodeBase`（只有指针）而不是 `Node`（含 T）—— 因为 T 可能没有默认构造函数，而且哨兵不需要存值。

### splice —— list 的杀手级操作

```cpp
a.splice(a.begin(), b);     // O(1) 把 b 整体接过来，只改 4 个指针
```

100 万个元素也是 O(1)。vector 做不到（必须逐个搬）。

### 但 list 为什么这么慢？

每个节点独立分配 → 内存分散 → 缓存不友好。Release 实测遍历 20 万个 int：

| 容器 | 耗时 | 相对 vector |
|---|---|---|
| vector | 14 μs | 1× |
| deque | 100 μs | ~7× |
| list | 324 μs | **~23×** |

原因：vector 元素连续，一次缓存行（64 字节）加载 16 个 int；list 几乎每次访问都是缓存未命中。

**list 只在「频繁在中间插删 + 需要迭代器长期有效 + splice」时才划算。**

## 7. 红黑树（map / set 的底层）

**为什么不用普通二叉搜索树？** 有序插入会退化成链表：插入 1,2,3,4,5 → 树高 = n，查找变 O(n)。

红黑树用 5 条不变式把树高约束在 O(log n)：

1. 每个节点是红色或黑色
2. 根节点是黑色
3. 所有叶子（NIL）是黑色
4. **红色节点的两个子节点必须是黑色**（不能有连续红节点）
5. **从任一节点到其所有后代 NIL 的路径包含相同数目的黑节点**

规则 4 + 5 共同保证：最长路径 ≤ 2 × 最短路径，于是树高 ≤ 2·log₂(n+1)。

实测（本章实现带不变式校验）：

| 输入 | 行数 | 实际树高 | 理论下界 | 红黑树上界 | 普通 BST |
|---|---|---|---|---|---|
| 顺序插入（最坏输入） | 1000 | 17 | 9 | 19 | **1000** |
| 随机插入 | 10000 | 16 | 13 | 26 | ~30 |

### 插入的三种修复情形

新节点总是红色（这样不破坏规则 5，只可能破坏规则 4）。只在"父节点也是红色"时需要修复：

**情形 1：叔叔 U 是红色** → 纯变色，问题上移两层

```
     G(黑)                  G(红)  <- 继续向上检查
    /    \                 /    \
  P(红)  U(红)   ====>   P(黑)  U(黑)
  /                      /
N(红)                  N(红)
```

**情形 2：叔叔黑，N 是「内侧」孙子** → 先旋转成情形 3

```
     G(黑)                    G(黑)
    /    \                   /    \
  P(红)  U(黑)   ====>     N(红)  U(黑)      左旋 P
    \                      /
    N(红)                P(红)
```

**情形 3：叔叔黑，N 是「外侧」孙子** → 旋转 + 变色，修复完成

```
     G(黑)                    P(黑)
    /    \                   /    \
  P(红)  U(黑)   ====>     N(红)  G(红)      右旋 G
  /                                  \
N(红)                                U(黑)
```

**为什么用红黑树而不是 AVL 树？** AVL 更平衡（查找略快），但插入/删除需要更多旋转来维持严格平衡。红黑树的"近似平衡"让修改操作的旋转次数是 O(1) 均摊，在"读写混合"场景总体更快。

**为什么删除比插入难得多？** 插入的新节点是红色，最多只破坏规则 4，情形只有 3 种。删除黑节点会破坏规则 5（黑高不等），要"借"一个黑节点回来，情形有 4 种且需要区分兄弟节点的孩子颜色，代码量是插入的 2~3 倍。这也是为什么很多人手写红黑树只写插入（本章也是）。

### map/set 的实际后果

- 节点独立分配 → 缓存不友好，实测比 `unordered_map` 慢 **4~5 倍**
- 但有序、迭代器稳定（插删不失效）、支持 `lower_bound` 范围查询
- 小数据量（< 几十个）时 sorted vector 或 `flat_map`（C++23）更快

## 8. 哈希表（unordered_map 的底层）

结构：**桶数组 + 链地址法**（separate chaining）

```
buckets_
┌───┐
│ 0 │──► ("cat",1) ──► ("act",9) ──► nullptr     哈希冲突时挂同一条链
├───┤
│ 1 │──► nullptr
├───┤
│ 2 │──► ("dog",2) ──► nullptr
└───┘
```

### 三个关键设计决策

**桶数怎么选？** libstdc++ / MSVC 用**质数**（13, 29, 59, 127…），因为取模质数能更好地打散有规律的哈希值。有些实现用 2 的幂（可以用位与代替取模，更快），代价是要求哈希函数质量更高。

**负载因子** = 元素数 / 桶数，默认上限 1.0。超过就 rehash（桶数翻倍并重新分配所有元素）。实测桶数变化：`13 → 29 → 59 → 127 → 257 → 541`。

**缓存哈希值。** 每个节点存一份 `hash`：

- rehash 时不用重新调用哈希函数
- 查找时先比 hash（整数比较）再比 key（可能是字符串比较），快得多

**为什么 rehash 会让迭代器失效但引用/指针不失效？** 因为 rehash 只是把节点重新挂到不同的桶上，**节点本身没有移动**。迭代器要靠桶索引来遍历，桶变了就失效；但指向节点里 value 的指针和引用依然有效。

### 哈希函数质量的影响（实测）

| 哈希函数 | 链长分布 | 最长链 |
|---|---|---|
| 恒返回 42（坏） | 全部 100 个元素挂一条链 | **100** |
| `std::hash` | 0→326桶 1→149桶 2→51桶 3→11桶 4→4桶 | 4 |

坏哈希让查找退化成 O(n)。

### 自定义类型做键

```cpp
struct Point { int x, y; bool operator==(const Point&) const = default; };

template <> struct std::hash<Point> {
    size_t operator()(const Point& p) const noexcept {
        size_t h = std::hash<int>{}(p.x);
        // 组合哈希：别用简单 XOR（(1,2) 和 (2,1) 会撞）
        h ^= std::hash<int>{}(p.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};
```

那个 `0x9e3779b9` 是黄金比例的定点表示，`boost::hash_combine` 就用它，作用是让每一位的影响均匀扩散到整个哈希值。

## 9. deque 的分段连续

deque 既要"两端 O(1) 插删"又要"O(1) 随机访问"，所以它不是单块连续内存：

```
map_（中控器，本身是一个 T** 数组）
┌────┬────┬────┬────┐
│ p0 │ p1 │ p2 │ p3 │
└─┬──┴─┬──┴─┬──┴─┬──┘
  ↓    ↓    ↓    ↓
[缓冲区][缓冲区][缓冲区][缓冲区]     每块固定 512 字节 / sizeof(T) 个元素
   ↑                        ↑
 start                     finish
```

- `push_front` → 在首块前面留的空位里放，满了就新分配一块挂到中控器前端
- `operator[i]` → 先算 i 落在第几块（`i / 块大小`），再算块内偏移（`i % 块大小`）—— 两次寻址，所以比 vector 的 `[]` 慢，但仍是 O(1)

迭代器要存 4 个指针（当前元素、当前块首、当前块尾、指向中控器的位置），所以 deque 的迭代器比 vector 的裸指针"重"得多。

**重要细节**：deque 的 `push_back`/`push_front` **不会**使引用和指针失效（已有的块不动），但**会**使迭代器失效（中控器可能重新分配）。这和 vector"全都失效"、list"全都不失效"形成三档差异。

## 10. std::sort = 内省排序（introsort）

三种排序算法各有致命弱点，introsort 把它们组合起来互相补位：

| 算法 | 优点 | 致命弱点 |
|---|---|---|
| 快速排序 | 平均最快（缓存友好、常数小） | 最坏 O(n²) |
| 堆排序 | 稳定 O(n log n) 保底 | 常数大、缓存不友好 |
| 插入排序 | 小数据和"几乎有序"时最快 | 一般情况 O(n²) |

**introsort 的策略：**

1. 主体用快排（**三点取中**选 pivot：取首、中、尾的中位数，抗有序输入）
2. 递归深度超过 `2·log₂(n)` → 判定快排退化，切**堆排序**保底
3. 区间缩小到 ≤ 16 个元素 → 停止递归，最后统一做一次**插入排序**

于是最坏情况也是 O(n log n)，平均情况保持快排的速度。这个算法由 David Musser 在 1997 年提出，此后成为所有 STL 实现的标准做法。

还有一个细节：递归处理较小的一半，循环处理较大的一半 → 栈深度 O(log n) 而不是 O(n)。

### 相关算法的选择

| 算法 | 特性 | 复杂度 |
|---|---|---|
| `std::sort` | 不稳定，原地。**默认选它** | O(n log n) |
| `std::stable_sort` | 稳定（相等元素保持原序），归并排序 | O(n log n)，需 O(n) 额外内存 |
| `std::partial_sort` | 只要前 k 个 → 堆排序 | O(n log k) |
| `std::nth_element` | 只要第 k 个就位 → 快速选择 | 平均 O(n) |

**"只要前 10 名"千万别 sort 全部再取前 10** —— 用 `partial_sort`。

## 11. 性能实测与容器选型

### 选型决策树

```
需要键值查找?
 ├─ 是 → 需要有序遍历/范围查询?
 │        ├─ 是 → map / set (红黑树, O(log n), 迭代器稳定)
 │        └─ 否 → unordered_map / unordered_set (哈希, O(1) 均摊)
 │                 元素少(<32)且频繁遍历? → flat_map (C++23) 或 sorted vector
 └─ 否 → 大小编译期已知?
          ├─ 是 → std::array (栈上, 零开销)
          └─ 否 → 只在尾部增删?
                   ├─ 是 → std::vector   ← 默认就选这个
                   └─ 否 → 两端都增删?
                            ├─ 是 → std::deque
                            └─ 否 → 中间频繁插删且需要稳定迭代器?
                                     ├─ 是 → std::list
                                     └─ 否 → 还是 vector（实测通常更快）
```

### Release 实测数据（20 万 int / 10 万字符串键，best-of-5）

| 操作 | 耗时 |
|---|---|
| vector push_back | 678 μs |
| vector push_back + **reserve** | **132 μs**（快 5 倍） |
| deque push_back | 2742 μs |
| list push_back | 8996 μs |
| vector 遍历 | 14 μs |
| deque 遍历 | 100 μs |
| list 遍历 | 324 μs（**慢 23 倍**） |
| map 查找 | 15125 μs |
| unordered_map 查找 | **3389 μs**（快 4.5 倍） |

### 一个诚实的反例

`unordered_map` 的 `reserve` 在 MSVC 上**没有变快，反而稳定慢 2.7 倍**（12752 μs vs 4717 μs）。

原因：MSVC 的 `unordered_map` 每个桶存两个迭代器（比 libstdc++ 的单指针桶重一倍），`reserve` 会一次性分配很大的桶数组，这笔开销抵消了省下的 rehash。libstdc++ 上则是明显收益。

**结论：`reserve` 对 vector 是无脑收益；对 unordered_map 要按你用的标准库实测，别照抄结论（包括本文的结论）。**

### 关于测量方法

本章的基准测试取**多次运行的最小值**，不是单次也不是平均值。理由：操作系统调度、其它进程抢 CPU、缓存冷启动只会让某次运行**变慢**，不可能让它变快，所以最小值最接近"这段代码本身的成本"。

用平均值的话，一次调度抖动就能让结论反过来 —— 开发本章时就遇到过：同一份代码两次运行，快慢结论正好相反。Google Benchmark 等基准库都用最小值/中位数。

**另外注意 Debug 构建的数字完全不可用作基准。** MSVC 的 Debug 开启迭代器调试检查（`_ITERATOR_DEBUG_LEVEL=2`），标准库容器每次访问都带边界校验，绝对耗时放大 5~30 倍：

```bash
cmake --build build --config Release
build\bin\Release\ch15_stl_source.exe
```

## 本章要点

1. 容器/算法/迭代器三者解耦，代价是 `std::remove` 删不掉元素
2. `iterator_traits` + tag dispatch 是编译期分派的经典实现，C++17 用 `if constexpr`、C++20 用 concepts 取代它
3. allocator 把"分配内存"和"构造对象"分开 → `reserve` 才能不构造对象
4. vector 2 倍扩容 = 均摊 O(1)；**移动构造必须 noexcept，否则扩容退化成全量深拷贝**
5. string 用 union 实现 SSO，短字符串零堆分配
6. list 的哨兵节点消灭了所有边界判断
7. 红黑树用 5 条不变式把树高压到 2·log₂n，插入 3 种修复情形
8. 哈希表 = 桶数组 + 链地址法 + 质数桶数 + 缓存哈希值；rehash 使迭代器失效但引用不失效
9. `std::sort` = 快排 + 堆排保底 + 插排收尾（introsort）
10. **缓存局部性常常比算法复杂度更决定实际性能**
