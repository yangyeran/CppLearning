// =============================================================================
// 第 0 章 —— C++ 语法回忆速通
//
// 目标：15 分钟把「忘掉的 C++ 基础」全部捡回来。
//       这一章只用 C++98/03 就有的东西 + 极少量现代写法，
//       为后面 C++11~23 的新特性做铺垫。
//
// 运行： ch00_refresher.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <memory>
#include <utility>

// =============================================================================
// 1. 命名空间 —— 防止名字冲突
// =============================================================================
namespace geometry {
    const double kPi = 3.14159265358979;
    double circle_area(double r) { return kPi * r * r; }
}

// =============================================================================
// 2. 函数：重载 / 默认参数 / 传值 vs 传引用 vs 传指针
// =============================================================================

// 重载：名字相同，参数列表不同。编译期按参数类型选择。
int  add(int a, int b)         { return a + b; }
double add(double a, double b) { return a + b; }

// 默认参数：只能从右往左给
int power(int base, int exp = 2) {
    int r = 1;
    for (int i = 0; i < exp; ++i) r *= base;
    return r;
}

// 三种传参方式 —— 这是新手最容易糊涂的地方
void by_value(int x)   { x = 100; }   // 拷贝一份，改的是副本，外面看不到
void by_ref(int& x)    { x = 200; }   // 引用 = 别名，直接改外面的变量
void by_ptr(int* x)    { if (x) *x = 300; }  // 指针，需要解引用，可能为空

// const 引用：既避免拷贝，又保证不改 —— 传大对象的标准做法
void print_big(const std::string& s) { std::cout << "  收到: " << s << "\n"; }

// =============================================================================
// 3. 类：封装 / 构造 / 析构 / this / 静态成员 / 运算符重载
// =============================================================================
class Rectangle {
public:
    // 构造函数：用「初始化列表」而不是函数体里赋值（更高效、const 成员只能这样初始化）
    Rectangle(double w, double h) : width_(w), height_(h) {
        ++count_;
        std::cout << "  [构造] Rectangle " << w << "x" << h << "\n";
    }

    // 析构函数：对象生命周期结束时自动调用 —— RAII 的基石
    ~Rectangle() {
        --count_;
        std::cout << "  [析构] Rectangle " << width_ << "x" << height_ << "\n";
    }

    // const 成员函数：承诺不修改对象状态，const 对象只能调用 const 函数
    double area() const { return width_ * height_; }
    double perimeter() const { return 2 * (width_ + height_); }

    // getter / setter
    double width() const { return width_; }
    void set_width(double w) { width_ = w; }

    // 运算符重载
    Rectangle operator+(const Rectangle& o) const {
        return Rectangle(width_ + o.width_, height_ + o.height_);
    }
    bool operator==(const Rectangle& o) const {
        return width_ == o.width_ && height_ == o.height_;
    }

    // 静态成员函数：不属于某个对象，没有 this
    static int alive_count() { return count_; }

private:
    double width_;
    double height_;
    static int count_;   // 静态数据成员：所有对象共享一份，需在类外定义
};

int Rectangle::count_ = 0;   // 静态成员的定义（C++17 起可写 inline static 就地初始化）

// 支持 std::cout << rect
std::ostream& operator<<(std::ostream& os, const Rectangle& r) {
    return os << "Rectangle(" << r.width() << ")";
}

// =============================================================================
// 4. 继承与多态
// =============================================================================
class Animal {
public:
    explicit Animal(std::string name) : name_(std::move(name)) {}

    // 【关键】基类析构必须是 virtual，否则 delete 基类指针不会调用派生类析构 -> 泄漏
    virtual ~Animal() { std::cout << "  [析构] Animal " << name_ << "\n"; }

    // 纯虚函数 = 抽象接口，有纯虚函数的类不能实例化
    virtual std::string speak() const = 0;

    // 普通虚函数：有默认实现，派生类可覆盖
    virtual void describe() const {
        std::cout << "  我是 " << name_ << "，我说：" << speak() << "\n";
    }

    const std::string& name() const { return name_; }

protected:                      // protected：派生类能访问，外部不能
    std::string name_;
};

class Dog : public Animal {
public:
    using Animal::Animal;       // 继承构造函数（C++11）
    ~Dog() override { std::cout << "  [析构] Dog\n"; }
    std::string speak() const override { return "汪汪"; }   // override 让编译器帮你查错
};

class Cat : public Animal {
public:
    using Animal::Animal;
    ~Cat() override { std::cout << "  [析构] Cat\n"; }
    std::string speak() const override { return "喵喵"; }
    void describe() const override {                        // 覆盖默认实现
        std::cout << "  高贵的 " << name_ << " 冷淡地说：" << speak() << "\n";
    }
};

// 多态的意义：写一份代码，处理所有派生类型
void make_them_talk(const std::vector<std::unique_ptr<Animal>>& animals) {
    for (const auto& a : animals) {
        a->describe();          // 运行期通过虚表找到真正的实现
    }
}

// =============================================================================
// 5. 模板 —— 编译期的「代码生成器」
// =============================================================================

// 函数模板
template <typename T>
T max_of(T a, T b) { return a > b ? a : b; }

// 类模板
template <typename T>
class Stack {
public:
    void push(const T& v) { data_.push_back(v); }
    void pop() {
        if (data_.empty()) throw std::out_of_range("栈是空的");
        data_.pop_back();
    }
    const T& top() const {
        if (data_.empty()) throw std::out_of_range("栈是空的");
        return data_.back();
    }
    bool empty() const { return data_.empty(); }
    size_t size() const { return data_.size(); }
private:
    std::vector<T> data_;
};

// 模板特化：为特定类型提供不同实现
template <>
class Stack<bool> {            // bool 版本用位压缩（这里只是演示特化语法）
public:
    void push(bool v) { bits_ = (bits_ << 1) | (v ? 1u : 0u); ++n_; }
    size_t size() const { return n_; }
private:
    unsigned bits_ = 0;
    size_t   n_    = 0;
};

// =============================================================================
// 6. 异常
// =============================================================================
double safe_divide(double a, double b) {
    if (b == 0.0) throw std::invalid_argument("除数不能为 0");
    return a / b;
}

class ResourceGuard {          // RAII：异常安全的关键
public:
    explicit ResourceGuard(std::string n) : name_(std::move(n)) {
        std::cout << "  [获取资源] " << name_ << "\n";
    }
    ~ResourceGuard() { std::cout << "  [释放资源] " << name_ << "（异常也会执行）\n"; }
private:
    std::string name_;
};

void may_throw(bool do_throw) {
    ResourceGuard g{"数据库连接"};
    if (do_throw) throw std::runtime_error("业务出错了");
    std::cout << "  正常完成\n";
}

// =============================================================================
// main
// =============================================================================
int main() {
    std::cout << "C++ 标准版本 __cplusplus = " << __cplusplus << "\n";

    // -------------------------------------------------------------------------
    demo::title("1. 基本类型与变量");
    // -------------------------------------------------------------------------
    {
        int         i = 42;
        double      d = 3.14;
        char        c = 'A';
        bool        b = true;
        std::string s = "你好 C++";

        SHOW(i); SHOW(d); SHOW(c); SHOW(b); SHOW(s);

        // 类型大小（不同平台可能不同！写跨平台代码要用 <cstdint>）
        demo::section("各类型占几个字节");
        SHOW(sizeof(char));  SHOW(sizeof(short)); SHOW(sizeof(int));
        SHOW(sizeof(long));  SHOW(sizeof(long long));
        SHOW(sizeof(float)); SHOW(sizeof(double)); SHOW(sizeof(void*));

        // const：常量，编译期检查不可修改
        const int kMax = 100;
        SHOW(kMax);

        // 类型转换
        demo::section("类型转换");
        double pi = 3.99;
        SHOW(static_cast<int>(pi));           // 3，截断不四舍五入
        SHOW(static_cast<double>(7) / 2);     // 3.5（不转的话 7/2 == 3 整数除法）
        SHOW(7 / 2);
        SHOW(7 % 2);
    }

    // -------------------------------------------------------------------------
    demo::title("2. 指针 与 引用 —— C++ 的核心概念");
    // -------------------------------------------------------------------------
    {
        int x = 10;
        int* p = &x;      // 指针：存的是地址，可以为空、可以改指向
        int& r = x;       // 引用：x 的别名，必须初始化、不能改绑定、不能为空

        demo::line("x = 10;  int* p = &x;  int& r = x;");
        SHOW(x); SHOW(*p); SHOW(r);

        *p = 20;  demo::line("执行 *p = 20 之后:");  SHOW(x); SHOW(r);
        r  = 30;  demo::line("执行  r = 30 之后:");  SHOW(x); SHOW(*p);

        demo::section("传参三种方式的区别");
        int v = 1;
        by_value(v); SHOW(v);   // 还是 1
        by_ref(v);   SHOW(v);   // 变 200
        by_ptr(&v);  SHOW(v);   // 变 300

        demo::section("指针可以为空，引用不能");
        int* np = nullptr;
        SHOW(np == nullptr);
        if (np == nullptr) demo::line("np 是空指针，解引用会崩溃，必须先判空");

        demo::section("数组与指针");
        int arr[5] = {1, 2, 3, 4, 5};
        SHOW(arr[2]);
        SHOW(*(arr + 2));            // 等价写法：数组名会退化成首元素指针
        SHOW(sizeof(arr));           // 20 = 5 * 4
        int* ap = arr;
        SHOW(sizeof(ap));            // 8（指针大小），数组信息丢了 —— 所以推荐 std::vector / std::array

        demo::section("栈内存 vs 堆内存");
        int stack_var = 1;                       // 栈：自动分配释放，快，容量小(约 1~8MB)
        int* heap_var = new int(2);              // 堆：手动 new/delete，慢，容量大
        SHOW(stack_var); SHOW(*heap_var);
        delete heap_var;                          // 忘记 delete = 内存泄漏
        demo::line("现代 C++：几乎不该手写 new/delete，用智能指针和容器（见第 1 章）");
    }

    // -------------------------------------------------------------------------
    demo::title("3. 函数：重载 / 默认参数 / 命名空间");
    // -------------------------------------------------------------------------
    {
        SHOW(add(1, 2));            // 调 int 版本
        SHOW(add(1.5, 2.5));        // 调 double 版本
        SHOW(power(3));             // exp 默认 2 -> 9
        SHOW(power(3, 3));          // 27
        SHOW(geometry::circle_area(2.0));

        print_big("这是一个很长的字符串，用 const& 传避免拷贝");
    }

    // -------------------------------------------------------------------------
    demo::title("4. 类：构造 / 析构 / 成员 / 运算符重载");
    // -------------------------------------------------------------------------
    {
        demo::section("对象的生命周期");
        SHOW(Rectangle::alive_count());
        {
            Rectangle a(3, 4);
            Rectangle b(1, 2);
            SHOW(Rectangle::alive_count());
            SHOW(a.area());
            SHOW(a.perimeter());

            Rectangle c = a + b;          // 运算符重载
            SHOW(c.width());
            SHOW(a == b);
            std::cout << "  用 << 打印对象: " << c << "\n";
        }   // 离开作用域，a b c 自动析构（顺序与构造相反）
        SHOW(Rectangle::alive_count());
    }

    // -------------------------------------------------------------------------
    demo::title("5. 继承与多态");
    // -------------------------------------------------------------------------
    {
        std::vector<std::unique_ptr<Animal>> zoo;
        zoo.push_back(std::make_unique<Dog>("旺财"));
        zoo.push_back(std::make_unique<Cat>("咪咪"));

        make_them_talk(zoo);

        demo::section("虚函数怎么工作的");
        demo::line("每个有虚函数的类有一张虚表(vtable)，对象里存一个 vptr 指向它。");
        demo::line("a->describe() 实际是: (*(a->vptr[describe 的槽位]))(a)");
        demo::line("代价：一次间接跳转 + 对象多 8 字节；收益：运行期多态。");
        SHOW(sizeof(Rectangle));   // 无虚函数：16 = 两个 double
        demo::line("Animal 有虚函数，对象里多一个 vptr 指针");

        demo::section("离开作用域，看析构顺序（派生类 -> 基类）");
    }   // zoo 析构，通过基类指针 delete，因为析构是 virtual 所以正确调用 Dog/Cat 的析构

    // -------------------------------------------------------------------------
    demo::title("6. 模板");
    // -------------------------------------------------------------------------
    {
        SHOW(max_of(3, 7));
        SHOW(max_of(3.5, 1.5));
        SHOW(max_of(std::string("apple"), std::string("banana")));

        Stack<int> si;
        si.push(1); si.push(2); si.push(3);
        SHOW(si.size());
        SHOW(si.top());
        si.pop();
        SHOW(si.top());

        Stack<bool> sb;   // 走特化版本
        sb.push(true); sb.push(false);
        SHOW(sb.size());

        demo::line("模板是编译期展开：Stack<int> 和 Stack<double> 是两个完全不同的类");
    }

    // -------------------------------------------------------------------------
    demo::title("7. 异常与 RAII");
    // -------------------------------------------------------------------------
    {
        demo::section("正常路径");
        try { may_throw(false); } catch (const std::exception& e) { std::cout << e.what(); }

        demo::section("异常路径 —— 注意资源仍然被释放");
        try {
            may_throw(true);
        } catch (const std::runtime_error& e) {
            std::cout << "  捕获异常: " << e.what() << "\n";
        }

        demo::section("异常类型的层次");
        try {
            safe_divide(1.0, 0.0);
        } catch (const std::invalid_argument& e) {
            std::cout << "  invalid_argument: " << e.what() << "\n";
        } catch (const std::exception& e) {           // 基类兜底，放最后
            std::cout << "  其它异常: " << e.what() << "\n";
        }

        demo::line("标准异常继承树: exception <- logic_error   <- invalid_argument / out_of_range");
        demo::line("                          <- runtime_error <- overflow_error / system_error");
    }

    // -------------------------------------------------------------------------
    demo::title("8. 编译链接模型（为什么要有 .h 和 .cpp）");
    // -------------------------------------------------------------------------
    {
        demo::line("源文件(.cpp) --预处理--> 展开 #include/#define");
        demo::line("             --编译----> 汇编 -> 目标文件(.obj/.o)，每个 cpp 独立编译");
        demo::line("             --链接----> 把所有 .obj 里的符号对上，产出 .exe");
        demo::line("");
        demo::line("头文件放【声明】，源文件放【定义】。");
        demo::line("头文件必须有 #pragma once（或 include guard），否则重复包含会报重定义。");
        demo::line("“无法解析的外部符号 LNK2019” = 声明了但没实现，或忘了链接某个库。");
        demo::line("“重定义 LNK2005” = 同一个函数/变量在多个 .obj 里都有定义。");
    }

    std::cout << "\n第 0 章结束。接下来跑 ch01_cpp11。\n";
    return 0;
}
