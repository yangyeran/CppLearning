# 第 0 章 · C++ 语法回忆速通

▶ 对应程序：`ch00_refresher`

这一章把「以前会但忘了」的东西集中捡回来。只用 C++98/03 就有的语法，为后面新特性打底。

---

## 0.1 编译链接模型 —— 先搞懂这个，报错才看得懂

```
   main.cpp        util.cpp
      │               │
      │ 预处理        │      展开 #include（就是把头文件内容原样贴进来）
      │               │      展开 #define，处理 #if
      ▼               ▼
   main.i          util.i
      │               │
      │ 编译          │      语法检查 → 生成汇编 → 汇编成机器码
      │               │      【每个 .cpp 独立编译，互相看不见】
      ▼               ▼
   main.obj        util.obj      ← 目标文件，里面有「符号表」
      │               │
      └───────┬───────┘
              │ 链接                把各个 .obj 里「用到但没定义」的符号
              │                     和「定义了」的符号对上号
              ▼
          program.exe
```

**为什么要有 .h 和 .cpp 分开**

- `.h` 放**声明**（"有这么个东西，长这样"）
- `.cpp` 放**定义**（"这个东西具体是什么"）
- 每个 `.cpp` 独立编译，只需要看到声明就能编译通过；链接时才需要找到定义

**两个最常见的链接错误**

| 错误 | 含义 | 常见原因 |
|------|------|---------|
| `LNK2019 无法解析的外部符号` | 声明了但找不到定义 | 忘了实现；忘了把 .cpp 加进工程；忘了链接第三方库；C 和 C++ 混用没加 `extern "C"` |
| `LNK2005 重定义` | 同一个符号有多个定义 | 在头文件里定义了非 inline 的函数/变量，被多个 cpp 包含 |

**头文件必须加保护**

```cpp
#pragma once            // 现代写法，所有主流编译器都支持

// 或者传统写法
#ifndef MY_HEADER_H
#define MY_HEADER_H
// ...
#endif
```

没有它，`a.h` 包含 `b.h`，`b.h` 又包含 `a.h`，就会无限递归或重复定义。

---

## 0.2 指针 vs 引用 —— 最容易糊涂的地方

```cpp
int x = 10;
int* p = &x;      // 指针：变量 p 里【存的是 x 的地址】
int& r = x;       // 引用：r 就【是】 x 的另一个名字
```

| | 指针 `T*` | 引用 `T&` |
|---|---|---|
| 可以为空 | ✅ `nullptr` | ❌ 必须绑定到有效对象 |
| 必须初始化 | ❌ 可以先声明 | ✅ 声明时就必须绑定 |
| 可以改指向 | ✅ `p = &y;` | ❌ `r = y;` 是给 x 赋值，不是重新绑定 |
| 有自己的地址 | ✅ `&p` 是指针的地址 | ❌ `&r` 就是 `&x` |
| 需要解引用 | ✅ `*p` | ❌ 直接用 `r` |
| 可以有指针的指针 | ✅ `int**` | ❌ 没有引用的引用 |

**什么时候用哪个**

```cpp
// 参数：能用引用就用引用，语法干净且不用判空
void f(const std::string& s);      // 只读大对象  ← 最常用
void g(std::string& s);            // 要修改调用方的对象
void h(std::string* s);            // 「可以不传」时用指针（传 nullptr 表示没有）

// 返回值：绝对不要返回局部变量的引用或指针！
int& bad() { int x = 1; return x; }   // 悬垂引用，未定义行为
```

**三种传参的实测对比**（`ch00_refresher` 里能看到输出）

```cpp
void by_value(int x)  { x = 100; }              // 改副本，外面不变
void by_ref(int& x)   { x = 200; }              // 改本体
void by_ptr(int* x)   { if (x) *x = 300; }      // 改本体，但要判空

int v = 1;
by_value(v);   // v 还是 1
by_ref(v);     // v 变 200
by_ptr(&v);    // v 变 300
```

---

## 0.3 栈内存 vs 堆内存

```
高地址  ┌──────────────────┐
        │      栈 Stack    │  局部变量、函数参数、返回地址
        │        ↓         │  自动分配释放，速度极快
        │                  │  容量小（默认 1MB Windows / 8MB Linux）
        │    (未使用)      │  栈溢出 = 递归太深 或 局部数组太大
        │                  │
        │        ↑         │
        │      堆 Heap     │  new / malloc 分配的内存
        ├──────────────────┤  手动管理（或用智能指针），慢
        │   BSS / Data     │  全局变量、静态变量
        ├──────────────────┤
低地址  │   Text (代码)    │  机器指令，只读
        └──────────────────┘
```

```cpp
void f() {
    int a = 1;                   // 栈：函数返回自动销毁
    int arr[1000];               // 栈：4KB，还好
    // int huge[10000000];       // 栈：40MB，直接栈溢出崩溃

    int* p = new int(2);         // 堆：必须 delete，否则泄漏
    delete p;

    int* arr2 = new int[100];    // 堆数组
    delete[] arr2;               // 注意是 delete[] 不是 delete！
}
```

**现代 C++ 的态度：几乎不该出现裸的 `new` / `delete`。**
用 `std::vector`、`std::string`、`std::unique_ptr` —— 它们内部管堆，
外部用起来像栈对象，离开作用域自动释放。

---

## 0.4 类的六个特殊成员函数

```cpp
class Widget {
public:
    Widget();                                 // 1. 默认构造
    ~Widget();                                // 2. 析构
    Widget(const Widget&);                    // 3. 拷贝构造
    Widget& operator=(const Widget&);         // 4. 拷贝赋值
    Widget(Widget&&) noexcept;                // 5. 移动构造   (C++11)
    Widget& operator=(Widget&&) noexcept;     // 6. 移动赋值   (C++11)
};
```

**编译器会自动生成它们**，但有联动规则（很绕，记结论即可）：

- 你写了**任何一个**拷贝/移动/析构 → 移动操作**不会**自动生成
- 你写了移动操作 → 拷贝操作被**删除**

**两条实用法则**

> **零法则（Rule of Zero）**：优先让类不管理任何资源（成员全用 vector / string / 智能指针），
> 这样六个函数一个都不用写，编译器生成的全都是对的。**这是首选。**

> **五法则（Rule of Five）**：如果确实要管裸资源（文件句柄、socket、C API 指针），
> 那五个（析构 + 拷贝×2 + 移动×2）都要显式处理，缺一个都可能出 bug。

**构造函数初始化列表 vs 函数体内赋值**

```cpp
class Foo {
    std::string name_;
    const int   id_;
    Bar&        ref_;
public:
    // ✅ 初始化列表：直接构造，一步到位
    Foo(std::string n, int i, Bar& b) : name_(std::move(n)), id_(i), ref_(b) {}

    // ❌ 函数体赋值：先默认构造再赋值，多一次操作
    //    而且 const 成员和引用成员【根本没法】在函数体里赋值
    Foo(std::string n) { name_ = n; /* id_ = ...; 编译错误 */ }
};
```

**【坑】初始化顺序取决于成员的声明顺序，不是初始化列表的书写顺序。**

```cpp
class Bad {
    int b_;
    int a_;
public:
    Bad() : a_(1), b_(a_) {}   // b_ 先初始化（声明在前），此时 a_ 还是垃圾值！
};
```

开 `/W4` 或 `-Wall` 编译器会警告，所以**警告一定要开**。

---

## 0.5 继承与多态

```cpp
class Animal {
public:
    virtual ~Animal() = default;              // 【必须】基类析构要 virtual
    virtual std::string speak() const = 0;    // 纯虚 = 抽象接口
    virtual void describe() const {           // 虚函数：有默认实现
        std::cout << speak();
    }
};

class Dog : public Animal {
public:
    std::string speak() const override { return "汪"; }   // override 让编译器帮你查错
};
```

**虚函数是怎么工作的**

```
   Dog 对象                 Dog 的虚表 (vtable)
  ┌──────────┐             ┌────────────────────┐
  │  vptr ───┼────────────>│ [0] ~Dog()         │
  ├──────────┤             │ [1] Dog::speak()   │
  │ 成员数据 │             │ [2] Animal::describe()│
  └──────────┘             └────────────────────┘

  animal_ptr->speak();
  实际执行: (*(animal_ptr->vptr[1]))(animal_ptr)
```

- **代价**：对象多一个指针（8 字节）；调用多一次间接跳转；无法内联
- **收益**：运行期多态 —— 一份代码处理所有派生类型

**【坑 1】基类析构不是 virtual = 内存泄漏**

```cpp
Animal* p = new Dog();
delete p;      // 如果 ~Animal() 不是 virtual，只会调 ~Animal()，
               // Dog 特有的成员不会被析构 -> 泄漏
```

规则：**只要类可能被继承并通过基类指针 delete，析构就必须 virtual。**
反之，不打算被继承的类应该标 `final`。

**【坑 2】对象切片（Object Slicing）**

```cpp
void f(Animal a);          // 按值传基类！
Dog d;
f(d);                      // Dog 被"切"成 Animal，派生部分丢失，多态失效

void g(const Animal& a);   // ✅ 正确：用引用或指针才有多态
```

**【坑 3】不要在构造函数/析构函数里调虚函数**
构造基类时派生类还没构造好，虚表还指向基类 —— 调不到派生类的实现。

**【面试】`override` 的价值**

```cpp
struct Base { virtual void f(int); };
struct Derived : Base {
    void f(long) override;   // 编译错误！签名不匹配
    // 不写 override 的话，编译器会当成【新增一个虚函数】，
    // 你调 base_ptr->f(1) 永远走不到这里，能调试一整天
};
```

---

## 0.6 模板

```cpp
// 函数模板
template <typename T>
T max_of(T a, T b) { return a > b ? a : b; }

// 类模板
template <typename T>
class Stack {
    std::vector<T> data_;
public:
    void push(const T& v) { data_.push_back(v); }
    const T& top() const { return data_.back(); }
};

// 全特化：为特定类型换一套实现
template <>
class Stack<bool> { /* 位压缩版 */ };
```

**模板是「编译期的代码生成器」**

`Stack<int>` 和 `Stack<double>` 是**两个完全不同的类**，编译器为每个用到的类型
各生成一份代码（叫「实例化」）。这带来：

- ✅ 零运行期开销，全部可内联
- ❌ 代码膨胀（每个类型一份）
- ❌ 编译慢
- ❌ 模板定义必须放头文件里（因为实例化时需要看到完整定义）
- ❌ C++20 之前报错信息极其恐怖（几百行）—— 这正是 concepts 要解决的问题

---

## 0.7 异常与 RAII

```
标准异常继承树
  std::exception
    ├── std::logic_error          （程序逻辑错误，本可避免）
    │     ├── std::invalid_argument
    │     ├── std::domain_error
    │     ├── std::length_error
    │     └── std::out_of_range
    └── std::runtime_error        （运行期才能发现的错误）
          ├── std::overflow_error
          ├── std::underflow_error
          ├── std::range_error
          └── std::system_error   （errno / OS 错误码）
    └── std::bad_alloc            （new 失败）
    └── std::bad_cast             （dynamic_cast 引用版失败）
```

```cpp
try {
    may_throw();
} catch (const std::invalid_argument& e) {   // 具体的放前面
    std::cout << e.what();
} catch (const std::exception& e) {          // 基类兜底放后面
    std::cout << e.what();
} catch (...) {                              // 什么都接（很少用）
}
```

**永远用 `const&` 接住异常** —— 按值接会切片，按指针接是 C 的思路。

**RAII 是 C++ 处理异常安全的唯一正解**

```cpp
void bad() {
    Lock* l = new Lock();
    may_throw();          // 抛异常 -> 下面那行不执行 -> 锁泄漏、死锁
    delete l;
}

void good() {
    std::lock_guard<std::mutex> l(mtx);   // 栈对象
    may_throw();          // 抛异常 -> 栈展开 -> l 的析构一定被调用 -> 锁释放
}
```

**核心机制叫「栈展开（stack unwinding）」**：异常抛出后，从抛出点到 catch 点之间
所有已构造的栈对象，析构函数都会被依次调用。这是 C++ 相对 C 最大的优势之一。

**【坑】析构函数绝对不能抛异常。** 栈展开过程中如果析构又抛，直接 `std::terminate`。
C++11 起析构函数默认就是 `noexcept`。

---

<div style="page-break-after: always;"></div>
