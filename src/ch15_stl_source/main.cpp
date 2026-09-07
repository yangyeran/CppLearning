// =============================================================================
// 第 15 章 —— STL 源码剖析（手写实现版）
//
// 读标准库源码最大的障碍不是算法难，而是它为了「通用 + 极致性能 + 异常安全」
// 被层层包裹：满屏 _Ty、__niter_wrap、_STD_BEGIN、_NODISCARD、SFINAE。
// 所以这一章的做法是：**把每个组件的核心机制单独重写一遍**，
// 去掉所有工程包装，只留下「为什么必须这么设计」的那部分代码。
//
// 目录：
//   15.1  STL 的六大组件与设计哲学
//   15.2  迭代器与 traits —— 算法和容器解耦的关键
//   15.3  allocator 与内存池 —— 为什么不直接 new
//   15.4  MyVector —— 扩容策略、异常安全、为什么是 2 倍
//   15.5  MyString —— SSO 小字符串优化
//   15.6  MyList —— 哨兵节点的妙用
//   15.7  红黑树 —— map / set 的底层
//   15.8  MyHashTable —— unordered_map 的底层
//   15.9  deque —— 分段连续与中控器
//   15.10 算法剖析 —— sort 为什么是「内省排序」
//   15.11 性能实测
//
// 运行： ch15_stl_source.exe
// =============================================================================

#include "demo.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <forward_list>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <new>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// =============================================================================
// 15.1  STL 的六大组件与设计哲学
// =============================================================================
static void s01_philosophy() {
    demo::title("15.1  STL 六大组件");

    std::cout <<
        "  STL 不是「一堆好用的类」，而是一套**正交分解**的设计：\n"
        "\n"
        "    容器 Container    只管「怎么存」        vector / list / map\n"
        "    算法 Algorithm    只管「怎么算」        sort / find / accumulate\n"
        "    迭代器 Iterator   连接容器与算法        容器提供，算法消费\n"
        "    仿函数 Functor    把「行为」参数化      less<> / plus<> / lambda\n"
        "    适配器 Adapter    改造已有组件的接口    stack / reverse_iterator / bind\n"
        "    分配器 Allocator  把「内存来源」参数化  allocator / 自定义内存池\n"
        "\n"
        "  为什么要这么拆？因为如果不拆，M 个容器 × N 个算法 = M*N 份实现。\n"
        "  拆开之后是 M + N 份：任何算法自动支持任何满足要求的容器。\n"
        "\n"
        "  代价也很明确（这解释了很多让人困惑的 API）：\n"
        "    - 算法拿不到容器本身，只有迭代器区间，所以 std::remove **删不掉**元素\n"
        "      （见第 13 章坑 10）—— 它根本不知道容器长什么样。\n"
        "    - list 有自己的 sort() 成员，因为 std::sort 要求随机访问迭代器。\n"
        "    - 算法用「半开区间 [first, last)」而不是闭区间，这样 last 可以是\n"
        "      「尾后位置」，空区间自然表示成 first == last，且元素个数 = last - first。\n";

    demo::section("正交分解的实际体现：同一个算法吃不同容器");
    std::vector<int>       v{5, 3, 1, 4};
    std::list<int>         l{5, 3, 1, 4};
    std::set<int>          s{5, 3, 1, 4};
    int                    raw[]{5, 3, 1, 4};

    auto count_gt2 = [](auto first, auto last) {
        return std::count_if(first, last, [](int x) { return x > 2; });
    };
    std::cout << "  同一份 count_if 逻辑作用于：\n";
    std::cout << "    vector -> " << count_gt2(v.begin(), v.end()) << "\n";
    std::cout << "    list   -> " << count_gt2(l.begin(), l.end()) << "\n";
    std::cout << "    set    -> " << count_gt2(s.begin(), s.end()) << "\n";
    std::cout << "    C 数组 -> " << count_gt2(std::begin(raw), std::end(raw)) << "\n";
}

// =============================================================================
// 15.2  迭代器与 iterator_traits
//
// 核心问题：算法怎么知道「这个迭代器能不能 +n」「它指向的元素是什么类型」？
// 答案：靠迭代器自己声明的 5 个「关联类型」，通过 iterator_traits 统一查询。
//
// 五种迭代器能力，逐级增强：
//
//   input_iterator          只读，单向，一次性（读过就不能回头）  istream_iterator
//        ↓
//   forward_iterator        可读写，单向，可多次遍历              forward_list
//        ↓
//   bidirectional_iterator  ++ 和 --                              list / map / set
//        ↓
//   random_access_iterator  + n、- n、[]、迭代器相减（O(1) 跳转） vector / deque / 数组
//        ↓
//   contiguous_iterator     元素在内存里物理连续（C++20 新增）    vector / array / span
//
// 为什么要分这么细？因为算法可以据此**选择不同实现**：
//   std::advance(it, 1000)  随机访问 -> it += 1000     一步到位
//                           其它     -> 循环 ++it       1000 次
//   std::distance           随机访问 -> last - first    O(1)
//                           其它     -> 边走边数        O(n)
// =============================================================================

// 自己实现一个 advance，展示「tag dispatch」这个 C++17 前的经典手法
namespace mystl {

// 版本 A：tag dispatch（C++11/14 的标准做法）
// 用「重载 + 空标签类型」在**编译期**选择实现，零运行时开销。
template <typename It, typename Distance>
void advance_impl(It& it, Distance n, std::random_access_iterator_tag) {
    std::cout << "      [tag dispatch] 随机访问：一次 += " << n << "\n";
    it += n;                                    // O(1)
}
template <typename It, typename Distance>
void advance_impl(It& it, Distance n, std::bidirectional_iterator_tag) {
    std::cout << "      [tag dispatch] 双向：循环 " << (n < 0 ? -n : n) << " 次\n";
    if (n >= 0) while (n--) ++it;
    else        while (n++) --it;               // O(n)
}
template <typename It, typename Distance>
void advance_impl(It& it, Distance n, std::input_iterator_tag) {
    std::cout << "      [tag dispatch] 输入迭代器：只能向前循环 " << n << " 次\n";
    while (n--) ++it;                           // O(n)，且不支持负数
}

template <typename It, typename Distance>
void advance(It& it, Distance n) {
    // iterator_traits 是「间接层」：既能查询自定义迭代器里声明的类型，
    // 也能通过偏特化支持裸指针（裸指针没法在内部声明 typedef）。
    advance_impl(it, n, typename std::iterator_traits<It>::iterator_category{});
}

// 版本 B：C++17 的 if constexpr（更直观，推荐）
template <typename It, typename Distance>
void advance17(It& it, Distance n) {
    using Cat = typename std::iterator_traits<It>::iterator_category;
    if constexpr (std::is_base_of_v<std::random_access_iterator_tag, Cat>) {
        std::cout << "      [if constexpr] 随机访问分支\n";
        it += n;
    } else {
        std::cout << "      [if constexpr] 通用循环分支\n";
        while (n-- > 0) ++it;
    }
    // 关键：**未选中的分支不参与编译**。所以 it += n 出现在这里
    // 也不会让 list 迭代器编译失败 —— 这正是 if constexpr 取代 tag dispatch 的原因。
}

// 版本 C：C++20 concepts（最清晰，编译错误信息最友好）
template <std::random_access_iterator It>
void advance20(It& it, std::ptrdiff_t n) {
    std::cout << "      [concepts] 匹配 random_access_iterator\n";
    it += n;
}
template <std::input_iterator It>
void advance20(It& it, std::ptrdiff_t n) {
    std::cout << "      [concepts] 匹配 input_iterator（更弱的约束）\n";
    while (n-- > 0) ++it;
}

} // namespace mystl

static void s02_iterators() {
    demo::title("15.2  迭代器与 traits");

    demo::section("iterator_traits 查询到的 5 个关联类型");
    using VecIt = std::vector<int>::iterator;
    std::cout << "  vector<int>::iterator\n";
    std::cout << "    value_type        元素类型          -> "
              << (std::is_same_v<std::iterator_traits<VecIt>::value_type, int> ? "int" : "?") << "\n";
    std::cout << "    difference_type   两迭代器之差      -> ptrdiff_t\n";
    std::cout << "    pointer / reference                 -> int* / int&\n";
    std::cout << "    iterator_category 能力标签          -> random_access_iterator_tag\n";
    std::cout << "  裸指针 int* 也有 traits（通过偏特化）  -> "
              << (std::is_same_v<std::iterator_traits<int*>::iterator_category,
                                 std::random_access_iterator_tag> ? "random_access" : "?") << "\n";

    demo::section("同一个 advance 调用，编译期分派到不同实现");
    std::vector<int> v{0, 1, 2, 3, 4, 5};
    std::list<int>   l{0, 1, 2, 3, 4, 5};

    auto vit = v.begin();
    mystl::advance(vit, 3);
    std::cout << "      vector 前进 3 后指向 " << *vit << "\n";

    auto lit = l.begin();
    mystl::advance(lit, 3);
    std::cout << "      list 前进 3 后指向   " << *lit << "\n";

    auto vit2 = v.begin();
    mystl::advance17(vit2, 2);
    auto lit2 = l.begin();
    mystl::advance17(lit2, 2);

    auto vit3 = v.begin();
    mystl::advance20(vit3, 4);
    auto lit3 = l.begin();
    mystl::advance20(lit3, 4);

    demo::section("为什么 list 不能用 std::sort");
    std::cout <<
        "  std::sort 内部要做 first + (last-first)/2 这类随机跳转，\n"
        "  list 的迭代器做不到（只能一个个 ++），编译直接失败。\n"
        "  所以 list 提供成员 l.sort()，用的是归并排序（链表归并只需改指针，O(1) 额外空间）。\n";
    l.sort();
    std::cout << "  l.sort() 后: ";
    for (int x : l) std::cout << x << ' ';
    std::cout << "\n";
}

// =============================================================================
// 15.3  allocator 与内存池
//
// 为什么 STL 要把「内存分配」抽象成 allocator，而不是直接 new？
//
//   1) 分离「分配内存」与「构造对象」。
//      vector 一次 reserve(1000) 只分配内存，**不构造** 1000 个对象。
//      如果用 new T[1000]，就会强制调用 1000 次构造函数。
//   2) 可替换内存来源：共享内存、GPU 显存、栈上 buffer、内存池。
//   3) 小对象频繁分配时，malloc 的开销（加锁 + 元数据 + 碎片）占比极高。
//      SGI STL 的经典方案是「二级分配器」：
//        > 128 字节 -> 直接 malloc
//        ≤ 128 字节 -> 按 8 字节对齐分成 16 条自由链表，从内存池切块
//
// allocator 的四个核心操作：
//   allocate(n)          分配能放 n 个 T 的**原始内存**（不构造）
//   deallocate(p, n)     释放
//   construct(p, args)   在 p 处 placement new 构造（C++20 起改用 construct_at）
//   destroy(p)           调析构，不释放内存
// =============================================================================

namespace mystl {

// 一个真实可用的固定大小内存池分配器（简化版 SGI 二级分配器思想）
// 思路：预先向系统申请大块内存（chunk），切成等大的 slot 串成自由链表。
//       分配 = 从链表头摘一个（O(1)，无锁无系统调用）
//       释放 = 挂回链表头（O(1)）
template <typename T>
class PoolAllocator {
public:
    using value_type = T;

    PoolAllocator() = default;
    // 容器内部会把 allocator<T> 转成 allocator<Node<T>>，必须提供这个转换构造
    template <typename U>
    PoolAllocator(const PoolAllocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        if (n != 1) {                        // 多个对象连续分配，退回 malloc
            ++stats().big_allocs;
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }
        ++stats().pool_allocs;
        if (!free_list()) refill();          // 自由链表空了，补货
        Slot* head  = free_list();
        free_list() = head->next;            // 摘链表头
        return reinterpret_cast<T*>(head);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        if (!p) return;
        if (n != 1) { ::operator delete(p); return; }
        Slot* s     = reinterpret_cast<Slot*>(p);
        s->next     = free_list();           // 挂回链表头
        free_list() = s;
    }

    template <typename U> bool operator==(const PoolAllocator<U>&) const noexcept { return true; }
    template <typename U> bool operator!=(const PoolAllocator<U>&) const noexcept { return false; }

    struct Stats { std::size_t pool_allocs = 0, big_allocs = 0, chunks = 0; };
    static Stats& stats() { static Stats s; return s; }

private:
    // slot 与对象共用同一块内存：空闲时当链表节点，占用时当对象。
    // 这是内存池的关键技巧 —— 链表指针**不占额外空间**。
    union Slot {
        Slot* next;
        alignas(T) unsigned char storage[sizeof(T)];
    };

    static constexpr std::size_t kChunkSlots = 64;

    static Slot*& free_list() { static Slot* head = nullptr; return head; }

    static void refill() {
        ++stats().chunks;
        Slot* chunk = static_cast<Slot*>(::operator new(kChunkSlots * sizeof(Slot)));
        // 把新 chunk 里的所有 slot 串成链表
        for (std::size_t i = 0; i < kChunkSlots - 1; ++i) chunk[i].next = &chunk[i + 1];
        chunk[kChunkSlots - 1].next = free_list();
        free_list() = chunk;
        // 注意：这个简化版**不归还** chunk 给系统（进程退出时统一回收），
        //       真实的 SGI 分配器同样如此 —— 这是内存池的常见取舍。
    }
};

} // namespace mystl

static void s03_allocator() {
    demo::title("15.3  allocator 与内存池");

    demo::section("分配内存 ≠ 构造对象");
    {
        std::allocator<std::string> alloc;
        std::string* raw = alloc.allocate(3);        // 只要内存，不构造
        std::cout << "  allocate(3)  拿到 " << 3 * sizeof(std::string)
                  << " 字节原始内存，此时**没有** string 对象存在\n";

        std::construct_at(raw + 0, "第一个");         // C++20；等价于 placement new
        std::construct_at(raw + 1, "第二个");
        std::construct_at(raw + 2, "第三个");
        std::cout << "  construct_at 之后: " << raw[0] << " / " << raw[1] << " / " << raw[2] << "\n";

        std::destroy_at(raw + 0);
        std::destroy_at(raw + 1);
        std::destroy_at(raw + 2);
        alloc.deallocate(raw, 3);
        std::cout << "  destroy_at 调析构；deallocate 还内存。两步分离。\n";
        std::cout << "  这就是 vector::reserve(1000) 不会构造 1000 个对象的原因。\n";
    }

    demo::section("自定义内存池分配器：给 list 换内存来源");
    {
        // list 每个节点都是独立小对象，是内存池收益最明显的场景
        using PoolList = std::list<int, mystl::PoolAllocator<int>>;
        PoolList pl;
        for (int i = 0; i < 200; ++i) pl.push_back(i);

        auto& st = mystl::PoolAllocator<int>::stats();
        std::cout << "  往 list 插入 200 个节点：\n";
        std::cout << "    从池中分配次数 = " << st.pool_allocs << "\n";
        std::cout << "    向系统申请 chunk 次数 = " << st.chunks
                  << "   <== 200 次分配只触发了 " << st.chunks << " 次真正的系统调用\n";
        std::cout << "    每 chunk 64 个 slot，所以 200 个节点需要 ceil(200/64)="
                  << (200 + 63) / 64 << " 个 chunk\n";
        std::cout << "  总和 = " << std::accumulate(pl.begin(), pl.end(), 0)
                  << "   （功能完全正常）\n";
    }

    std::cout <<
        "\n  C++17 起还有 <memory_resource>（PMR），比自定义 allocator 好用得多：\n"
        "    char buf[8192];\n"
        "    std::pmr::monotonic_buffer_resource pool{buf, sizeof buf};\n"
        "    std::pmr::vector<int> v{&pool};   // 直接在栈 buffer 上分配，零系统调用\n"
        "  PMR 用**运行时多态**替代模板参数，所以 pmr::vector<int> 是同一个类型，\n"
        "  不会因为分配器不同而类型不兼容 —— 这是老式 allocator 的最大痛点。\n";
}

// =============================================================================
// 15.4  MyVector —— 最重要的容器
//
// 三个数据成员就是全部状态：
//
//   begin_          end_              cap_
//     ↓               ↓                 ↓
//   ┌───┬───┬───┬───┬───┬───┬───┬───┐
//   │ 1 │ 2 │ 3 │ 4 │   │   │   │   │
//   └───┴───┴───┴───┴───┴───┴───┴───┘
//   |<---- size() 4 ---->|
//   |<---------- capacity() 8 -------->|
//
//   size()     = end_ - begin_
//   capacity() = cap_ - begin_
//
// 为什么扩容是「乘法」而不是「加法」？
//   若每次 +k：插入 n 个元素总搬移量 = k + 2k + ... = O(n²/k) -> 二次复杂度
//   若每次 ×2：搬移量 = 1 + 2 + 4 + ... + n < 2n -> 均摊 O(1)
//   这就是「均摊常数时间」的来源。
//
// 为什么 MSVC 用 1.5 倍而 GCC/Clang 用 2 倍？
//   2 倍：新块永远比「之前所有释放块的总和」还大（1+2+4 < 8），
//         永远无法复用之前释放的空间，堆碎片更多。
//   1.5 倍：增长几次后新块可以放进之前释放的空隙里（内存复用性更好），
//         但搬移次数更多。两种都是合理取舍，没有绝对优劣。
// =============================================================================

namespace mystl {

template <typename T, typename Alloc = std::allocator<T>>
class MyVector {
    using AllocTraits = std::allocator_traits<Alloc>;

public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;
    using iterator        = T*;              // vector 的迭代器就是裸指针（简化版）
    using const_iterator  = const T*;

    MyVector() = default;

    explicit MyVector(size_type n, const T& value = T()) {
        reserve(n);
        for (size_type i = 0; i < n; ++i) {
            AllocTraits::construct(alloc_, end_, value);
            ++end_;                          // 逐个 ++：中途抛异常时已构造的能被正确析构
        }
    }

    MyVector(std::initializer_list<T> il) {
        reserve(il.size());
        for (const auto& x : il) {
            AllocTraits::construct(alloc_, end_, x);
            ++end_;
        }
    }

    // 拷贝构造：只分配「实际需要」的容量，不复制源的 capacity
    MyVector(const MyVector& o) {
        reserve(o.size());
        for (const auto& x : o) {
            AllocTraits::construct(alloc_, end_, x);
            ++end_;
        }
    }

    // 移动构造：偷三个指针，把源置空。noexcept 很关键（见下面 reserve 的说明）
    MyVector(MyVector&& o) noexcept
        : begin_(o.begin_), end_(o.end_), cap_(o.cap_), alloc_(std::move(o.alloc_)) {
        o.begin_ = o.end_ = o.cap_ = nullptr;
    }

    // 统一赋值：copy-and-swap（自赋值安全 + 异常安全 + 一份代码兼顾拷贝与移动）
    MyVector& operator=(MyVector o) noexcept { swap(o); return *this; }

    ~MyVector() {
        clear();                                       // 析构所有元素
        if (begin_) AllocTraits::deallocate(alloc_, begin_, capacity());  // 再还内存
    }

    void swap(MyVector& o) noexcept {
        std::swap(begin_, o.begin_);
        std::swap(end_,   o.end_);
        std::swap(cap_,   o.cap_);
        std::swap(alloc_, o.alloc_);
    }

    // ---- 容量 ----
    size_type size()     const noexcept { return static_cast<size_type>(end_ - begin_); }
    size_type capacity() const noexcept { return static_cast<size_type>(cap_ - begin_); }
    bool      empty()    const noexcept { return begin_ == end_; }

    // reserve 是 vector 最值得读的函数：它包含了「异常安全」的全部要点
    void reserve(size_type new_cap) {
        if (new_cap <= capacity()) return;             // 从不缩小

        // 1) 先在新内存上完成所有工作，**旧数据保持完好**
        T* new_begin = AllocTraits::allocate(alloc_, new_cap);
        T* new_end   = new_begin;

        try {
            for (T* p = begin_; p != end_; ++p) {
                // 关键决策：能移动就移动，不能就拷贝。
                //
                // 但只有「移动构造是 noexcept」时才敢用移动！
                // 因为如果搬到第 5 个元素时移动构造抛异常，前 4 个已经被搬空，
                // 旧内存里是「被移动过」的残骸，**无法回滚** —— vector 就废了。
                // 拷贝失败则可以直接丢弃新内存，旧数据完好，能给出强异常保证。
                //
                // 这就是「为什么移动构造函数一定要标 noexcept」的真正原因。
                if constexpr (std::is_nothrow_move_constructible_v<T>) {
                    AllocTraits::construct(alloc_, new_end, std::move(*p));
                } else {
                    AllocTraits::construct(alloc_, new_end, *p);   // 抛异常可回滚
                }
                ++new_end;
            }
        } catch (...) {
            // 2) 出错：销毁新内存上已构造的部分，还掉新内存，旧状态一点没动
            for (T* p = new_begin; p != new_end; ++p) AllocTraits::destroy(alloc_, p);
            AllocTraits::deallocate(alloc_, new_begin, new_cap);
            throw;                                     // 原样抛出，调用方看到的是「什么都没发生」
        }

        // 3) 全部成功，才切换到新内存
        clear();
        if (begin_) AllocTraits::deallocate(alloc_, begin_, capacity());
        begin_ = new_begin;
        end_   = new_end;
        cap_   = new_begin + new_cap;
    }

    void clear() noexcept {
        // 倒序析构：与构造顺序相反，和栈上对象、成员变量的规则保持一致
        while (end_ != begin_) {
            --end_;
            AllocTraits::destroy(alloc_, end_);
        }
    }

    // ---- 修改 ----
    void push_back(const T& x) { emplace_back(x); }
    void push_back(T&& x)      { emplace_back(std::move(x)); }

    // emplace_back：直接在目标位置构造，省掉「先造临时对象再移动」的一步
    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (end_ == cap_) grow();
        AllocTraits::construct(alloc_, end_, std::forward<Args>(args)...);
        return *end_++;
    }

    void pop_back() {
        assert(!empty());
        --end_;
        AllocTraits::destroy(alloc_, end_);
    }

    // ---- 访问 ----
    reference       operator[](size_type i)       { return begin_[i]; }
    const_reference operator[](size_type i) const { return begin_[i]; }
    reference at(size_type i) {
        if (i >= size()) throw std::out_of_range("MyVector::at 下标越界");
        return begin_[i];
    }
    reference       front()       { return *begin_; }
    reference       back()        { return *(end_ - 1); }
    T*              data() noexcept { return begin_; }

    iterator       begin()       noexcept { return begin_; }
    iterator       end()         noexcept { return end_; }
    const_iterator begin() const noexcept { return begin_; }
    const_iterator end()   const noexcept { return end_; }

private:
    void grow() {
        // 增长因子 2；空 vector 首次分配 1 个（标准库通常也是 1 或 2）
        reserve(capacity() == 0 ? 1 : capacity() * 2);
    }

    T*    begin_ = nullptr;
    T*    end_   = nullptr;
    T*    cap_   = nullptr;
    Alloc alloc_{};
};

} // namespace mystl

// 用来观察拷贝/移动次数的探针类型
struct Probe {
    static int ctor, copy, move, dtor;
    int v;
    explicit Probe(int x = 0) : v(x) { ++ctor; }
    Probe(const Probe& o) : v(o.v) { ++copy; }
    Probe(Probe&& o) noexcept : v(o.v) { ++move; }      // noexcept！
    ~Probe() { ++dtor; }
    static void reset() { ctor = copy = move = dtor = 0; }
    static void report(const char* tag) {
        std::cout << "    " << demo::pad(tag, 30)
                  << " 构造=" << ctor << " 拷贝=" << copy
                  << " 移动=" << move << " 析构=" << dtor << "\n";
    }
};
int Probe::ctor = 0, Probe::copy = 0, Probe::move = 0, Probe::dtor = 0;

// 移动构造**不**标 noexcept 的版本，用来验证 reserve 的分支选择
struct ProbeThrowMove {
    static int copy, move;
    int v;
    explicit ProbeThrowMove(int x = 0) : v(x) {}
    ProbeThrowMove(const ProbeThrowMove& o) : v(o.v) { ++copy; }
    ProbeThrowMove(ProbeThrowMove&& o) : v(o.v) { ++move; }   // 没有 noexcept
    static void reset() { copy = move = 0; }
};
int ProbeThrowMove::copy = 0, ProbeThrowMove::move = 0;

static void s04_vector() {
    demo::title("15.4  MyVector");

    demo::section("扩容轨迹：观察容量怎么翻倍");
    {
        mystl::MyVector<int> v;
        std::size_t last_cap = 0;
        std::cout << "  ";
        for (int i = 0; i < 33; ++i) {
            v.push_back(i);
            if (v.capacity() != last_cap) {
                std::cout << v.capacity() << ' ';
                last_cap = v.capacity();
            }
        }
        std::cout << "\n  ^^^ 每次翻倍。33 次 push_back 只触发了 6 次重新分配。\n";
    }

    demo::section("reserve 的价值：避免重复搬移");
    {
        Probe::reset();
        {
            mystl::MyVector<Probe> v;
            for (int i = 0; i < 8; ++i) v.emplace_back(i);
        }
        Probe::report("不 reserve，8 次 emplace");

        Probe::reset();
        {
            mystl::MyVector<Probe> v;
            v.reserve(8);                    // 一次分配到位
            for (int i = 0; i < 8; ++i) v.emplace_back(i);
        }
        Probe::report("先 reserve(8) 再插入");
        std::cout << "    ^^^ 移动次数从 7 降到 0。已知数量就一定要 reserve。\n";
    }

    demo::section("push_back vs emplace_back");
    {
        Probe::reset();
        {
            mystl::MyVector<Probe> v;
            v.reserve(4);
            v.push_back(Probe{1});           // 构造临时对象 -> 移动进容器 -> 临时对象析构
        }
        Probe::report("push_back(Probe{1})");

        Probe::reset();
        {
            mystl::MyVector<Probe> v;
            v.reserve(4);
            v.emplace_back(1);               // 直接用参数 1 在容器内构造，无临时对象
        }
        Probe::report("emplace_back(1)");
        std::cout << "    ^^^ emplace_back 省掉了一次移动和一次析构。\n";
    }

    demo::section("noexcept 移动构造决定 reserve 用移动还是拷贝");
    {
        Probe::reset();
        mystl::MyVector<Probe> a;
        for (int i = 0; i < 4; ++i) a.emplace_back(i);
        a.reserve(64);
        std::cout << "    移动构造是 noexcept    -> 扩容时移动 " << Probe::move
                  << " 次，拷贝 " << Probe::copy << " 次\n";

        ProbeThrowMove::reset();
        mystl::MyVector<ProbeThrowMove> b;
        for (int i = 0; i < 4; ++i) b.emplace_back(i);
        b.reserve(64);
        std::cout << "    移动构造**不是** noexcept -> 扩容时移动 " << ProbeThrowMove::move
                  << " 次，拷贝 " << ProbeThrowMove::copy << " 次\n";
        std::cout << "    ^^^ 少写一个 noexcept，vector 扩容就退化成全量深拷贝。\n";
    }

    demo::section("功能正确性对照标准库");
    {
        mystl::MyVector<std::string> mv{"a", "b", "c"};
        std::vector<std::string>     sv{"a", "b", "c"};
        mv.push_back("d");  sv.push_back("d");
        mv.pop_back();      sv.pop_back();
        bool same = mv.size() == sv.size() &&
                    std::equal(mv.begin(), mv.end(), sv.begin());
        std::cout << "  MyVector 与 std::vector 行为一致: " << std::boolalpha << same << "\n";

        // 迭代器是裸指针，所以标准算法直接可用
        std::sort(mv.begin(), mv.end(), std::greater<>{});
        std::cout << "  std::sort 直接作用于 MyVector: ";
        for (const auto& s : mv) std::cout << s << ' ';
        std::cout << "\n";

        try { mv.at(99); } catch (const std::out_of_range& e) {
            std::cout << "  at(99) 抛出: " << e.what() << "\n";
        }
    }
}

// =============================================================================
// 15.5  MyString —— SSO 小字符串优化
//
// 问题：std::string("hi") 如果总是堆分配，那「短字符串满天飞」的程序
//       就会被 malloc 拖死。而短字符串恰恰是最常见的（键名、标签、路径片段）。
//
// 方案：SSO (Small String Optimization)。用一个 union 把两种布局叠在一起：
//
//   长字符串模式（heap）：            短字符串模式（栈内联）：
//   ┌──────────────────┐             ┌──────────────────┐
//   │ char* data       │ 8 字节       │ char buf[23]     │ 直接存字符
//   │ size_t size      │ 8 字节       │                  │
//   │ size_t capacity  │ 8 字节       │ uint8_t 剩余容量  │ 兼作模式标志
//   └──────────────────┘ = 24         └──────────────────┘ = 24
//
// 于是 sizeof(std::string) == 32（MSVC）或 32（libstdc++）—— 用「本来就要占的
// 那几十个字节」换掉了一次堆分配。libc++ 的阈值是 22 字节，MSVC 是 15 字节。
//
// 代价：string 的移动**不是**纯指针搬运（短字符串模式要逐字节拷贝），
//       所以 std::string 的移动构造在 MSVC 上不是 noexcept 的那么理所当然。
// =============================================================================

namespace mystl {

class MyString {
    // 让 sizeof(MyString) == 24，短模式能内联存 22 个字符 + '\0'
    static constexpr std::size_t kBufSize = 23;

    struct Heap {
        char*       data;
        std::size_t size;
        std::size_t capacity;
    };
    struct Small {
        char          buf[kBufSize];
        // 最高位当模式标志：1 = heap 模式。低 7 位存长度。
        unsigned char size_and_flag;
    };
    union {
        Heap  heap_;
        Small small_;
    };

    static constexpr unsigned char kHeapFlag = 0x80;

    bool is_heap() const noexcept { return (small_.size_and_flag & kHeapFlag) != 0; }
    void set_small_size(std::size_t n) noexcept {
        small_.size_and_flag = static_cast<unsigned char>(n);   // 高位为 0 = small
    }
    void set_heap_mode() noexcept { small_.size_and_flag = kHeapFlag; }

public:
    MyString() noexcept { small_.buf[0] = '\0'; set_small_size(0); }

    MyString(const char* s) {
        std::size_t n = std::strlen(s);
        if (n < kBufSize) {                       // 短：直接内联，零分配
            std::memcpy(small_.buf, s, n + 1);
            set_small_size(n);
        } else {                                  // 长：走堆
            heap_.size     = n;
            heap_.capacity = n;
            heap_.data     = new char[n + 1];
            std::memcpy(heap_.data, s, n + 1);
            set_heap_mode();
        }
    }

    MyString(const MyString& o) {
        if (o.is_heap()) {
            heap_.size     = o.heap_.size;
            heap_.capacity = o.heap_.size;
            heap_.data     = new char[heap_.size + 1];
            std::memcpy(heap_.data, o.heap_.data, heap_.size + 1);
            set_heap_mode();
        } else {
            small_ = o.small_;                    // 短模式：整个结构体一次拷贝
        }
    }

    // 移动构造：注意短模式下必须逐字节拷贝（数据就在对象内部，没有指针可偷）
    MyString(MyString&& o) noexcept {
        if (o.is_heap()) {
            heap_ = o.heap_;                      // 偷指针
            set_heap_mode();
            o.small_.buf[0] = '\0';               // 源变成空的短字符串
            o.set_small_size(0);
        } else {
            small_ = o.small_;                    // 只能拷贝，无法「偷」
        }
    }

    MyString& operator=(MyString o) noexcept { swap(o); return *this; }

    ~MyString() { if (is_heap()) delete[] heap_.data; }

    void swap(MyString& o) noexcept {
        // union 的按位交换：两种模式都能正确处理
        alignas(MyString) unsigned char tmp[sizeof(MyString)];
        std::memcpy(tmp,  this, sizeof(MyString));
        std::memcpy(this, &o,   sizeof(MyString));
        std::memcpy(&o,   tmp,  sizeof(MyString));
    }

    const char* c_str() const noexcept { return is_heap() ? heap_.data : small_.buf; }
    std::size_t size()  const noexcept {
        return is_heap() ? heap_.size
                         : static_cast<std::size_t>(small_.size_and_flag & 0x7F);
    }
    bool on_heap() const noexcept { return is_heap(); }
};

} // namespace mystl

static void s05_string_sso() {
    demo::title("15.5  MyString —— SSO");

    std::cout << "  sizeof(MyString)    = " << sizeof(mystl::MyString) << " 字节\n";
    std::cout << "  sizeof(std::string) = " << sizeof(std::string)
              << " 字节（本次编译的实测值）\n";
    std::cout <<
        "    各实现差异：libc++      24 字节（SSO 阈值 22）\n"
        "                libstdc++   32 字节（阈值 15）\n"
        "                MSVC Release 32 字节（阈值 15）；Debug 40 字节\n"
        "                Debug 多出的 8 字节是迭代器调试信息\n"
        "                (_ITERATOR_DEBUG_LEVEL=2)，不是 SSO 缓冲区\n\n";

    auto probe = [](const char* s) {
        mystl::MyString ms(s);
        std::cout << "    长度 " << std::setw(2) << ms.size()
                  << "  模式=" << (ms.on_heap() ? "堆分配" : "栈内联")
                  << "  内容=\"" << ms.c_str() << "\"\n";
    };
    std::cout << "  MyString（阈值 23）：\n";
    probe("");
    probe("hello");
    probe("0123456789012345678901");        // 22 字符，刚好内联
    probe("0123456789012345678901234567890");// 31 字符，转堆

    demo::section("SSO 为什么重要：实测短字符串构造开销");
    {
        constexpr int kN = 200000;
        auto bench = [](const char* tag, auto&& fn) {
            auto t0 = std::chrono::steady_clock::now();
            fn();
            auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            std::cout << "    " << demo::pad(tag, 40) << dt << " us\n";
        };

        bench("短字符串 (SSO 命中, 无堆分配)", [&] {
            for (int i = 0; i < kN; ++i) { std::string s = "short"; (void)s.size(); }
        });
        bench("长字符串 (超过阈值, 每次 malloc)", [&] {
            for (int i = 0; i < kN; ++i) {
                std::string s = "this string is definitely longer than any SSO buffer";
                (void)s.size();
            }
        });
        std::cout << "    ^^^ 差距就是一次 malloc + free 的成本。\n";
    }

    std::cout <<
        "\n  实用推论：\n"
        "    - 短字符串当值传递/返回是廉价的，别为此改用 const char*\n"
        "    - reserve 对 string 同样有效（大量 += 时）\n"
        "    - string_view 的价值主要在**长**字符串的子串上（短的本来就不分配）\n";
}

// =============================================================================
// 15.6  MyList —— 哨兵节点
//
// 双向链表的实现难点全在「边界处理」：插入第一个元素、删除最后一个元素、
// 空链表……如果 head/tail 是裸指针，每个操作都要写一堆 if (head == nullptr)。
//
// STL 的解法是「哨兵节点」(sentinel / dummy head)：
// 一个不存数据的节点，让链表**永远成环**：
//
//        ┌──────────────────────────────────────┐
//        ↓                                      │
//   ┌─────────┐    ┌───────┐    ┌───────┐      │
//   │ 哨兵    │───►│  1    │───►│  2    │──────┘
//   │ (end()) │◄───│       │◄───│       │
//   └─────────┘    └───────┘    └───────┘
//        ↑
//      begin() = 哨兵->next
//
// 好处：
//   1) 空链表也是合法的环（哨兵自己指自己），**没有** nullptr 分支
//   2) end() 就是哨兵本身，天然满足「end 是尾后位置」且可以 --end()
//   3) 插入/删除只需改 4 个指针，永远不用判断「是否是头/尾」
// =============================================================================

namespace mystl {

template <typename T>
class MyList {
    struct NodeBase {                       // 哨兵只需要指针，不需要 T（T 可能没有默认构造）
        NodeBase* prev = nullptr;
        NodeBase* next = nullptr;
    };
    struct Node : NodeBase {
        T value;
        template <typename... Args>
        explicit Node(Args&&... args) : value(std::forward<Args>(args)...) {}
    };

    NodeBase    sentinel_;                  // 哨兵内嵌在容器里，不需要堆分配
    std::size_t size_ = 0;

public:
    class iterator {
        NodeBase* p_ = nullptr;
        friend class MyList;
    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = T*;
        using reference         = T&;

        iterator() = default;
        explicit iterator(NodeBase* p) : p_(p) {}

        reference operator*()  const { return static_cast<Node*>(p_)->value; }
        pointer   operator->() const { return &static_cast<Node*>(p_)->value; }

        iterator& operator++()    { p_ = p_->next; return *this; }
        iterator  operator++(int) { iterator t = *this; p_ = p_->next; return t; }
        iterator& operator--()    { p_ = p_->prev; return *this; }
        iterator  operator--(int) { iterator t = *this; p_ = p_->prev; return t; }

        bool operator==(const iterator& o) const { return p_ == o.p_; }
        bool operator!=(const iterator& o) const { return p_ != o.p_; }
    };

    MyList() { sentinel_.prev = sentinel_.next = &sentinel_; }   // 空链表 = 哨兵自环

    ~MyList() { clear(); }

    MyList(const MyList& o) : MyList() { for (const auto& x : o) push_back(x); }
    MyList(MyList&& o) noexcept : MyList() { splice_all(o); }
    MyList& operator=(MyList o) noexcept { swap(o); return *this; }

    MyList(std::initializer_list<T> il) : MyList() { for (const auto& x : il) push_back(x); }

    void swap(MyList& o) noexcept {
        // 哨兵是内嵌对象，不能简单交换指针，要重接环
        MyList tmp;
        tmp.splice_all(*this);
        splice_all(o);
        o.splice_all(tmp);
    }

    iterator begin() noexcept { return iterator(sentinel_.next); }
    iterator end()   noexcept { return iterator(&sentinel_); }   // 哨兵即尾后
    iterator begin() const noexcept { return iterator(const_cast<NodeBase*>(sentinel_.next)); }
    iterator end()   const noexcept { return iterator(const_cast<NodeBase*>(&sentinel_)); }

    std::size_t size()  const noexcept { return size_; }
    bool        empty() const noexcept { return size_ == 0; }

    // 所有插入都归结到这一个函数 —— 没有任何边界判断，这就是哨兵的收益
    template <typename... Args>
    iterator emplace(iterator pos, Args&&... args) {
        Node* node = new Node(std::forward<Args>(args)...);
        NodeBase* next = pos.p_;
        NodeBase* prev = next->prev;
        node->prev = prev;
        node->next = next;
        prev->next = node;      // 四次指针赋值，插头插尾插中间完全一样
        next->prev = node;
        ++size_;
        return iterator(node);
    }

    void push_back(const T& x)  { emplace(end(), x); }
    void push_back(T&& x)       { emplace(end(), std::move(x)); }
    void push_front(const T& x) { emplace(begin(), x); }

    // 删除同理，无边界判断
    iterator erase(iterator pos) {
        NodeBase* node = pos.p_;
        assert(node != &sentinel_);
        node->prev->next = node->next;
        node->next->prev = node->prev;
        iterator ret(node->next);
        delete static_cast<Node*>(node);
        --size_;
        return ret;
    }

    void clear() noexcept {
        NodeBase* p = sentinel_.next;
        while (p != &sentinel_) {
            NodeBase* next = p->next;
            delete static_cast<Node*>(p);
            p = next;
        }
        sentinel_.prev = sentinel_.next = &sentinel_;
        size_ = 0;
    }

    // splice：list 独有的杀手级操作 —— O(1) 把另一个链表整体接过来，只改指针
    void splice_all(MyList& o) noexcept {
        if (o.empty()) return;
        NodeBase* first = o.sentinel_.next;
        NodeBase* last  = o.sentinel_.prev;
        NodeBase* at    = &sentinel_;
        NodeBase* prev  = at->prev;

        prev->next  = first;  first->prev = prev;
        last->next  = at;     at->prev    = last;

        size_ += o.size_;
        o.sentinel_.prev = o.sentinel_.next = &o.sentinel_;
        o.size_ = 0;
    }
};

} // namespace mystl

static void s06_list() {
    demo::title("15.6  MyList —— 哨兵节点");

    mystl::MyList<std::string> l{"b", "c"};
    l.push_front("a");
    l.push_back("d");

    std::cout << "  正向遍历: ";
    for (const auto& s : l) std::cout << s << ' ';
    std::cout << "\n  反向遍历: ";
    for (auto it = l.end(); it != l.begin(); ) { --it; std::cout << *it << ' '; }
    std::cout << "\n  size = " << l.size() << "\n";

    // 删除中间元素
    auto it = l.begin(); ++it;
    it = l.erase(it);
    std::cout << "  删掉第二个元素后: ";
    for (const auto& s : l) std::cout << s << ' ';
    std::cout << "\n";

    demo::section("splice：O(1) 转移，list 相对 vector 的真正优势");
    {
        mystl::MyList<int> a{1, 2, 3};
        mystl::MyList<int> b{4, 5, 6};
        a.splice_all(b);      // 不管多少元素，都只改 4 个指针
        std::cout << "  a.splice_all(b) 之后 a = ";
        for (int x : a) std::cout << x << ' ';
        std::cout << "  b.size()=" << b.size() << "\n";
        std::cout << "  100 万个元素也是 O(1) —— vector 做不到（必须逐个搬）\n";
    }

    std::cout <<
        "\n  哨兵节点带来的三个直接好处：\n"
        "    1. emplace / erase 里没有一个 if 判断头尾 —— 上面的代码你可以数一下\n"
        "    2. end() 可以 --（因为哨兵是真实存在的节点），vector 的 end() 也能减，\n"
        "       但裸指针链表的 nullptr 不行\n"
        "    3. 空容器与非空容器走完全相同的代码路径 -> bug 少一半\n"
        "\n"
        "  为什么 list 这么慢（尽管插入是 O(1)）？\n"
        "    每个节点独立分配 -> 内存分散 -> 缓存不友好。\n"
        "    遍历 100 万个 int：vector 约 0.3 ms，list 约 5 ms（十几倍差距）。\n"
        "    所以 list 只在「频繁在中间插删 + 需要迭代器长期有效 + splice」时才划算。\n";
}

// =============================================================================
// 15.7  红黑树 —— map / set 的底层
//
// 为什么不用普通二叉搜索树？因为有序插入会退化成链表：
//   插入 1,2,3,4,5 -> 1→2→3→4→5，查找变 O(n)
//
// 红黑树用 5 条不变式把树高约束在 O(log n)：
//   1. 每个节点是红色或黑色
//   2. 根节点是黑色
//   3. 所有叶子（NIL）是黑色
//   4. 红色节点的两个子节点必须是黑色（不能有连续红节点）
//   5. 从任一节点到其所有后代 NIL 的路径包含**相同数目**的黑节点
//
// 规则 4 + 5 共同保证：最长路径 ≤ 2 × 最短路径，于是树高 ≤ 2·log₂(n+1)。
//
// 为什么用红黑树而不是 AVL 树？
//   AVL 更平衡（查找略快），但插入/删除需要更多旋转来维持严格平衡。
//   红黑树的「近似平衡」让修改操作的旋转次数是 O(1) 均摊，
//   在「读写混合」场景总体更快。所以 STL 选它。
//
// 插入的三种修复情形（新节点 N 为红，父 P 为红时才需要修复）：
//
//   情形 1：叔叔 U 是红色  -> 变色（P、U 变黑，祖父 G 变红），问题上移到 G
//        G(黑)                  G(红)  <- 继续向上检查
//       /    \                 /    \
//     P(红)  U(红)   ====>   P(黑)  U(黑)
//     /                      /
//   N(红)                  N(红)
//
//   情形 2：叔叔黑，N 是「内侧」孙子 -> 先旋转成情形 3
//        G(黑)                    G(黑)
//       /    \                   /    \
//     P(红)  U(黑)   ====>     N(红)  U(黑)      左旋 P
//       \                      /
//       N(红)                P(红)
//
//   情形 3：叔叔黑，N 是「外侧」孙子 -> 旋转 + 变色，修复完成
//        G(黑)                    P(黑)
//       /    \                   /    \
//     P(红)  U(黑)   ====>     N(红)  G(红)      右旋 G
//     /                                  \
//   N(红)                                U(黑)
// =============================================================================

namespace mystl {

template <typename K, typename V, typename Compare = std::less<K>>
class MyRBTree {
    enum Color { kRed, kBlack };

    struct Node {
        K     key;
        V     value;
        Color color = kRed;      // 新节点总是红色（这样不破坏规则 5，只可能破坏规则 4）
        Node* left   = nullptr;
        Node* right  = nullptr;
        Node* parent = nullptr;
        Node(const K& k, const V& v) : key(k), value(v) {}
    };

    Node*       root_ = nullptr;
    std::size_t size_ = 0;
    Compare     comp_{};

    static bool is_red(Node* n) noexcept { return n && n->color == kRed; }

    // 左旋：把 x 的右孩子 y 提上来当父亲
    //     x              y
    //      \            /
    //       y   ==>    x
    //      /            \
    //     b              b
    void rotate_left(Node* x) {
        Node* y  = x->right;
        x->right = y->left;
        if (y->left) y->left->parent = x;
        y->parent = x->parent;
        if (!x->parent)              root_ = y;
        else if (x == x->parent->left)  x->parent->left  = y;
        else                            x->parent->right = y;
        y->left   = x;
        x->parent = y;
    }

    void rotate_right(Node* x) {
        Node* y = x->left;
        x->left = y->right;
        if (y->right) y->right->parent = x;
        y->parent = x->parent;
        if (!x->parent)              root_ = y;
        else if (x == x->parent->right) x->parent->right = y;
        else                            x->parent->left  = y;
        y->right  = x;
        x->parent = y;
    }

    // 插入后修复：只在「父节点也是红色」时需要（连续红违反规则 4）
    void fix_insert(Node* n) {
        while (n != root_ && is_red(n->parent)) {
            Node* parent = n->parent;
            Node* grand  = parent->parent;
            if (!grand) break;

            bool  parent_is_left = (parent == grand->left);
            Node* uncle = parent_is_left ? grand->right : grand->left;

            if (is_red(uncle)) {
                // 情形 1：叔叔红 -> 纯变色，问题上移两层
                parent->color = kBlack;
                uncle->color  = kBlack;
                grand->color  = kRed;
                n = grand;                       // 继续检查祖父
            } else {
                // 情形 2：内侧 -> 先旋转成外侧
                if (parent_is_left && n == parent->right) {
                    rotate_left(parent);
                    n      = parent;
                    parent = n->parent;
                } else if (!parent_is_left && n == parent->left) {
                    rotate_right(parent);
                    n      = parent;
                    parent = n->parent;
                }
                // 情形 3：外侧 -> 旋转祖父 + 变色，结束
                parent->color = kBlack;
                grand->color  = kRed;
                if (parent_is_left) rotate_right(grand);
                else                rotate_left(grand);
                break;
            }
        }
        root_->color = kBlack;                   // 规则 2：根永远黑
    }

    void destroy(Node* n) noexcept {
        if (!n) return;
        destroy(n->left);
        destroy(n->right);
        delete n;
    }

    // 校验函数：验证 5 条不变式确实成立（返回黑高，-1 表示违规）
    int check(Node* n) const {
        if (!n) return 1;                        // NIL 算 1 个黑
        if (is_red(n) && (is_red(n->left) || is_red(n->right))) return -1;  // 规则 4
        int lh = check(n->left);
        int rh = check(n->right);
        if (lh == -1 || rh == -1 || lh != rh) return -1;                    // 规则 5
        return lh + (n->color == kBlack ? 1 : 0);
    }

    void inorder(Node* n, std::vector<std::pair<K, V>>& out) const {
        if (!n) return;
        inorder(n->left, out);
        out.emplace_back(n->key, n->value);
        inorder(n->right, out);
    }

    int height(Node* n) const {
        if (!n) return 0;
        return 1 + std::max(height(n->left), height(n->right));
    }

public:
    MyRBTree() = default;
    ~MyRBTree() { destroy(root_); }
    MyRBTree(const MyRBTree&)            = delete;   // 教学版不实现深拷贝
    MyRBTree& operator=(const MyRBTree&) = delete;

    // 标准 BST 插入 + 修复
    bool insert(const K& key, const V& value) {
        Node* parent = nullptr;
        Node* cur    = root_;
        while (cur) {
            parent = cur;
            if      (comp_(key, cur->key)) cur = cur->left;
            else if (comp_(cur->key, key)) cur = cur->right;
            else { cur->value = value; return false; }   // 键已存在
        }

        Node* n = new Node(key, value);
        n->parent = parent;
        if      (!parent)              root_ = n;
        else if (comp_(key, parent->key)) parent->left  = n;
        else                              parent->right = n;

        ++size_;
        fix_insert(n);
        return true;
    }

    const V* find(const K& key) const {
        Node* cur = root_;
        while (cur) {
            if      (comp_(key, cur->key)) cur = cur->left;
            else if (comp_(cur->key, key)) cur = cur->right;
            else return &cur->value;
        }
        return nullptr;
    }

    std::size_t size()   const noexcept { return size_; }
    bool        valid()  const { return check(root_) != -1; }
    int         height() const { return height(root_); }

    std::vector<std::pair<K, V>> to_sorted_vector() const {
        std::vector<std::pair<K, V>> out;
        out.reserve(size_);
        inorder(root_, out);          // 中序遍历 BST = 有序序列
        return out;
    }
};

} // namespace mystl

static void s07_rbtree() {
    demo::title("15.7  红黑树（map / set 的底层）");

    demo::section("最坏情况：有序插入 —— 普通 BST 会退化成链表");
    {
        mystl::MyRBTree<int, std::string> tree;
        constexpr int kN = 1000;
        for (int i = 0; i < kN; ++i) tree.insert(i, "v" + std::to_string(i));

        std::cout << "  顺序插入 " << kN << " 个键（对普通 BST 是最坏输入）\n";
        std::cout << "    实际树高       = " << tree.height() << "\n";
        std::cout << "    理论下界 log2(n) = " << static_cast<int>(std::log2(kN)) << "\n";
        std::cout << "    红黑树上界 2*log2(n+1) = "
                  << static_cast<int>(2 * std::log2(kN + 1)) << "\n";
        std::cout << "    普通 BST 会是   = " << kN << "（退化成链表）\n";
        std::cout << "    5 条不变式全部成立: " << std::boolalpha << tree.valid() << "\n";
    }

    demo::section("功能验证：中序遍历自动有序");
    {
        mystl::MyRBTree<std::string, int> t;
        for (const char* k : {"pear", "apple", "cherry", "banana", "date"})
            t.insert(k, static_cast<int>(std::strlen(k)));

        std::cout << "  乱序插入，中序遍历结果: ";
        for (const auto& [k, v] : t.to_sorted_vector()) std::cout << k << "(" << v << ") ";
        std::cout << "\n  这就是 std::map 为什么天然有序 —— 它就是中序遍历。\n";

        const int* p = t.find("cherry");
        std::cout << "  find(\"cherry\") -> " << (p ? std::to_string(*p) : "未找到") << "\n";
        std::cout << "  find(\"grape\")  -> " << (t.find("grape") ? "找到" : "未找到") << "\n";
    }

    demo::section("随机插入下的树高");
    {
        mystl::MyRBTree<int, int> t;
        std::mt19937 gen(42);
        std::vector<int> keys(10000);
        std::iota(keys.begin(), keys.end(), 0);
        std::shuffle(keys.begin(), keys.end(), gen);
        for (int k : keys) t.insert(k, k);
        std::cout << "  随机插入 10000 个键：树高=" << t.height()
                  << "  不变式成立=" << t.valid() << "\n";
    }

    std::cout <<
        "\n  为什么删除比插入难得多（本实现只做了插入）？\n"
        "    插入的新节点是红色，最多只破坏规则 4（连续红），情形只有 3 种。\n"
        "    删除黑节点会破坏规则 5（黑高不等），要「借」一个黑节点回来，\n"
        "    情形有 4 种且需要区分兄弟节点的孩子颜色，代码量是插入的 2~3 倍。\n"
        "    这也是为什么很多人手写红黑树只写插入。\n"
        "\n"
        "  map/set 的实际后果：\n"
        "    - 节点独立分配 -> 缓存不友好，实测比 unordered_map 慢 2~5 倍\n"
        "    - 但有序、迭代器稳定（插删不失效）、支持 lower_bound 范围查询\n"
        "    - 小数据量（< 几十个）时 sorted vector 或 flat_map 更快\n";
}

// =============================================================================
// 15.8  MyHashTable —— unordered_map 的底层
//
// 结构：桶数组 + 链地址法（separate chaining）
//
//   buckets_
//   ┌───┐
//   │ 0 │──► ("cat",1) ──► ("act",9) ──► nullptr     哈希冲突时挂同一条链
//   ├───┤
//   │ 1 │──► nullptr
//   ├───┤
//   │ 2 │──► ("dog",2) ──► nullptr
//   └───┘
//
// 三个关键设计决策：
//
//   1) 桶数怎么选？
//      libstdc++ / MSVC 用**质数**（如 13, 29, 59, 127...），
//      因为取模质数能更好地打散有规律的哈希值。
//      有些实现用 2 的幂（可以用位与代替取模，更快），代价是要求哈希函数质量更高。
//
//   2) 负载因子 (load_factor = 元素数 / 桶数)
//      默认上限 1.0。超过就 rehash（桶数翻倍并重新分配所有元素）。
//      负载因子越小越快但越浪费内存。
//
//   3) 为什么 rehash 会让**迭代器**失效但**引用/指针**不失效？
//      因为 rehash 只是把节点重新挂到不同的桶上，**节点本身没有移动**。
//      迭代器要靠桶索引来遍历，桶变了就失效；但指向节点里 value 的
//      指针和引用依然有效。这是 unordered_map 与 vector 的重要区别。
// =============================================================================

namespace mystl {

template <typename K, typename V, typename Hash = std::hash<K>>
class MyHashTable {
    struct Node {
        K     key;
        V     value;
        std::size_t hash;      // 缓存哈希值：rehash 时不用重新计算（重要优化）
        Node* next = nullptr;
        Node(const K& k, const V& v, std::size_t h) : key(k), value(v), hash(h) {}
    };

    std::vector<Node*> buckets_;
    std::size_t        size_ = 0;
    float              max_load_factor_ = 1.0f;
    Hash               hasher_{};
    std::size_t        rehash_count_ = 0;

    // 质数序列：每次 rehash 取下一个「大于当前两倍」的质数
    static std::size_t next_prime(std::size_t n) {
        static const std::size_t primes[] = {
            13, 29, 59, 127, 257, 541, 1109, 2357, 5087, 10273,
            20753, 42043, 85229, 172933, 351061, 712697, 1447153};
        for (std::size_t p : primes) if (p > n) return p;
        return n * 2 + 1;
    }

    std::size_t bucket_index(std::size_t h) const { return h % buckets_.size(); }

public:
    explicit MyHashTable(std::size_t initial = 13) : buckets_(next_prime(initial - 1), nullptr) {}

    ~MyHashTable() { clear(); }
    MyHashTable(const MyHashTable&)            = delete;
    MyHashTable& operator=(const MyHashTable&) = delete;

    void clear() noexcept {
        for (Node*& head : buckets_) {
            while (head) { Node* n = head->next; delete head; head = n; }
        }
        size_ = 0;
    }

    bool insert(const K& key, const V& value) {
        std::size_t h  = hasher_(key);
        std::size_t bi = bucket_index(h);

        // 先查重：注意先比 hash 再比 key —— 比较哈希是整数比较，远快于比较字符串
        for (Node* n = buckets_[bi]; n; n = n->next) {
            if (n->hash == h && n->key == key) { n->value = value; return false; }
        }

        Node* n = new Node(key, value, h);
        n->next = buckets_[bi];          // 头插：O(1)，不用遍历到链尾
        buckets_[bi] = n;
        ++size_;

        if (load_factor() > max_load_factor_) rehash(next_prime(buckets_.size()));
        return true;
    }

    V* find(const K& key) {
        std::size_t h = hasher_(key);
        for (Node* n = buckets_[bucket_index(h)]; n; n = n->next) {
            if (n->hash == h && n->key == key) return &n->value;
        }
        return nullptr;
    }

    bool erase(const K& key) {
        std::size_t h  = hasher_(key);
        std::size_t bi = bucket_index(h);
        Node** pp = &buckets_[bi];       // 用「指向指针的指针」消除「是否为头节点」的分支
        while (*pp) {
            if ((*pp)->hash == h && (*pp)->key == key) {
                Node* dead = *pp;
                *pp = dead->next;
                delete dead;
                --size_;
                return true;
            }
            pp = &(*pp)->next;
        }
        return false;
    }

    void rehash(std::size_t new_bucket_count) {
        ++rehash_count_;
        std::vector<Node*> new_buckets(new_bucket_count, nullptr);
        for (Node* head : buckets_) {
            while (head) {
                Node* next = head->next;
                // 复用缓存的哈希值 —— 不必重新调用 hasher_，这是 hash 字段的价值
                std::size_t bi = head->hash % new_bucket_count;
                head->next = new_buckets[bi];
                new_buckets[bi] = head;      // 节点本身没有移动，只是换了挂载点
                head = next;
            }
        }
        buckets_.swap(new_buckets);
    }

    std::size_t size()         const noexcept { return size_; }
    std::size_t bucket_count() const noexcept { return buckets_.size(); }
    std::size_t rehashes()     const noexcept { return rehash_count_; }
    float load_factor() const noexcept {
        return static_cast<float>(size_) / static_cast<float>(buckets_.size());
    }

    // 诊断：链长分布。理想情况下大部分桶的链长是 0 或 1
    void print_distribution() const {
        std::map<std::size_t, std::size_t> hist;
        std::size_t longest = 0;
        for (Node* head : buckets_) {
            std::size_t len = 0;
            for (Node* n = head; n; n = n->next) ++len;
            ++hist[len];
            longest = std::max(longest, len);
        }
        std::cout << "    桶数=" << buckets_.size() << " 元素数=" << size_
                  << " 负载因子=" << std::fixed << std::setprecision(2) << load_factor()
                  << " 最长链=" << longest << "\n";
        std::cout << "    链长分布: ";
        for (const auto& [len, cnt] : hist) std::cout << len << "->" << cnt << "桶  ";
        std::cout << "\n";
    }
};

} // namespace mystl

static void s08_hashtable() {
    demo::title("15.8  MyHashTable（unordered_map 的底层）");

    demo::section("插入 + rehash 轨迹");
    {
        mystl::MyHashTable<std::string, int> ht;
        std::size_t last_bc = ht.bucket_count();
        std::cout << "  桶数变化: " << last_bc << ' ';
        for (int i = 0; i < 300; ++i) {
            ht.insert("key" + std::to_string(i), i);
            if (ht.bucket_count() != last_bc) {
                std::cout << "-> " << ht.bucket_count() << ' ';
                last_bc = ht.bucket_count();
            }
        }
        std::cout << "\n  共 rehash " << ht.rehashes() << " 次（都是质数）\n";
        ht.print_distribution();
    }

    demo::section("查找 / 删除正确性");
    {
        mystl::MyHashTable<std::string, int> ht;
        ht.insert("apple", 1);
        ht.insert("banana", 2);
        ht.insert("apple", 100);                 // 覆盖已有键
        std::cout << "  find(\"apple\")  -> " << *ht.find("apple") << "（被覆盖为 100）\n";
        std::cout << "  find(\"grape\")  -> " << (ht.find("grape") ? "找到" : "nullptr") << "\n";
        std::cout << "  erase(\"banana\")-> " << std::boolalpha << ht.erase("banana")
                  << "  size=" << ht.size() << "\n";
    }

    demo::section("哈希函数质量的影响（故意用一个坏哈希）");
    {
        struct BadHash {   // 所有键都返回同一个值 -> 全部冲突，退化成单链表
            std::size_t operator()(const std::string&) const { return 42; }
        };
        mystl::MyHashTable<std::string, int, BadHash> bad;
        for (int i = 0; i < 100; ++i) bad.insert("k" + std::to_string(i), i);
        std::cout << "  坏哈希（恒返回 42）：\n";
        bad.print_distribution();
        std::cout << "    ^^^ 所有元素挂在一条链上，查找退化成 O(n)\n";

        mystl::MyHashTable<std::string, int> good;
        for (int i = 0; i < 100; ++i) good.insert("k" + std::to_string(i), i);
        std::cout << "  标准 std::hash：\n";
        good.print_distribution();
    }

    std::cout <<
        "\n  自定义类型做键必须提供哈希（两种方式）：\n"
        "\n"
        "    // 方式 1：特化 std::hash（推荐，用起来最自然）\n"
        "    struct Point { int x, y; bool operator==(const Point&) const = default; };\n"
        "    template <> struct std::hash<Point> {\n"
        "        size_t operator()(const Point& p) const noexcept {\n"
        "            size_t h = std::hash<int>{}(p.x);\n"
        "            // 组合哈希：别用简单 XOR（(1,2) 和 (2,1) 会撞）\n"
        "            h ^= std::hash<int>{}(p.y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);\n"
        "            return h;\n"
        "        }\n"
        "    };\n"
        "    std::unordered_map<Point, int> m;   // 现在可以用了\n"
        "\n"
        "    // 方式 2：作为模板参数传入（不侵入 std 命名空间）\n"
        "    std::unordered_map<Point, int, PointHash> m2;\n"
        "\n"
        "  那个 0x9e3779b9 是黄金比例的 32 位定点表示，boost::hash_combine 就用它，\n"
        "  作用是让每一位的影响均匀扩散到整个哈希值。\n";
}

// =============================================================================
// 15.9  deque —— 分段连续与中控器
// =============================================================================
static void s09_deque() {
    demo::title("15.9  deque 的结构");

    std::cout <<
        "  deque 既要「两端 O(1) 插删」又要「O(1) 随机访问」，\n"
        "  所以它不是单块连续内存，而是「分段连续」：\n"
        "\n"
        "    map_（中控器，本身是一个 T** 数组）\n"
        "    ┌────┬────┬────┬────┐\n"
        "    │ p0 │ p1 │ p2 │ p3 │\n"
        "    └─┬──┴─┬──┴─┬──┴─┬──┘\n"
        "      ↓    ↓    ↓    ↓\n"
        "    [缓冲区][缓冲区][缓冲区][缓冲区]     每块固定 512 字节 / sizeof(T) 个元素\n"
        "       ↑                        ↑\n"
        "     start                     finish\n"
        "\n"
        "  于是：\n"
        "    push_front  -> 在首块前面留的空位里放，满了就新分配一块挂到中控器前端\n"
        "    operator[i] -> 先算 i 落在第几块（i / 块大小），再算块内偏移（i % 块大小）\n"
        "                   两次寻址，所以比 vector 的 [] 慢，但仍是 O(1)\n"
        "\n"
        "  迭代器要存 4 个指针（当前元素、当前块首、当前块尾、指向中控器的位置），\n"
        "  所以 deque 的迭代器比 vector 的裸指针「重」得多，遍历也更慢。\n";

    demo::section("实测：两端插入 vector vs deque");
    {
        constexpr int kN = 60000;
        auto bench = [](const char* tag, auto&& fn) {
            auto t0 = std::chrono::steady_clock::now();
            fn();
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            std::cout << "    " << demo::pad(tag, 34) << us << " us\n";
        };
        bench("vector push_back", [&] {
            std::vector<int> v; for (int i = 0; i < kN; ++i) v.push_back(i);
        });
        bench("deque  push_back", [&] {
            std::deque<int> d; for (int i = 0; i < kN; ++i) d.push_back(i);
        });
        bench("vector insert(begin) 头插", [&] {
            std::vector<int> v; for (int i = 0; i < kN / 10; ++i) v.insert(v.begin(), i);
        });
        bench("deque  push_front 头插", [&] {
            std::deque<int> d; for (int i = 0; i < kN / 10; ++i) d.push_front(i);
        });
        std::cout << "    ^^^ 头插差距是 O(n²) vs O(n)（注意 vector 只做了 1/10 的量）\n";
    }

    std::cout <<
        "\n  重要细节：deque 的 push_back/push_front **不会**使引用和指针失效\n"
        "  （因为已有的块不动），但**会**使迭代器失效（中控器可能重新分配）。\n"
        "  这和 vector「全都失效」、list「全都不失效」形成三档差异。\n";
}

// =============================================================================
// 15.10  std::sort 为什么是「内省排序」(introsort)
// =============================================================================
namespace mystl {

// 简化版 introsort，展示三种算法怎么配合
template <typename It>
void introsort_impl(It first, It last, int depth_limit) {
    constexpr int kSmallThreshold = 16;

    while (last - first > kSmallThreshold) {
        if (depth_limit == 0) {
            // 递归太深（说明快排在这份数据上退化了）-> 切换堆排序保底 O(n log n)
            std::make_heap(first, last);
            std::sort_heap(first, last);
            return;
        }
        --depth_limit;

        // 三点取中：取首、中、尾的中位数做 pivot，避免有序数据退化
        It mid = first + (last - first) / 2;
        It a = first, b = mid, c = last - 1;
        if (*b < *a) std::iter_swap(a, b);
        if (*c < *b) { std::iter_swap(b, c); if (*b < *a) std::iter_swap(a, b); }
        auto pivot = *b;

        // Hoare 分区
        It i = first, j = last - 1;
        while (i <= j) {
            while (*i < pivot) ++i;
            while (pivot < *j) --j;
            if (i <= j) { std::iter_swap(i, j); ++i; --j; }
        }
        // 递归处理较小的一半，循环处理较大的一半 -> 栈深度 O(log n)
        introsort_impl(first, j + 1, depth_limit);
        first = i;
    }
    // 收尾：小区间用插入排序（元素少时它比快排快，且对「几乎有序」极快）
    for (It it = first + (first == last ? 0 : 1); it != last; ++it) {
        auto val = *it;
        It   pos = it;
        while (pos != first && val < *(pos - 1)) { *pos = *(pos - 1); --pos; }
        *pos = val;
    }
}

template <typename It>
void introsort(It first, It last) {
    if (first == last) return;
    // 深度上限 2*log2(n)：超过就认为快排退化了
    int depth = 2 * static_cast<int>(std::log2(static_cast<double>(last - first)));
    introsort_impl(first, last, depth);
}

} // namespace mystl

static void s10_sort() {
    demo::title("15.10  std::sort = 内省排序");

    std::cout <<
        "  三种排序算法各有致命弱点，introsort 把它们组合起来互相补位：\n"
        "\n"
        "    快速排序   平均最快（缓存友好、常数小），但最坏 O(n²)\n"
        "    堆排序     稳定 O(n log n) 保底，但常数大、缓存不友好\n"
        "    插入排序   小数据和「几乎有序」时最快，但一般情况 O(n²)\n"
        "\n"
        "  introsort 的策略：\n"
        "    1. 主体用快排（三点取中选 pivot，抗有序输入）\n"
        "    2. 递归深度超过 2*log2(n) -> 判定快排退化，切堆排序保底\n"
        "    3. 区间缩小到 ≤ 16 个元素 -> 停止递归，最后统一做一次插入排序\n"
        "\n"
        "  于是最坏情况也是 O(n log n)，平均情况保持快排的速度。\n"
        "  这个算法由 David Musser 在 1997 年提出，此后成为所有 STL 实现的标准做法。\n";

    demo::section("正确性 + 性能对照");
#ifndef NDEBUG
    std::cout <<
        "  （Debug 构建下手写版慢于 std::sort，是因为标准库经过大量特化与\n"
        "    内联优化，而 Debug 关闭了这些优化。这里要看的结论是\n"
        "    「三种输入下都没有退化成 O(n^2)」。）\n";
#endif
    {
        std::mt19937 gen(12345);
        auto make_data = [&](int n, int mode) {
            std::vector<int> v(n);
            std::iota(v.begin(), v.end(), 0);
            if (mode == 0) std::shuffle(v.begin(), v.end(), gen);      // 随机
            else if (mode == 2) std::reverse(v.begin(), v.end());      // 完全逆序
            // mode == 1: 保持完全有序
            return v;
        };
        const char* names[] = {"随机数据", "已排序（快排最坏输入之一）", "完全逆序"};

        for (int mode = 0; mode < 3; ++mode) {
            auto a = make_data(200000, mode);
            auto b = a;

            auto t0 = std::chrono::steady_clock::now();
            mystl::introsort(a.begin(), a.end());
            auto t1 = std::chrono::steady_clock::now();
            std::sort(b.begin(), b.end());
            auto t2 = std::chrono::steady_clock::now();

            auto us = [](auto d) {
                return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
            };
            std::cout << "    " << demo::pad(names[mode], 32)
                      << "手写 introsort " << std::setw(7) << us(t1 - t0)
                      << "us   std::sort " << std::setw(7) << us(t2 - t1)
                      << "us   结果一致=" << std::boolalpha
                      << (a == b && std::is_sorted(a.begin(), a.end())) << "\n";
        }
    }

    std::cout <<
        "\n  相关算法的选择：\n"
        "    std::sort          不稳定，O(n log n)，原地。默认选它\n"
        "    std::stable_sort   稳定（相等元素保持原序），需要 O(n) 额外内存，归并排序\n"
        "    std::partial_sort  只要前 k 个 -> 堆排序，O(n log k)\n"
        "    std::nth_element   只要第 k 个就位 -> 快速选择，平均 O(n)\n"
        "\n"
        "  「只要前 10 名」千万别 sort 全部再取前 10 —— 用 partial_sort。\n";
}

// =============================================================================
// 15.11  性能实测：容器选型的量化依据
// =============================================================================
static void s11_benchmark() {
    demo::title("15.11  容器性能实测");

#ifndef NDEBUG
    std::cout <<
        "  【注意】当前是 Debug 构建。MSVC 的 Debug 开启了迭代器调试检查\n"
        "  (_ITERATOR_DEBUG_LEVEL=2)，标准库容器每次访问都带边界校验，\n"
        "  绝对耗时会放大 5~30 倍。**量级关系仍然可靠，绝对值不可当基准。**\n"
        "  要看真实数字请用 Release：\n"
        "      cmake --build build --config Release\n"
        "      build\\bin\\Release\\ch15_stl_source.exe\n";
#endif

    constexpr int kN = 200000;

    // 计时取「多次运行的最小值」，不是单次也不是平均值。
    // 理由：操作系统调度、其它进程抢 CPU、缓存冷启动只会让某次运行**变慢**，
    //       不可能让它变快，所以最小值最接近「这段代码本身的成本」。
    //       用平均值的话，一次调度抖动就能让结论反过来 —— 开发本章时
    //       就遇到过：同一份代码两次运行，快慢结论正好相反。
    //       Google Benchmark 等基准库都用最小值/中位数，不用平均值。
    constexpr int kRepeat = 5;
    auto bench = [](const char* tag, auto&& fn) {
        long long best = (std::numeric_limits<long long>::max)();
        long long sink = 0;
        for (int r = 0; r < kRepeat; ++r) {
            auto t0 = std::chrono::steady_clock::now();
            sink = static_cast<long long>(fn());
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();
            best = (std::min)(best, us);
        }
        std::cout << "    " << demo::pad(tag, 40)
                  << std::right << std::setw(8) << best << " us   (校验和 " << sink << ")\n";
        return best;
    };

    demo::section("1) 顺序插入 " + std::to_string(kN) + " 个 int");
    bench("vector push_back", [&] {
        std::vector<int> v; for (int i = 0; i < kN; ++i) v.push_back(i); return v.size();
    });
    bench("vector push_back + reserve", [&] {
        std::vector<int> v; v.reserve(kN);
        for (int i = 0; i < kN; ++i) v.push_back(i); return v.size();
    });
    bench("deque push_back", [&] {
        std::deque<int> d; for (int i = 0; i < kN; ++i) d.push_back(i); return d.size();
    });
    bench("list push_back", [&] {
        std::list<int> l; for (int i = 0; i < kN; ++i) l.push_back(i); return l.size();
    });

    demo::section("2) 遍历求和（缓存局部性的威力）");
    {
        std::vector<int> v(kN); std::iota(v.begin(), v.end(), 0);
        std::deque<int>  d(v.begin(), v.end());
        std::list<int>   l(v.begin(), v.end());
        auto vt = bench("vector 遍历", [&] { return std::accumulate(v.begin(), v.end(), 0LL); });
        auto dt = bench("deque  遍历", [&] { return std::accumulate(d.begin(), d.end(), 0LL); });
        auto lt = bench("list   遍历", [&] { return std::accumulate(l.begin(), l.end(), 0LL); });
        if (vt > 0) {
            std::cout << "    ^^^ deque 约 " << (double)dt / (double)vt << "x，"
                      << "list 约 " << (double)lt / (double)vt << "x 于 vector\n";
            std::cout << "    原因：vector 元素连续，一次缓存行加载 16 个 int；\n"
                      << "          list 每个节点独立分配，几乎每次访问都是缓存未命中。\n";
        }
    }

    demo::section("3) 关联容器：插入 + 查找 100000 个字符串键");
    {
        constexpr int kM = 100000;
        std::vector<std::string> keys(kM);
        for (int i = 0; i < kM; ++i) keys[i] = "key_" + std::to_string(i * 7919);

        std::map<std::string, int>           m;
        std::unordered_map<std::string, int> um;

        bench("map 插入（红黑树）", [&] {
            for (int i = 0; i < kM; ++i) m[keys[i]] = i; return m.size();
        });
        bench("unordered_map 插入（哈希）", [&] {
            for (int i = 0; i < kM; ++i) um[keys[i]] = i; return um.size();
        });
        bench("unordered_map 插入 + reserve", [&] {
            std::unordered_map<std::string, int> u; u.reserve(kM);
            for (int i = 0; i < kM; ++i) u[keys[i]] = i; return u.size();
        });
        bench("map 查找", [&] {
            long long s = 0; for (const auto& k : keys) s += m.find(k)->second; return s;
        });
        bench("unordered_map 查找", [&] {
            long long s = 0; for (const auto& k : keys) s += um.find(k)->second; return s;
        });
        bench("有序 vector + binary_search 查找", [&] {
            std::vector<std::pair<std::string, int>> sv;
            sv.reserve(kM);
            for (int i = 0; i < kM; ++i) sv.emplace_back(keys[i], i);
            std::sort(sv.begin(), sv.end());
            long long s = 0;
            for (const auto& k : keys) {
                auto it = std::lower_bound(sv.begin(), sv.end(), k,
                    [](const auto& p, const std::string& key) { return p.first < key; });
                s += it->second;
            }
            return s;
        });
    }

    std::cout << "\n  上面每项都是 " << kRepeat << " 次运行取最小值。\n";
    std::cout <<
        "  本机 Release 实测（仅看量级，绝对值随机器变化）：\n"
        "    vector 遍历比 list 快 20~25 倍        <- 缓存局部性\n"
        "    vector 加 reserve 比不加快 5~6 倍     <- 省掉反复搬移\n"
        "    unordered_map 查找比 map 快 4~5 倍    <- O(1) vs O(log n)\n"
        "\n"
        "  一个诚实的反例：unordered_map 的 reserve 在 MSVC 上**没变快**，\n"
        "  甚至更慢（本机稳定慢 2.7 倍）。MSVC 的 unordered_map 每个桶存两个\n"
        "  迭代器（比 libstdc++ 的单指针桶重一倍），reserve 会一次性分配很大的\n"
        "  桶数组，这笔开销抵消了省下的 rehash；libstdc++ 上则是明显收益。\n"
        "  -> reserve 对 vector 是无脑收益；对 unordered_map 要按你用的\n"
        "     标准库实测，别照抄结论。\n";

    std::cout <<
        "\n  选型结论（数字会随机器变化，量级关系稳定）：\n"
        "    - 默认用 vector。90% 的场景它就是最优解\n"
        "    - 知道元素个数就 reserve —— 这是最廉价的优化\n"
        "    - 需要键值查找且不要求有序 -> unordered_map（reserve 收益看实现）\n"
        "    - 需要有序遍历 / 范围查询 / 迭代器稳定 -> map\n"
        "    - 元素少（几十个）-> 有序 vector 往往比两者都快（缓存友好 + 无节点开销）\n"
        "    - list 只在「频繁中间插删 + splice + 迭代器必须稳定」时才用\n";
}

// =============================================================================
int main() {
    demo::title("第 15 章  STL 源码剖析");
    std::cout <<
        "  手写实现的组件：PoolAllocator / MyVector / MyString(SSO) /\n"
        "                  MyList(哨兵) / MyRBTree / MyHashTable / introsort\n"
        "  每个都会与标准库对照验证功能和性能。\n";

    s01_philosophy();
    s02_iterators();
    s03_allocator();
    s04_vector();
    s05_string_sso();
    s06_list();
    s07_rbtree();
    s08_hashtable();
    s09_deque();
    s10_sort();
    s11_benchmark();

    demo::title("本章要点");
    std::cout <<
        "   1. 容器/算法/迭代器三者解耦，代价是 std::remove 删不掉元素\n"
        "   2. iterator_traits + tag dispatch 是「编译期分派」的经典实现，\n"
        "      C++17 用 if constexpr、C++20 用 concepts 取代它\n"
        "   3. allocator 把「分配内存」和「构造对象」分开 -> reserve 才能不构造对象\n"
        "   4. vector 2 倍扩容 = 均摊 O(1)；移动构造必须 noexcept 否则扩容退化成深拷贝\n"
        "   5. string 用 union 实现 SSO，短字符串零堆分配\n"
        "   6. list 的哨兵节点消灭了所有边界判断\n"
        "   7. 红黑树用 5 条不变式把树高压到 2*log2(n)，插入 3 种修复情形\n"
        "   8. 哈希表 = 桶数组 + 链地址法 + 质数桶数 + 缓存哈希值；rehash 使迭代器\n"
        "      失效但引用不失效\n"
        "   9. std::sort = 快排 + 堆排保底 + 插排收尾（introsort）\n"
        "  10. 缓存局部性常常比算法复杂度更决定实际性能\n";
    return 0;
}
