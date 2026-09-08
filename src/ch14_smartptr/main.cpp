// =============================================================================
// 第 14 章 —— 智能指针深入（用法 + 原理 + 手写实现）
//
// 这一章分三部分：
//   Part A  为什么需要智能指针（裸指针的四种失败模式）
//   Part B  三种智能指针的完整用法与内部结构
//   Part C  从零手写 unique_ptr / shared_ptr / weak_ptr，看懂每一行
//
// 核心思想只有一句：**把「资源的生命周期」绑定到「对象的生命周期」上**。
// 对象析构是编译器保证一定会发生的事（正常返回、提前 return、抛异常都会），
// 所以把释放动作放进析构函数，就等于让编译器帮你管资源。这就是 RAII。
//
// 运行： ch14_smartptr.exe
// =============================================================================

#include "demo.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <stdexcept>
#include <cstddef>
#include <type_traits>

// =============================================================================
// Part A —— 为什么需要智能指针
// =============================================================================

// 一个会打印生命周期的类，方便观察
struct Tracked {
    std::string name;
    explicit Tracked(std::string n) : name(std::move(n)) {
        std::cout << "      [+] 构造 " << name << "\n";
    }
    ~Tracked() { std::cout << "      [-] 析构 " << name << "\n"; }
    void hello() const { std::cout << "      " << name << " 说你好\n"; }
};

// -----------------------------------------------------------------------------
// 裸指针的四种失败模式
// -----------------------------------------------------------------------------
static void partA_why() {
    demo::title("Part A —— 裸指针的四种失败模式");

    demo::section("失败 1：忘记 delete（内存泄漏）");
    std::cout <<
        "    void f() {\n"
        "        Tracked* p = new Tracked(\"leak\");\n"
        "        // ... 一百行代码 ...\n"
        "    }   // 忘了 delete p; -> 泄漏\n"
        "\n"
        "  单次泄漏 32 字节看着无害，但如果这个函数在事件循环里\n"
        "  每秒调用一千次，一天就是 2.7 GB。\n";

    demo::section("失败 2：提前 return / 抛异常，跳过了 delete");
    std::cout <<
        "    void f() {\n"
        "        Tracked* p = new Tracked(\"x\");\n"
        "        if (!check()) return;            // 泄漏路径 1\n"
        "        may_throw();                     // 泄漏路径 2\n"
        "        delete p;                        // 只有「一切顺利」才走到这里\n"
        "    }\n"
        "\n"
        "  一个函数有 N 个出口，你就要写 N 处 delete，还不能漏。\n"
        "  异常路径根本无法用 delete 覆盖 —— 这是 RAII 出现的直接原因。\n";

    demo::section("失败 3：重复 delete（堆损坏）");
    std::cout <<
        "    delete p;\n"
        "    delete p;   // 第二次是 UB，通常直接崩在堆管理器里\n"
        "\n"
        "  更隐蔽的版本：两个模块都以为自己拥有这块内存，各 delete 一次。\n"
        "  「谁负责释放」这个问题，在裸指针的类型里完全没有体现。\n";

    demo::section("失败 4：悬垂指针（use-after-free）");
    std::cout <<
        "    Tracked* p = new Tracked(\"x\");\n"
        "    delete p;\n"
        "    p->hello();   // UB。内存可能已经被别的对象占用，\n"
        "                  // 于是你「以为在读 Tracked，实际读到别人的数据」\n"
        "\n"
        "  这类 bug 最难查：崩溃点离出错点很远，且 Debug 下常常「正常」。\n";

    demo::section("智能指针的解法");
    std::cout <<
        "  不是「自动垃圾回收」，而是把所有权写进**类型**里：\n"
        "\n"
        "    unique_ptr<T>   我独占它，我死它就死。不可拷贝，只能移动。\n"
        "    shared_ptr<T>   大家共享，最后一个走的人关灯（引用计数）。\n"
        "    weak_ptr<T>     我只是看着它，不影响它的生死（可检测是否还活着）。\n"
        "    T*  / T&        我只是「用」它，所有权在别处（函数参数首选）。\n"
        "\n"
        "  于是「谁负责释放」从「靠文档和记忆」变成「编译器检查」。\n";

    // 实际对比
    demo::section("实测：异常路径下的表现");
    auto with_raw = [] {
        std::cout << "    裸指针 + 异常：\n";
        // volatile 让编译器无法在编译期证明这个分支必然成立，
        // 于是 delete 那行不会被判成「无法访问的代码」——
        // 这样反面教材才能完整保留在代码里给你看。
        volatile bool something_went_wrong = true;
        try {
            Tracked* p = new Tracked("裸指针对象");
            if (something_went_wrong) throw std::runtime_error("出错了");
            delete p;   // 运行时永远到不了这里 —— 这就是泄漏的成因
        } catch (const std::exception&) {
            std::cout << "      捕获异常。注意上面**没有**析构输出 -> 泄漏了\n";
        }
    };
    auto with_smart = [] {
        std::cout << "    unique_ptr + 异常：\n";
        try {
            auto p = std::make_unique<Tracked>("智能指针对象");
            throw std::runtime_error("出错了");
        } catch (const std::exception&) {
            std::cout << "      捕获异常。析构在抛出时（栈展开）就已经执行了\n";
        }
    };
    with_raw();
    with_smart();
}

// =============================================================================
// Part B —— 三种智能指针的完整用法
// =============================================================================

// -----------------------------------------------------------------------------
// B1. unique_ptr —— 独占所有权，零额外开销
// -----------------------------------------------------------------------------
// 内部就是一个裸指针（无删除器时 sizeof 与裸指针相同），所有操作都在编译期
// 内联掉。**没有任何运行时代价**，所以「默认就该用它」，不要因为怕性能而用裸指针。
static void partB1_unique_ptr() {
    demo::title("Part B1 —— unique_ptr");

    demo::section("大小验证：零开销");
    std::cout << "  sizeof(Tracked*)                = " << sizeof(Tracked*) << "\n";
    std::cout << "  sizeof(unique_ptr<Tracked>)     = " << sizeof(std::unique_ptr<Tracked>)
              << "   <== 完全一样\n";
    // 带「有状态删除器」时会变大，因为要存删除器对象
    auto lambda_deleter = [](Tracked* p) { delete p; };
    std::cout << "  带 lambda 删除器的 unique_ptr    = "
              << sizeof(std::unique_ptr<Tracked, decltype(lambda_deleter)>)
              << "   <== 无捕获 lambda 是空类，编译器做了空基类优化\n";

    demo::section("创建与访问");
    {
        auto p = std::make_unique<Tracked>("u1");   // 首选：一次分配，异常安全
        p->hello();
        (*p).hello();
        std::cout << "  p.get()   = " << static_cast<const void*>(p.get())
                  << "   （拿裸指针，但不转移所有权）\n";
        std::cout << "  bool(p)   = " << static_cast<bool>(p) << "\n";
    }   // 离开作用域自动析构

    demo::section("移动语义：所有权转移");
    {
        auto a = std::make_unique<Tracked>("u2");
        // auto b = a;                  // 编译错误：拷贝构造被 delete 了
        auto b = std::move(a);          // 正确：所有权从 a 转到 b
        std::cout << "  move 后 a 为空: " << (a == nullptr)
                  << "   （标准保证移动后源指针为 nullptr）\n";
        b->hello();
    }

    demo::section("release / reset —— 手动干预");
    {
        auto p = std::make_unique<Tracked>("u3");

        Tracked* raw = p.release();   // 放弃所有权，返回裸指针；**你现在要负责 delete**
        std::cout << "  release 后 p 为空: " << (p == nullptr) << "\n";
        delete raw;                   // 必须手动删，release 的唯一正当用途是交给 C API

        p.reset(new Tracked("u4"));   // 释放旧的（如果有），接管新的
        p.reset();                    // 等价于 p = nullptr，立即释放
        std::cout << "  reset() 后 p 为空: " << (p == nullptr) << "\n";
    }

    demo::section("自定义删除器 —— 管理任何资源，不只是 new 出来的内存");
    {
        // 场景 1：FILE* 要用 fclose 关闭
        //   删除器类型是 unique_ptr 模板参数的一部分，所以类型里带着 fclose
        auto file_closer = [](std::FILE* f) {
            if (f) {
                std::cout << "      [-] fclose 关闭文件\n";
                std::fclose(f);
            }
        };
        {
            std::unique_ptr<std::FILE, decltype(file_closer)> fp(
                std::fopen("ch14_tmp.txt", "w"), file_closer);
            if (fp) {
                std::fputs("智能指针也能管文件句柄\n", fp.get());
                std::cout << "      [+] 文件已打开并写入\n";
            }
        }   // 自动 fclose

        // 场景 2：malloc 出来的内存要用 free
        auto free_deleter = [](void* p) {
            std::cout << "      [-] free 释放 malloc 的内存\n";
            std::free(p);
        };
        {
            std::unique_ptr<void, decltype(free_deleter)> mem(std::malloc(1024), free_deleter);
            std::cout << "      [+] malloc 1024 字节\n";
        }

        // 场景 3：函数指针作删除器（会让 unique_ptr 变大，因为要存指针）
        std::cout << "  带函数指针删除器的大小 = "
                  << sizeof(std::unique_ptr<std::FILE, int (*)(std::FILE*)>)
                  << "   <== 16 字节（对象指针 + 函数指针）\n";
        std::cout << "  无捕获 lambda 删除器    = "
                  << sizeof(std::unique_ptr<std::FILE, decltype(file_closer)>)
                  << "   <== 8 字节，lambda 更优\n";
        std::remove("ch14_tmp.txt");
    }

    demo::section("数组特化");
    {
        auto arr = std::make_unique<int[]>(5);   // 元素零初始化
        for (int i = 0; i < 5; ++i) arr[i] = i * i;   // 有 operator[]，没有 operator->
        std::cout << "  make_unique<int[]>(5): ";
        for (int i = 0; i < 5; ++i) std::cout << arr[i] << ' ';
        std::cout << "\n  内部调用 delete[]，不会错配\n";
        std::cout << "  实际项目里优先 std::vector：多了 size()、可增长、可拷贝\n";
    }

    demo::section("函数签名怎么写 —— 用类型表达所有权意图");
    std::cout <<
        "    void sink(unique_ptr<T> p)         我接管所有权（调用方必须 move 进来）\n"
        "    unique_ptr<T> source()             我把所有权交给你（工厂函数）\n"
        "    void use(const T& t)               我只读，所有权在你那儿  <== 最常用\n"
        "    void modify(T& t)                  我要改，所有权在你那儿\n"
        "    void maybe(T* p)                   我只读，且可能为空\n"
        "    void reseat(unique_ptr<T>& p)      我要换掉你持有的对象（少见）\n"
        "\n"
        "  反例：void bad(shared_ptr<T> p) —— 只是想读一下，却强迫调用方\n"
        "        承担引用计数的原子操作开销，还把接口和 shared_ptr 绑死了。\n";
}

// unique_ptr 的典型应用 1：工厂函数
class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual std::string name() const = 0;
};
class Circle : public Shape {
    double r_;
public:
    explicit Circle(double r) : r_(r) {}
    double area() const override { return 3.14159265358979 * r_ * r_; }
    std::string name() const override { return "Circle"; }
};
class Rect : public Shape {
    double w_, h_;
public:
    Rect(double w, double h) : w_(w), h_(h) {}
    double area() const override { return w_ * h_; }
    std::string name() const override { return "Rect"; }
};
// 返回 unique_ptr<基类>：调用方拿到所有权，多态可用，且不可能忘记释放
static std::unique_ptr<Shape> make_shape(const std::string& kind) {
    if (kind == "circle") return std::make_unique<Circle>(2.0);
    if (kind == "rect")   return std::make_unique<Rect>(3.0, 4.0);
    return nullptr;
}

// unique_ptr 的典型应用 2：Pimpl（隐藏实现，稳定 ABI，降低编译依赖）
class Widget {
public:
    Widget();
    ~Widget();                         // 必须在 .cpp 里定义（此处 Impl 不完整）
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;
    void draw() const;
private:
    struct Impl;                       // 只声明，不定义
    std::unique_ptr<Impl> impl_;
};
struct Widget::Impl {                  // 真实定义（正常项目里在 .cpp 中）
    std::string heavy_state = "重量级实现细节";
    void draw() const { std::cout << "      Widget::Impl::draw  " << heavy_state << "\n"; }
};
Widget::Widget() : impl_(std::make_unique<Impl>()) {}
Widget::~Widget() = default;           // 到这里 Impl 已完整，可以析构
Widget::Widget(Widget&&) noexcept = default;
Widget& Widget::operator=(Widget&&) noexcept = default;
void Widget::draw() const { impl_->draw(); }

static void partB1_unique_ptr_usecases() {
    demo::section("应用场景实测");

    std::cout << "  1) 工厂函数返回多态对象：\n";
    for (const char* k : {"circle", "rect"}) {
        if (auto s = make_shape(k))
            std::cout << "      " << s->name() << " 面积=" << s->area() << "\n";
    }

    std::cout << "  2) 多态容器（存指针才不会切片）：\n";
    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Circle>(1.0));
    shapes.push_back(std::make_unique<Rect>(2.0, 5.0));
    double total = 0;
    for (const auto& s : shapes) total += s->area();
    std::cout << "      两个图形总面积 = " << total << "\n";

    std::cout << "  3) Pimpl：\n";
    Widget w;
    w.draw();
    std::cout << "      头文件里看不到 Impl 的任何细节，改 Impl 不会导致下游重编译\n";
}

// -----------------------------------------------------------------------------
// B2. shared_ptr —— 共享所有权，内部结构详解
// -----------------------------------------------------------------------------
// shared_ptr 是「两个指针」：一个指对象，一个指控制块。
//
//   shared_ptr<T> sp;              控制块 (control block)
//   ┌──────────────┐              ┌────────────────────────┐
//   │ ptr        ──┼─────────────►│ strong_count  (原子)   │
//   │ ctrl       ──┼─────────────►│ weak_count    (原子)   │
//   └──────────────┘              │ deleter               │
//                                 │ allocator             │
//                                 └────────────────────────┘
//
// 两个计数的分工（很多人只知道 strong）：
//   strong_count == 0  -> 销毁**对象**（调用 T 的析构函数 / 删除器）
//   weak_count   == 0  -> 释放**控制块**本身的内存
//
// 为什么要分开？因为 weak_ptr 必须能在对象已死后安全地回答
// 「对象还活着吗」——它得读 strong_count，所以控制块必须比对象活得久。
//
// make_shared 与 shared_ptr(new T) 的区别（面试高频）：
//   shared_ptr<T>(new T)   两次堆分配（T 一次，控制块一次）；
//                          若控制块分配失败，new T 出来的对象会泄漏
//   make_shared<T>()       一次堆分配，对象和控制块放在同一块内存里，
//                          缓存友好、异常安全
//                          唯一缺点：只要还有 weak_ptr 存在，
//                          整块内存（含对象那部分）都无法归还系统
static void partB2_shared_ptr() {
    demo::title("Part B2 —— shared_ptr");

    demo::section("大小与计数");
    std::cout << "  sizeof(shared_ptr<Tracked>) = " << sizeof(std::shared_ptr<Tracked>)
              << "   <== 两个指针：对象指针 + 控制块指针\n";
    {
        auto p1 = std::make_shared<Tracked>("s1");
        std::cout << "  刚创建           use_count=" << p1.use_count() << "\n";
        {
            auto p2 = p1;                 // 拷贝：原子自增
            auto p3 = p1;
            std::cout << "  两次拷贝后       use_count=" << p1.use_count() << "\n";
            std::weak_ptr<Tracked> w = p1;   // weak 不影响 strong
            std::cout << "  加一个 weak_ptr  use_count=" << p1.use_count()
                      << "  （不变）\n";
        }   // p2 p3 析构，原子自减
        std::cout << "  内层作用域结束   use_count=" << p1.use_count() << "\n";
    }   // strong 归零 -> 析构对象

    demo::section("移动 vs 拷贝的性能差异");
    {
        auto src = std::make_shared<Tracked>("s2");
        auto copied = src;               // 原子自增（有锁总线开销）
        auto moved  = std::move(copied); // 只搬两个指针，**无**原子操作
        std::cout << "  拷贝要做原子自增；移动只是指针搬运，明显更快\n";
        std::cout << "  能 move 的地方别 copy —— 高并发下原子操作是真实瓶颈\n";
        std::cout << "  use_count=" << src.use_count() << "\n";
    }

    demo::section("自定义删除器（不影响 shared_ptr 的类型！）");
    {
        // 与 unique_ptr 不同：删除器存在控制块里，不进类型。
        // 所以 shared_ptr<FILE> 可以持有任意删除器，能放进同一个容器。
        auto fp = std::shared_ptr<std::FILE>(
            std::fopen("ch14_tmp2.txt", "w"),
            [](std::FILE* f) {
                if (f) { std::cout << "      [-] 删除器：fclose\n"; std::fclose(f); }
            });
        if (fp) std::cout << "      [+] 文件打开成功\n";
        std::cout << "  sizeof 仍是 " << sizeof(fp) << " —— 删除器不占 shared_ptr 的大小\n";
    }
    std::remove("ch14_tmp2.txt");

    demo::section("别名构造 —— 持有成员，却延长整个对象的寿命");
    {
        struct Parent {
            std::string child = "我是 Parent 里的成员";
            ~Parent() { std::cout << "      [-] Parent 析构\n"; }
        };
        std::shared_ptr<std::string> member_ptr;
        {
            auto parent = std::make_shared<Parent>();
            // 别名构造：ptr 指向成员，但控制块用 parent 的
            member_ptr = std::shared_ptr<std::string>(parent, &parent->child);
            std::cout << "      parent 作用域内，use_count=" << parent.use_count() << "\n";
        }   // parent 这个 shared_ptr 走了，但 member_ptr 还持有控制块
        std::cout << "      parent 离开作用域后，Parent 仍然存活（没有析构输出）\n";
        std::cout << "      通过 member_ptr 访问成员: " << *member_ptr << "\n";
    }
    std::cout << "      ^^^ member_ptr 析构，此时 Parent 才真正销毁\n";

    demo::section("线程安全的边界（极其容易误解）");
    std::cout <<
        "  **安全**：多线程同时拷贝/析构同一个 shared_ptr\n"
        "            （引用计数是原子的）\n"
        "  **不安全**：多线程同时给同一个 shared_ptr **变量**赋值\n"
        "            （改的是 ptr 和 ctrl 两个字段，不是原子的）\n"
        "  **不安全**：多线程同时读写它**指向的对象**\n"
        "            （shared_ptr 只管指针的线程安全，不管数据的）\n"
        "\n"
        "  一句话：引用计数是原子的，指针本身和被指对象都不是。\n"
        "  C++20 起有 std::atomic<std::shared_ptr<T>> 解决第二种情况。\n";
}

// -----------------------------------------------------------------------------
// B3. weak_ptr —— 观察者，不参与所有权
// -----------------------------------------------------------------------------
static void partB3_weak_ptr() {
    demo::title("Part B3 —— weak_ptr");

    demo::section("基本用法：lock() 提升");
    {
        std::weak_ptr<Tracked> w;
        {
            auto s = std::make_shared<Tracked>("w1");
            w = s;
            std::cout << "  对象存活: expired=" << w.expired()
                      << "  use_count=" << w.use_count() << "\n";
            if (auto locked = w.lock()) {      // 提升成 shared_ptr（原子地）
                std::cout << "  lock() 成功，此时 use_count=" << locked.use_count()
                          << "  （提升会临时+1）\n";
                locked->hello();
            }
        }   // s 析构 -> 对象销毁
        std::cout << "  对象销毁: expired=" << w.expired() << "\n";
        if (auto locked = w.lock()) {
            std::cout << "  不会走到这里\n";
        } else {
            std::cout << "  lock() 返回空 shared_ptr，安全地知道「对象没了」\n";
        }
    }

    std::cout <<
        "\n  为什么必须 lock() 而不能直接用？\n"
        "    多线程下，你「检查 expired」和「使用对象」之间，对象可能刚好被销毁。\n"
        "    lock() 把「检查 + 加引用」做成一个原子操作，拿到 shared_ptr 后\n"
        "    对象就一定活着。所以：**永远不要写 if (!w.expired()) w.lock()->f();**\n";

    demo::section("用途 1：打破循环引用");
    {
        // 典型场景：树/双向链表，父子互相引用
        struct Node {
            std::string          name;
            std::vector<std::shared_ptr<Node>> children;   // 父拥有子 -> shared
            std::weak_ptr<Node>  parent;                   // 子指向父 -> weak（关键）
            explicit Node(std::string n) : name(std::move(n)) {}
            ~Node() { std::cout << "      [-] Node " << name << " 析构\n"; }
        };
        {
            auto root  = std::make_shared<Node>("root");
            auto child = std::make_shared<Node>("child");
            root->children.push_back(child);
            child->parent = root;                          // weak，不增加计数

            std::cout << "      root.use_count=" << root.use_count()
                      << "  child.use_count=" << child.use_count() << "\n";
            // 通过 weak 访问父节点
            if (auto p = child->parent.lock())
                std::cout << "      child 的父节点是 " << p->name << "\n";
        }
        std::cout << "      ^^^ 两个节点都正常析构（若 parent 用 shared 则全部泄漏）\n";
    }

    demo::section("用途 2：缓存（不阻止对象被回收）");
    {
        // 缓存持有 weak_ptr：只要外部还在用就命中缓存，没人用了就自动失效
        class ImageCache {
            std::map<std::string, std::weak_ptr<Tracked>> cache_;
        public:
            std::shared_ptr<Tracked> get(const std::string& key) {
                if (auto it = cache_.find(key); it != cache_.end()) {
                    if (auto hit = it->second.lock()) {
                        std::cout << "      缓存命中: " << key << "\n";
                        return hit;
                    }
                    std::cout << "      缓存条目已失效，清理并重新加载: " << key << "\n";
                    cache_.erase(it);
                }
                auto obj = std::make_shared<Tracked>("image:" + key);
                cache_[key] = obj;          // 只存 weak，不延长寿命
                return obj;
            }
            size_t entries() const { return cache_.size(); }
        };

        ImageCache cache;
        {
            auto a = cache.get("logo.png");
            auto b = cache.get("logo.png");   // 命中
            std::cout << "      a 和 b 是同一对象: " << (a == b) << "\n";
        }   // a b 都销毁 -> 对象被回收，缓存里的 weak 失效
        auto c = cache.get("logo.png");       // 失效，重新加载
        std::cout << "      缓存条目数 = " << cache.entries() << "\n";
    }

    demo::section("用途 3：异步回调中安全引用 this");
    std::cout <<
        "  见第 13 章坑 22：回调里捕获 weak_from_this()，\n"
        "  执行时 lock() 一下 —— 对象已销毁就安全跳过。\n"
        "  这是 Reactor / 异步框架的标准写法（第 16 章会大量使用）。\n";
}

// -----------------------------------------------------------------------------
// B4. enable_shared_from_this 的原理
// -----------------------------------------------------------------------------
// 问题：成员函数里怎么拿到「管理自己的那个 shared_ptr」？
//       直接 shared_ptr<T>(this) 会创建**第二个控制块** -> 双重释放。
//
// 原理：enable_shared_from_this<T> 内部有一个 weak_ptr<T> 成员。
//       当你用 shared_ptr 首次接管这个对象时，shared_ptr 的构造函数会检测到
//       T 继承自 enable_shared_from_this（通过 SFINAE），于是把自己的控制块
//       塞进那个 weak_ptr 里。之后 shared_from_this() 只是 weak.lock()。
//
//       伪代码：
//         template<class T> class enable_shared_from_this {
//             mutable weak_ptr<T> weak_this_;      // 由 shared_ptr 构造函数填写
//         public:
//             shared_ptr<T> shared_from_this() { return shared_ptr<T>(weak_this_); }
//             weak_ptr<T>   weak_from_this()   { return weak_this_; }   // C++17
//         };
//
// 三个必须遵守的前提：
//   1. 对象必须**已经**被 shared_ptr 管理，否则 weak_this_ 是空的
//      -> shared_from_this() 抛 std::bad_weak_ptr
//   2. 不能在构造函数里调用（此时 shared_ptr 还没接管）
//   3. 必须是 public 继承（shared_ptr 的构造函数要能看到基类）
class Session : public std::enable_shared_from_this<Session> {
    std::string id_;
public:
    explicit Session(std::string id) : id_(std::move(id)) {
        std::cout << "      [+] Session " << id_ << " 构造\n";
        // 这里调 shared_from_this() 会抛异常 —— 构造期间还没被 shared_ptr 接管
    }
    ~Session() { std::cout << "      [-] Session " << id_ << " 析构\n"; }

    // 典型用途：把自己注册到某个异步队列里，保证回调执行期间自己不死
    void start(std::vector<std::function<void()>>& queue) {
        auto self = shared_from_this();          // 让引用计数 +1
        queue.push_back([self, this] {           // self 被 lambda 持有 -> 对象不会死
            std::cout << "      异步回调执行，Session " << id_ << " 仍然存活\n";
        });
        std::cout << "      注册回调后 use_count=" << self.use_count() << "\n";
    }

    // 安全版本：用 weak，避免「回调队列永不清空导致对象永不释放」
    void start_weak(std::vector<std::function<void()>>& queue) {
        auto weak = weak_from_this();            // C++17
        queue.push_back([weak] {
            if (auto self = weak.lock())
                std::cout << "      [weak] Session " << self->id_ << " 存活，执行\n";
            else
                std::cout << "      [weak] Session 已销毁，跳过回调\n";
        });
    }
};

static void partB4_shared_from_this() {
    demo::title("Part B4 —— enable_shared_from_this");

    demo::section("shared_from_this：延长自身寿命直到回调执行");
    std::vector<std::function<void()>> queue;
    {
        auto s = std::make_shared<Session>("A");
        s->start(queue);
    }   // 局部 shared_ptr 销毁，但 lambda 里的 self 还持有 -> 对象不死
    std::cout << "      离开作用域，但对象还活着（回调持有强引用）\n";
    for (auto& f : queue) f();
    queue.clear();
    std::cout << "      ^^^ 清空队列，最后一个强引用消失，此时才析构\n";

    demo::section("weak_from_this：对象销毁则跳过回调");
    {
        auto s = std::make_shared<Session>("B");
        s->start_weak(queue);
    }   // 对象立刻销毁
    for (auto& f : queue) f();
    queue.clear();

    demo::section("误用：栈对象调用 shared_from_this");
    try {
        Session on_stack("C");
        std::vector<std::function<void()>> q2;
        on_stack.start(q2);
    } catch (const std::bad_weak_ptr& e) {
        std::cout << "      抛出 std::bad_weak_ptr: " << e.what() << "\n";
        std::cout << "      原因：对象不是由 shared_ptr 管理的，内部 weak_this_ 为空\n";
    }

    std::cout <<
        "\n  选择：回调**必须**执行（如写入落盘）用 shared_from_this；\n"
        "        回调只是「通知」（对象没了就不用通知了）用 weak_from_this。\n"
        "        网络库里 99% 是后者 —— 连接断了就没必要回调了。\n";
}

// =============================================================================
// Part C —— 从零手写智能指针
//
// 目的不是造轮子，而是把「引用计数」「控制块」「移动语义」这些抽象概念
// 变成你能读懂的具体代码。读完这部分，标准库的行为你就全知道为什么了。
// =============================================================================

namespace my {

// -----------------------------------------------------------------------------
// C1. MyUniquePtr —— 独占所有权
//
// 关键设计点：
//   1) 拷贝构造/拷贝赋值必须 delete —— 「独占」这个语义就是靠这个强制的
//   2) 移动必须把源置空 —— 否则两个指针指同一块内存，析构两次
//   3) 析构函数不能抛异常（否则栈展开时抛异常 = 直接 terminate）
//   4) 删除器作为模板参数，用「继承」而非「成员」以获得空基类优化
// -----------------------------------------------------------------------------
template <typename T>
struct DefaultDelete {
    void operator()(T* p) const noexcept { delete p; }
};
template <typename T>
struct DefaultDelete<T[]> {                            // 数组特化
    void operator()(T* p) const noexcept { delete[] p; }
};

template <typename T, typename Deleter = DefaultDelete<T>>
class MyUniquePtr : private Deleter {                  // 私有继承 -> 空删除器不占空间
public:
    using pointer = T*;

    // 构造：explicit，防止 MyUniquePtr<T> p = raw_ptr; 这种隐式接管
    MyUniquePtr() noexcept = default;
    explicit MyUniquePtr(pointer p) noexcept : ptr_(p) {}
    MyUniquePtr(pointer p, Deleter d) noexcept : Deleter(std::move(d)), ptr_(p) {}

    // 析构：唯一的释放点
    ~MyUniquePtr() { reset(); }

    // 禁止拷贝 —— 这两行就是「独占」的全部实现
    MyUniquePtr(const MyUniquePtr&)            = delete;
    MyUniquePtr& operator=(const MyUniquePtr&) = delete;

    // 移动构造：接管，并把源置空（置空是关键，漏了就双重释放）
    MyUniquePtr(MyUniquePtr&& o) noexcept
        : Deleter(std::move(static_cast<Deleter&>(o))), ptr_(o.ptr_) {
        o.ptr_ = nullptr;
    }

    // 移动赋值：先释放自己的旧资源，再接管
    MyUniquePtr& operator=(MyUniquePtr&& o) noexcept {
        if (this != &o) {                      // 自赋值保护
            reset();                            // 释放旧的
            static_cast<Deleter&>(*this) = std::move(static_cast<Deleter&>(o));
            ptr_   = o.ptr_;
            o.ptr_ = nullptr;
        }
        return *this;
    }

    // 支持 p = nullptr
    MyUniquePtr& operator=(std::nullptr_t) noexcept { reset(); return *this; }

    // 访问
    T&      operator*()  const noexcept { return *ptr_; }
    pointer operator->() const noexcept { return ptr_; }
    pointer get()        const noexcept { return ptr_; }

    // explicit operator bool：允许 if (p)，但禁止 int x = p; 这类荒谬转换
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // 放弃所有权，调用方负责释放
    pointer release() noexcept { pointer t = ptr_; ptr_ = nullptr; return t; }

    // 释放当前对象，接管新对象
    void reset(pointer p = nullptr) noexcept {
        pointer old = ptr_;
        ptr_ = p;
        if (old) Deleter::operator()(old);     // 先改状态再释放：防止删除器里回访本对象
    }

    void swap(MyUniquePtr& o) noexcept { std::swap(ptr_, o.ptr_); }

private:
    pointer ptr_ = nullptr;
};

// 对应的 make 函数：完美转发构造参数
template <typename T, typename... Args>
MyUniquePtr<T> make_unique_ptr(Args&&... args) {
    return MyUniquePtr<T>(new T(std::forward<Args>(args)...));
}

// -----------------------------------------------------------------------------
// C2. MyControlBlock —— 控制块
//
// 这是 shared_ptr 的心脏。要理解两个计数的分工：
//
//   strong == 0  ->  销毁**被管理的对象**，但控制块还要留着，
//                    因为可能还有 weak_ptr 需要查询「对象死了吗」
//   weak   == 0  ->  释放**控制块自身**
//
// 注意 weak 的初值是 1：所有 shared_ptr 作为一个整体，共同持有
// 「一份 weak 引用」。这样当最后一个 shared 走掉时（strong: 1->0），
// 顺便把这份 weak 也还掉（weak: n->n-1），逻辑统一，无需特殊分支。
// -----------------------------------------------------------------------------
class MyControlBlockBase {
public:
    virtual ~MyControlBlockBase() = default;
    virtual void destroy_object() noexcept = 0;   // 由派生类决定怎么销毁对象

    void add_strong() noexcept { strong_.fetch_add(1, std::memory_order_relaxed); }
    void add_weak()   noexcept { weak_.fetch_add(1, std::memory_order_relaxed); }

    void release_strong() noexcept {
        // acq_rel：保证「对象的所有写操作」在销毁前对本线程可见，
        //          且本线程的写操作对最后那个执行销毁的线程可见
        if (strong_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            destroy_object();      // 我是最后一个强引用 -> 销毁对象
            release_weak();        // 归还「所有 shared 共有的那份 weak」
        }
    }

    void release_weak() noexcept {
        if (weak_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;           // 最后一个 weak 也走了 -> 控制块自杀
        }
    }

    // weak_ptr::lock() 的核心：原子地「只有在计数不为 0 时才 +1」
    // 必须用 CAS 循环，不能写成 if (strong_ != 0) ++strong_ ——
    // 那样在判断和自增之间对象可能已被销毁。
    bool try_add_strong() noexcept {
        long cur = strong_.load(std::memory_order_relaxed);
        while (cur != 0) {
            if (strong_.compare_exchange_weak(cur, cur + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                return true;       // 抢到了，对象保证存活
            }
            // CAS 失败：cur 已被更新为最新值，重试
        }
        return false;              // 计数已归零，对象没了
    }

    long strong_count() const noexcept { return strong_.load(std::memory_order_relaxed); }

protected:
    std::atomic<long> strong_{1};
    std::atomic<long> weak_{1};    // 注意初值 1，见上面的说明
};

// 派生 1：管理 new 出来的指针（对应 shared_ptr<T>(new T)）
template <typename T, typename Deleter>
class MyControlBlockPtr final : public MyControlBlockBase {
    T*      ptr_;
    Deleter del_;
public:
    MyControlBlockPtr(T* p, Deleter d) : ptr_(p), del_(std::move(d)) {}
    void destroy_object() noexcept override {
        if (ptr_) { del_(ptr_); ptr_ = nullptr; }
    }
};

// 派生 2：对象内嵌在控制块里（对应 make_shared，一次分配）
// 用 aligned_storage 手动管理，避免「控制块构造时对象就被构造」
template <typename T>
class MyControlBlockInline final : public MyControlBlockBase {
    alignas(T) unsigned char storage_[sizeof(T)];
public:
    template <typename... Args>
    explicit MyControlBlockInline(Args&&... args) {
        ::new (storage_) T(std::forward<Args>(args)...);   // placement new
    }
    T* get() noexcept { return reinterpret_cast<T*>(storage_); }
    void destroy_object() noexcept override {
        get()->~T();     // 只调析构，不释放内存（内存属于控制块）
    }
};

// -----------------------------------------------------------------------------
// C3. MySharedPtr / MyWeakPtr
// -----------------------------------------------------------------------------
template <typename T> class MyWeakPtr;

template <typename T>
class MySharedPtr {
public:
    MySharedPtr() noexcept = default;

    // 从裸指针接管：注意每次都会 new 一个控制块 —— 这就是坑 24 的根源
    //
    // requires 约束不是装饰：没有它的话，下面那个「已有控制块」的私有构造函数
    // 会被这个模板挤掉。因为 make_shared_ptr 传进来的是
    // MyControlBlockInline<T>*（派生类指针），对本模板是**精确匹配**
    // (Deleter = MyControlBlockInline<T>*)，而对私有构造函数需要一次
    // 派生类到基类的指针转换 —— 精确匹配优先，于是模板胜出，
    // 然后在 del_(ptr_) 处报「指针不是可调用对象」。
    // 这就是第 13 章坑 26「万能引用吞掉重载」的真实翻版。
    template <typename Deleter = DefaultDelete<T>>
        requires std::is_invocable_v<Deleter&, T*>
    explicit MySharedPtr(T* p, Deleter d = Deleter{})
        : ptr_(p),
          ctrl_(p ? new MyControlBlockPtr<T, Deleter>(p, std::move(d)) : nullptr) {}

    // 拷贝：原子自增
    MySharedPtr(const MySharedPtr& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        if (ctrl_) ctrl_->add_strong();
    }

    // 移动：搬指针，**无原子操作** —— 这就是移动比拷贝快的原因
    MySharedPtr(MySharedPtr&& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        o.ptr_ = nullptr; o.ctrl_ = nullptr;
    }

    // 统一赋值（copy-and-swap：一份代码同时正确处理拷贝、移动、自赋值）
    MySharedPtr& operator=(MySharedPtr o) noexcept { swap(o); return *this; }

    ~MySharedPtr() { if (ctrl_) ctrl_->release_strong(); }

    void swap(MySharedPtr& o) noexcept {
        std::swap(ptr_, o.ptr_);
        std::swap(ctrl_, o.ctrl_);
    }

    T&  operator*()  const noexcept { return *ptr_; }
    T*  operator->() const noexcept { return ptr_; }
    T*  get()        const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    long use_count() const noexcept { return ctrl_ ? ctrl_->strong_count() : 0; }

private:
    T*                   ptr_  = nullptr;
    MyControlBlockBase*  ctrl_ = nullptr;

    // 让 make_shared_ptr 和 MyWeakPtr 能构造「已有控制块」的实例。
    // 用一个 tag 参数把它和上面的删除器模板彻底分开 —— 标准库里到处是
    // 这个手法（std::piecewise_construct、std::in_place、std::nothrow 都是）。
    struct AdoptCtrl {};
    MySharedPtr(AdoptCtrl, T* p, MyControlBlockBase* c) noexcept : ptr_(p), ctrl_(c) {}

    template <typename U, typename... Args> friend MySharedPtr<U> make_shared_ptr(Args&&...);
    friend class MyWeakPtr<T>;
};

// make_shared：一次分配，对象内嵌于控制块
template <typename T, typename... Args>
MySharedPtr<T> make_shared_ptr(Args&&... args) {
    auto* cb = new MyControlBlockInline<T>(std::forward<Args>(args)...);
    return MySharedPtr<T>(typename MySharedPtr<T>::AdoptCtrl{}, cb->get(), cb);
}

template <typename T>
class MyWeakPtr {
public:
    MyWeakPtr() noexcept = default;

    MyWeakPtr(const MySharedPtr<T>& s) noexcept : ptr_(s.ptr_), ctrl_(s.ctrl_) {
        if (ctrl_) ctrl_->add_weak();
    }
    MyWeakPtr(const MyWeakPtr& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        if (ctrl_) ctrl_->add_weak();
    }
    MyWeakPtr(MyWeakPtr&& o) noexcept : ptr_(o.ptr_), ctrl_(o.ctrl_) {
        o.ptr_ = nullptr; o.ctrl_ = nullptr;
    }
    MyWeakPtr& operator=(MyWeakPtr o) noexcept { swap(o); return *this; }
    ~MyWeakPtr() { if (ctrl_) ctrl_->release_weak(); }

    void swap(MyWeakPtr& o) noexcept {
        std::swap(ptr_, o.ptr_);
        std::swap(ctrl_, o.ctrl_);
    }

    bool expired() const noexcept { return !ctrl_ || ctrl_->strong_count() == 0; }

    // lock：原子地尝试提升。这是 weak_ptr 存在的全部意义。
    MySharedPtr<T> lock() const noexcept {
        if (ctrl_ && ctrl_->try_add_strong()) {
            return MySharedPtr<T>(typename MySharedPtr<T>::AdoptCtrl{}, ptr_, ctrl_);  // 强引用已 +1
        }
        return MySharedPtr<T>{};                   // 对象已死，返回空
    }

private:
    T*                  ptr_  = nullptr;
    MyControlBlockBase* ctrl_ = nullptr;
};

} // namespace my

// -----------------------------------------------------------------------------
// 手写版本的验证
// -----------------------------------------------------------------------------
static void partC_handwritten() {
    demo::title("Part C —— 手写智能指针验证");

    demo::section("MyUniquePtr");
    {
        auto p = my::make_unique_ptr<Tracked>("my_unique");
        p->hello();
        std::cout << "  sizeof(MyUniquePtr<Tracked>) = " << sizeof(p)
                  << "   <== 和裸指针一样，空基类优化生效\n";

        auto q = std::move(p);            // 移动
        std::cout << "  move 后源为空: " << !static_cast<bool>(p) << "\n";
        q->hello();
    }

    demo::section("MySharedPtr 引用计数");
    {
        auto a = my::make_shared_ptr<Tracked>("my_shared");
        std::cout << "  创建后        use_count=" << a.use_count() << "\n";
        {
            auto b = a;
            auto c = a;
            std::cout << "  两次拷贝后    use_count=" << a.use_count() << "\n";
        }
        std::cout << "  副本销毁后    use_count=" << a.use_count() << "\n";
    }

    demo::section("MyWeakPtr lock");
    {
        my::MyWeakPtr<Tracked> w;
        {
            auto s = my::make_shared_ptr<Tracked>("my_weak_target");
            w = my::MyWeakPtr<Tracked>(s);
            std::cout << "  对象存活: expired=" << w.expired() << "\n";
            if (auto locked = w.lock()) {
                std::cout << "  lock 成功，提升期间 use_count=" << locked.use_count() << "\n";
            }
        }
        std::cout << "  对象销毁: expired=" << w.expired() << "\n";
        std::cout << "  lock 结果为空: " << !static_cast<bool>(w.lock()) << "\n";
    }

    demo::section("多线程压测：验证原子计数正确");
    {
        auto shared = my::make_shared_ptr<Tracked>("concurrent");
        constexpr int kThreads = 4;
        constexpr int kIter    = 20000;

        std::vector<std::thread> threads;
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([shared] {          // 每个线程按值捕获（各 +1）
                for (int j = 0; j < kIter; ++j) {
                    auto copy = shared;              // 反复拷贝/析构
                    (void)copy;
                }
            });
        }
        for (auto& t : threads) t.join();
        std::cout << "  " << kThreads << " 线程各拷贝 " << kIter
                  << " 次后 use_count=" << shared.use_count()
                  << "   <== 应为 1，说明原子计数没有丢失更新\n";
    }
}

// =============================================================================
// 选型总结
// =============================================================================
static void summary() {
    demo::title("选型决策");

    std::cout <<
        "  需要管理动态分配的对象吗？\n"
        "   ├─ 不需要 -> 用栈对象 / 成员对象（最快，最安全）\n"
        "   └─ 需要 -> 所有权要共享吗？\n"
        "              ├─ 不共享（99% 的情况）-> unique_ptr    零开销\n"
        "              └─ 共享 -> shared_ptr + 反向引用用 weak_ptr\n"
        "\n"
        "  只是「借用」不涉及所有权 -> 传 T& / const T& / T*（不要传智能指针）\n"
        "\n"
        "  开销对照：\n"
        "    unique_ptr    大小 = 1 指针，操作全部内联，运行时开销为 0\n"
        "    shared_ptr    大小 = 2 指针，拷贝/析构各一次原子操作（几十个周期）\n"
        "    weak_ptr      大小 = 2 指针，lock() 是一次 CAS 循环\n"
        "\n"
        "  高频错误清单（详见第 13 章）：\n"
        "    坑 23  shared_ptr 循环引用          -> 反向引用改 weak_ptr\n"
        "    坑 24  同一裸指针给两个 shared_ptr  -> 只用 make_shared\n"
        "    坑 25  shared_ptr<T>(this)          -> enable_shared_from_this\n"
        "    坑 22  回调捕获裸 this              -> 捕获 weak_from_this()\n"
        "\n"
        "  三条实践准则：\n"
        "    1. 永远用 make_unique / make_shared，不写裸 new\n"
        "    2. 函数参数默认传引用，只在「转移或共享所有权」时传智能指针\n"
        "    3. shared_ptr 是最后的选择，不是默认选择 —— 它意味着\n"
        "       「生命周期由运行时决定」，这本身就让程序更难推理\n";
}

// =============================================================================
int main() {
    demo::title("第 14 章  智能指针深入");
    std::cout << "  A 为什么需要  |  B 完整用法与内部结构  |  C 手写实现\n";

    partA_why();

    partB1_unique_ptr();
    partB1_unique_ptr_usecases();
    partB2_shared_ptr();
    partB3_weak_ptr();
    partB4_shared_from_this();

    partC_handwritten();

    summary();
    return 0;
}
