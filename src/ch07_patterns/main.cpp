// =============================================================================
// 第 7 章 —— 设计模式（现代 C++ 版）
//
// 说明：GoF 的 23 个模式是在 Java/C++98 语境下总结的。
//       在现代 C++ 里，有些模式被语言特性直接吃掉了（lambda 取代简单 Strategy，
//       variant 取代 Visitor，<=> 取代比较 mixin，指定初始化器取代简单 Builder）。
//       本章给出【现代写法】，并说明每个模式的适用边界。
//
// 运行： ch07_patterns.exe
// =============================================================================

#include "demo.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include <functional>
#include <variant>
#include <optional>
#include <algorithm>
#include <mutex>
#include <utility>
#include <cstdio>
#include <cctype>
#include <stdexcept>
#include <cstddef>

// #############################################################################
// 一、C++ 专属惯用法（比 GoF 更常用，先讲）
// #############################################################################

// -----------------------------------------------------------------------------
// 1. RAII —— C++ 最重要的模式，没有之一
//    资源获取即初始化：构造函数拿资源，析构函数还资源。
//    因为析构一定会被调用（正常返回、提前 return、抛异常都一样），所以永不泄漏。
// -----------------------------------------------------------------------------
class FileHandle {
public:
    FileHandle(const char* path, const char* mode) : f_(std::fopen(path, mode)) {
        std::cout << "    [RAII] 打开文件 " << path << (f_ ? " 成功\n" : " 失败\n");
    }
    ~FileHandle() { if (f_) { std::fclose(f_); std::cout << "    [RAII] 文件已关闭\n"; } }

    FileHandle(FileHandle&& o) noexcept : f_(std::exchange(o.f_, nullptr)) {}
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) { if (f_) std::fclose(f_); f_ = std::exchange(o.f_, nullptr); }
        return *this;
    }
    FileHandle(const FileHandle&)            = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    std::FILE* get() const { return f_; }
    explicit operator bool() const { return f_ != nullptr; }
private:
    std::FILE* f_ = nullptr;
};

// 通用的「作用域退出时执行」守卫
template <typename F>
class ScopeGuard {
public:
    explicit ScopeGuard(F f) : f_(std::move(f)) {}
    ~ScopeGuard() { if (active_) f_(); }
    void dismiss() { active_ = false; }              // 取消执行（比如操作成功了不用回滚）
    ScopeGuard(ScopeGuard&& o) noexcept
        : f_(std::move(o.f_)), active_(std::exchange(o.active_, false)) {}
    ScopeGuard(const ScopeGuard&) = delete;
private:
    F    f_;
    bool active_ = true;
};
template <typename F> ScopeGuard(F) -> ScopeGuard<F>;   // CTAD

// -----------------------------------------------------------------------------
// 2. Pimpl —— 把实现藏起来，隔断编译依赖
//    真实项目里 Widget 声明在 .h，Impl 定义在 .cpp。这里为了单文件演示放一起。
// -----------------------------------------------------------------------------
class Widget {
public:
    Widget();
    ~Widget();                                    // 必须在 Impl 完整之后定义
    Widget(Widget&&) noexcept;
    Widget& operator=(Widget&&) noexcept;
    void do_work();
private:
    struct Impl;                                  // 只声明，头文件里看不到任何实现细节
    std::unique_ptr<Impl> impl_;
};

struct Widget::Impl {                             // 这部分在真实项目里位于 .cpp
    std::string heavy_data{"一堆重量级依赖产生的数据"};
    int counter = 0;
    void work() { ++counter; std::cout << "    [Pimpl] 干活第 " << counter << " 次\n"; }
};

Widget::Widget() : impl_(std::make_unique<Impl>()) {}
Widget::~Widget() = default;                      // 放在这里，此时 Impl 是完整类型
Widget::Widget(Widget&&) noexcept = default;
Widget& Widget::operator=(Widget&&) noexcept = default;
void Widget::do_work() { impl_->work(); }

// -----------------------------------------------------------------------------
// 3. CRTP —— 静态多态（编译期，零虚函数开销）
// -----------------------------------------------------------------------------
template <typename Derived>
class Printable {
public:
    void print() const {
        std::cout << "    [CRTP] " << static_cast<const Derived&>(*this).to_string() << "\n";
    }
};
class Money : public Printable<Money> {
public:
    explicit Money(int cents) : cents_(cents) {}
    std::string to_string() const { return std::to_string(cents_ / 100) + "." +
                                           std::to_string(cents_ % 100) + " 元"; }
private:
    int cents_;
};

// -----------------------------------------------------------------------------
// 4. 类型擦除 —— 不需要继承的多态，还保留值语义
//    std::function / std::any 就是这么实现的。
// -----------------------------------------------------------------------------
class Drawable {
    struct Concept {
        virtual ~Concept() = default;
        virtual void draw() const = 0;
        virtual std::unique_ptr<Concept> clone() const = 0;
    };
    template <typename T>
    struct Model final : Concept {
        T obj;
        explicit Model(T o) : obj(std::move(o)) {}
        void draw() const override { obj.draw(); }          // 只要有 draw() 就行（鸭子类型）
        std::unique_ptr<Concept> clone() const override {
            return std::make_unique<Model>(obj);
        }
    };
    std::unique_ptr<Concept> self_;
public:
    template <typename T>
    Drawable(T obj) : self_(std::make_unique<Model<T>>(std::move(obj))) {}
    Drawable(const Drawable& o) : self_(o.self_->clone()) {}
    Drawable(Drawable&&) noexcept = default;
    void draw() const { self_->draw(); }
};
// 注意：这两个类【不继承任何东西】
struct CatShape { void draw() const { std::cout << "    /\\_/\\  猫\n"; } };
struct DogShape { void draw() const { std::cout << "    U･ᴥ･U  狗\n"; } };

// #############################################################################
// 二、创建型模式
// #############################################################################

// -----------------------------------------------------------------------------
// 单例（Meyers Singleton）—— C++11 起局部静态初始化是线程安全的
// -----------------------------------------------------------------------------
class Logger {
public:
    static Logger& instance() {
        static Logger inst;                 // 首次调用时构造，线程安全，程序结束时析构
        return inst;
    }
    void log(std::string_view msg) { std::cout << "    [Log #" << ++n_ << "] " << msg << "\n"; }

    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;
private:
    Logger() = default;
    int n_ = 0;
};

// -----------------------------------------------------------------------------
// 工厂（注册式）—— 加新类型不用改工厂代码
// -----------------------------------------------------------------------------
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
    double area() const override { return 3.14159265 * r_ * r_; }
    std::string name() const override { return "圆"; }
};
class Square : public Shape {
    double s_;
public:
    explicit Square(double s) : s_(s) {}
    double area() const override { return s_ * s_; }
    std::string name() const override { return "正方形"; }
};

class ShapeFactory {
public:
    using Creator = std::function<std::unique_ptr<Shape>(double)>;

    static ShapeFactory& instance() { static ShapeFactory f; return f; }

    void register_type(std::string key, Creator c) { creators_[std::move(key)] = std::move(c); }

    std::unique_ptr<Shape> create(const std::string& key, double param) const {
        auto it = creators_.find(key);
        return it == creators_.end() ? nullptr : it->second(param);
    }
    std::vector<std::string> registered() const {
        std::vector<std::string> ks;
        for (const auto& [k, v] : creators_) ks.push_back(k);
        return ks;
    }
private:
    std::map<std::string, Creator> creators_;
};

// 自动注册器：静态对象在 main 之前构造，把自己注册进工厂
template <typename T>
struct Registrar {
    explicit Registrar(std::string key) {
        ShapeFactory::instance().register_type(std::move(key),
            [](double p) -> std::unique_ptr<Shape> { return std::make_unique<T>(p); });
    }
};
static Registrar<Circle> s_reg_circle{"circle"};
static Registrar<Square> s_reg_square{"square"};

// -----------------------------------------------------------------------------
// 建造者（链式）—— 参数多且大部分有默认值时用
// -----------------------------------------------------------------------------
class HttpRequest {
public:
    class Builder;
    void dump() const {
        std::cout << "    " << method_ << " " << url_ << "  timeout=" << timeout_ms_ << "ms\n";
        for (const auto& [k, v] : headers_) std::cout << "      " << k << ": " << v << "\n";
        if (!body_.empty()) std::cout << "      body: " << body_ << "\n";
    }
private:
    HttpRequest() = default;
    std::string url_, method_{"GET"}, body_;
    std::map<std::string, std::string> headers_;
    int timeout_ms_ = 5000;
};

class HttpRequest::Builder {
public:
    Builder& url(std::string u)     { r_.url_ = std::move(u); return *this; }
    Builder& method(std::string m)  { r_.method_ = std::move(m); return *this; }
    Builder& header(std::string k, std::string v) { r_.headers_[std::move(k)] = std::move(v); return *this; }
    Builder& body(std::string b)    { r_.body_ = std::move(b); return *this; }
    Builder& timeout(int ms)        { r_.timeout_ms_ = ms; return *this; }
    HttpRequest build() { return std::move(r_); }
private:
    HttpRequest r_;
};

// #############################################################################
// 三、结构型模式
// #############################################################################

// 适配器：把不兼容的接口包一层
struct LegacyPrinter {                                  // 假设这是不能改的老代码
    void print_raw(const char* data, int len) {
        std::cout << "    [Legacy] " << std::string(data, len) << "\n";
    }
};
class IWriter {
public:
    virtual ~IWriter() = default;
    virtual void write(std::string_view s) = 0;
};
class LegacyAdapter : public IWriter {
    LegacyPrinter* p_;
public:
    explicit LegacyAdapter(LegacyPrinter* p) : p_(p) {}
    void write(std::string_view s) override { p_->print_raw(s.data(), static_cast<int>(s.size())); }
};

// 装饰器：层层包裹，动态添加职责
class DataSource {
public:
    virtual ~DataSource() = default;
    virtual std::string read() = 0;
};
class RawSource : public DataSource {
public:
    std::string read() override { return "原始数据"; }
};
class SourceDecorator : public DataSource {
protected:
    std::unique_ptr<DataSource> inner_;
public:
    explicit SourceDecorator(std::unique_ptr<DataSource> d) : inner_(std::move(d)) {}
};
class Encrypted : public SourceDecorator {
public:
    using SourceDecorator::SourceDecorator;
    std::string read() override { return "解密(" + inner_->read() + ")"; }
};
class Compressed : public SourceDecorator {
public:
    using SourceDecorator::SourceDecorator;
    std::string read() override { return "解压(" + inner_->read() + ")"; }
};

// 代理：延迟加载
class Image {
public:
    virtual ~Image() = default;
    virtual void draw() = 0;
};
class RealImage : public Image {
    std::string path_;
public:
    explicit RealImage(std::string p) : path_(std::move(p)) {
        std::cout << "    [代理] 真正加载 " << path_ << "（假设很慢）\n";
    }
    void draw() override { std::cout << "    [代理] 绘制 " << path_ << "\n"; }
};
class LazyImage : public Image {
    std::string path_;
    std::unique_ptr<RealImage> real_;
public:
    explicit LazyImage(std::string p) : path_(std::move(p)) {
        std::cout << "    [代理] 创建 LazyImage（还没加载）\n";
    }
    void draw() override {
        if (!real_) real_ = std::make_unique<RealImage>(path_);
        real_->draw();
    }
};

// 桥接：抽象和实现各自独立演化，避免 N×M 类爆炸
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void render_circle(double r) = 0;
    virtual void render_square(double s) = 0;
};
class ConsoleRenderer : public Renderer {
public:
    void render_circle(double r) override { std::cout << "    [Console] 画圆 r=" << r << "\n"; }
    void render_square(double s) override { std::cout << "    [Console] 画方 s=" << s << "\n"; }
};
class SvgRenderer : public Renderer {
public:
    void render_circle(double r) override { std::cout << "    <circle r=\"" << r << "\"/>\n"; }
    void render_square(double s) override { std::cout << "    <rect w=\"" << s << "\"/>\n"; }
};
class BridgeShape {
protected:
    Renderer& r_;
public:
    explicit BridgeShape(Renderer& r) : r_(r) {}
    virtual ~BridgeShape() = default;
    virtual void draw() = 0;
};
class BridgeCircle : public BridgeShape {
    double rad_;
public:
    BridgeCircle(Renderer& r, double rad) : BridgeShape(r), rad_(rad) {}
    void draw() override { r_.render_circle(rad_); }
};

// 组合：树形结构，叶子和容器一视同仁
class FsNode {
public:
    virtual ~FsNode() = default;
    virtual size_t size() const = 0;
    virtual void print(int depth = 0) const = 0;
protected:
    static void indent(int d) { for (int i = 0; i < d; ++i) std::cout << "  "; }
};
class FsFile : public FsNode {
    std::string name_; size_t sz_;
public:
    FsFile(std::string n, size_t s) : name_(std::move(n)), sz_(s) {}
    size_t size() const override { return sz_; }
    void print(int d) const override { std::cout << "    "; indent(d);
                                       std::cout << name_ << " (" << sz_ << "B)\n"; }
};
class FsDir : public FsNode {
    std::string name_;
    std::vector<std::unique_ptr<FsNode>> children_;
public:
    explicit FsDir(std::string n) : name_(std::move(n)) {}
    FsDir& add(std::unique_ptr<FsNode> n) { children_.push_back(std::move(n)); return *this; }
    size_t size() const override {
        size_t t = 0;
        for (const auto& c : children_) t += c->size();     // 递归，叶子和目录写法一样
        return t;
    }
    void print(int d) const override {
        std::cout << "    "; indent(d); std::cout << name_ << "/ (" << size() << "B)\n";
        for (const auto& c : children_) c->print(d + 1);
    }
};

// 享元：共享不变的内部状态
class GlyphCache {
public:
    std::shared_ptr<const std::string> get(char c) {
        auto& slot = cache_[c];
        if (!slot) {
            slot = std::make_shared<std::string>(std::string("字形位图<") + c + ">");
            std::cout << "    [享元] 生成字形 '" << c << "'（只会看到一次）\n";
        }
        return slot;
    }
    size_t distinct() const { return cache_.size(); }
private:
    std::unordered_map<char, std::shared_ptr<const std::string>> cache_;
};

// #############################################################################
// 四、行为型模式
// #############################################################################

// 策略 —— 三种实现方式
// (a) 虚函数：运行期可换，有间接调用开销
class CompressPolicy {
public:
    virtual ~CompressPolicy() = default;
    virtual std::string compress(std::string_view s) = 0;
};
class GzipPolicy : public CompressPolicy {
public:
    std::string compress(std::string_view s) override { return "gzip(" + std::string(s) + ")"; }
};
class ZstdPolicy : public CompressPolicy {
public:
    std::string compress(std::string_view s) override { return "zstd(" + std::string(s) + ")"; }
};
class Archiver {
    std::unique_ptr<CompressPolicy> p_;
public:
    void set_policy(std::unique_ptr<CompressPolicy> p) { p_ = std::move(p); }
    std::string archive(std::string_view s) { return p_ ? p_->compress(s) : std::string(s); }
};
// (c) 模板策略：编译期确定，可内联，零开销
struct GzipTag { static std::string compress(std::string_view s) { return "gzip!(" + std::string(s) + ")"; } };
template <typename Policy>
class FastArchiver {
public:
    std::string archive(std::string_view s) { return Policy::compress(s); }
};

// 观察者 —— 信号槽
template <typename... Args>
class Signal {
public:
    using Slot  = std::function<void(Args...)>;
    using Token = std::size_t;

    Token connect(Slot s) { slots_.emplace(next_, std::move(s)); return next_++; }
    void  disconnect(Token t) { slots_.erase(t); }
    void  emit(Args... args) const { for (const auto& [id, s] : slots_) s(args...); }
    size_t count() const { return slots_.size(); }
private:
    std::map<Token, Slot> slots_;
    Token next_ = 0;
};

class Button {
public:
    Signal<int, int> on_click;
    void click(int x, int y) { on_click.emit(x, y); }
};

// 命令 + 撤销/重做
class Document {
public:
    std::string text;
};
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;
    virtual void undo() = 0;
    virtual std::string desc() const = 0;
};
class InsertText : public Command {
    Document& doc_; size_t pos_; std::string txt_;
public:
    InsertText(Document& d, size_t p, std::string t) : doc_(d), pos_(p), txt_(std::move(t)) {}
    void execute() override { doc_.text.insert(pos_, txt_); }
    void undo() override    { doc_.text.erase(pos_, txt_.size()); }
    std::string desc() const override { return "插入 \"" + txt_ + "\""; }
};
class History {
public:
    void run(std::unique_ptr<Command> c) {
        c->execute();
        std::cout << "    执行: " << c->desc() << "\n";
        done_.push_back(std::move(c));
        undone_.clear();                       // 新操作会清空重做栈
    }
    void undo() {
        if (done_.empty()) { std::cout << "    没有可撤销的操作\n"; return; }
        done_.back()->undo();
        std::cout << "    撤销: " << done_.back()->desc() << "\n";
        undone_.push_back(std::move(done_.back()));
        done_.pop_back();
    }
    void redo() {
        if (undone_.empty()) { std::cout << "    没有可重做的操作\n"; return; }
        undone_.back()->execute();
        std::cout << "    重做: " << undone_.back()->desc() << "\n";
        done_.push_back(std::move(undone_.back()));
        undone_.pop_back();
    }
private:
    std::vector<std::unique_ptr<Command>> done_, undone_;
};

// 模板方法（骨架固定，步骤可换）
class DataProcessor {
public:
    virtual ~DataProcessor() = default;
    void run() {                               // 这就是「模板方法」，不允许子类改流程
        auto raw   = read();
        auto valid = validate(std::move(raw));
        auto out   = transform(std::move(valid));
        write(out);
    }
protected:
    virtual std::string read() { return "raw-data"; }
    virtual std::string validate(std::string s) { return s; }        // 钩子，有默认实现
    virtual std::string transform(std::string s) = 0;                // 必须实现
    virtual void write(const std::string& s) { std::cout << "    输出: " << s << "\n"; }
};
class UpperProcessor : public DataProcessor {
protected:
    std::string transform(std::string s) override {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return s;
    }
};

// 状态机（用 variant，无堆分配，漏处理编译期报错）
template <class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

struct Idle {};
struct Connecting { int attempts; };
struct Connected  { int fd; };
struct Failed     { std::string reason; };
using ConnState = std::variant<Idle, Connecting, Connected, Failed>;

std::string state_name(const ConnState& s) {
    return std::visit(overloaded{
        [](Idle)                 { return std::string("空闲"); },
        [](const Connecting& c)  { return "连接中(第" + std::to_string(c.attempts) + "次)"; },
        [](const Connected& c)   { return "已连接(fd=" + std::to_string(c.fd) + ")"; },
        [](const Failed& f)      { return "失败(" + f.reason + ")"; },
    }, s);
}
ConnState on_event(const ConnState& s, std::string_view ev) {
    if (ev == "connect") {
        return std::visit(overloaded{
            [](Idle)                -> ConnState { return Connecting{1}; },
            [](const Connecting& c) -> ConnState { return c.attempts >= 3
                                                       ? ConnState{Failed{"重试超限"}}
                                                       : ConnState{Connecting{c.attempts + 1}}; },
            [](const Connected& c)  -> ConnState { return c; },
            [](const Failed& f)     -> ConnState { return Connecting{1}; },
        }, s);
    }
    if (ev == "success") return Connected{42};
    if (ev == "close")   return Idle{};
    return s;
}

// 访问者（variant 版：加新操作不用改类）
struct CircleV { double r; };
struct SquareV { double s; };
struct TriV    { double b, h; };
using ShapeV = std::variant<CircleV, SquareV, TriV>;

double area_of(const ShapeV& s) {
    return std::visit(overloaded{
        [](const CircleV& c) { return 3.14159265 * c.r * c.r; },
        [](const SquareV& q) { return q.s * q.s; },
        [](const TriV& t)    { return 0.5 * t.b * t.h; },
    }, s);
}

// 责任链（中间件管道版，像 Express / Gin）
struct Request {
    std::string path;
    bool authed = false;
    std::vector<std::string> trace;
};
using Next       = std::function<void()>;
using Middleware = std::function<void(Request&, Next)>;

class Pipeline {
public:
    Pipeline& use(Middleware m) { mws_.push_back(std::move(m)); return *this; }
    void run(Request& r) { invoke(r, 0); }
private:
    void invoke(Request& r, size_t i) {
        if (i >= mws_.size()) return;
        mws_[i](r, [this, &r, i] { invoke(r, i + 1); });
    }
    std::vector<Middleware> mws_;
};

// =============================================================================
// main
// =============================================================================
int main() {
    // =========================================================================
    demo::title("7.1 RAII —— C++ 最重要的模式");
    // =========================================================================
    {
        demo::section("资源在异常路径下也一定被释放");
        try {
            FileHandle f("pattern_tmp.txt", "w");
            if (f) std::fputs("test", f.get());
            throw std::runtime_error("模拟中途出错");
        } catch (const std::exception& e) {
            std::cout << "    捕获: " << e.what() << "（注意上面文件已经关了）\n";
        }
        std::remove("pattern_tmp.txt");

        demo::section("ScopeGuard —— 通用的「退出时执行」");
        {
            bool committed = false;
            ScopeGuard rollback{[&] {
                if (!committed) std::cout << "    [Guard] 事务回滚\n";
            }};
            std::cout << "    做一些可能失败的操作...\n";
            committed = true;
            rollback.dismiss();                      // 成功了，取消回滚
            std::cout << "    [Guard] 已提交，回滚被取消\n";
        }

        demo::line("【用在哪】文件、锁、socket、数据库连接、GPU 句柄、事务、临时状态恢复。");
        demo::line("标准库里的例子：unique_ptr / lock_guard / ifstream / jthread。");
    }

    // =========================================================================
    demo::title("7.2 Pimpl —— 隔断编译依赖");
    // =========================================================================
    {
        Widget w;
        w.do_work();
        w.do_work();
        demo::line("好处：");
        demo::line("  1) 头文件不用 #include 重量级依赖 -> 全项目编译快很多");
        demo::line("  2) 改实现不会导致下游全部重编译");
        demo::line("  3) ABI 稳定（类大小永远是一个指针）—— 做动态库必备");
        demo::line("代价：一次额外的堆分配 + 一次间接访问。热点小对象别用。");
        demo::line("注意：析构函数必须在 .cpp 里定义（= default 也要写在 cpp），");
        demo::line("      否则 unique_ptr<Impl> 在 Impl 不完整时析构会编译报错。");
    }

    // =========================================================================
    demo::title("7.3 CRTP 与 类型擦除");
    // =========================================================================
    {
        demo::section("CRTP：编译期多态，零虚函数开销");
        Money m{12345};
        m.print();
        demo::line("基类通过 static_cast<Derived&>(*this) 调到派生类，全部内联。");
        demo::line("【用在哪】给一堆类批量加公共功能（mixin）、表达式模板、单例基类。");
        demo::line("注意：C++20 有了 <=> 和 concepts 之后，CRTP 的一部分用途被取代了。");

        demo::section("类型擦除：值语义 + 多态 + 不需要继承");
        std::vector<Drawable> zoo;
        zoo.emplace_back(CatShape{});
        zoo.emplace_back(DogShape{});
        auto copy = zoo;                                   // 可以整体拷贝（虚继承做不到）
        for (const auto& d : copy) d.draw();
        demo::line("CatShape / DogShape 没有继承任何基类，只是「碰巧有 draw()」。");
        demo::line("【用在哪】std::function、std::any、插件系统、想要值语义容器时。");
    }

    // =========================================================================
    demo::title("7.4 单例");
    // =========================================================================
    {
        Logger::instance().log("第一条");
        Logger::instance().log("第二条");
        demo::line("Meyers Singleton：函数内 static，C++11 起标准保证线程安全的初始化。");
        demo::line("警告：单例是全局状态，会让单元测试变难、隐藏依赖、有析构顺序问题。");
        demo::line("      能用「依赖注入」（把对象当参数传进去）就别用单例。");
    }

    // =========================================================================
    demo::title("7.5 工厂（注册式）");
    // =========================================================================
    {
        auto& f = ShapeFactory::instance();
        std::cout << "    已注册类型: ";
        for (const auto& k : f.registered()) std::cout << k << " ";
        std::cout << "\n";

        for (auto key : {"circle", "square", "triangle"}) {
            if (auto s = f.create(key, 2.0)) {
                std::cout << "    " << key << " -> " << s->name()
                          << " 面积=" << s->area() << "\n";
            } else {
                std::cout << "    " << key << " -> 未注册\n";
            }
        }
        demo::line("加一个新形状只需要写 static Registrar<Triangle> reg{\"triangle\"};");
        demo::line("工厂本身一行都不用改 —— 这就是「开闭原则」。");
        demo::line("【用在哪】插件、配置驱动的对象创建、序列化反序列化、消息分发。");
    }

    // =========================================================================
    demo::title("7.6 建造者");
    // =========================================================================
    {
        auto req = HttpRequest::Builder{}
                       .url("https://api.example.com/v1/users")
                       .method("POST")
                       .header("Content-Type", "application/json")
                       .header("Authorization", "Bearer xxx")
                       .body(R"({"name":"张三"})")
                       .timeout(3000)
                       .build();
        req.dump();

        demo::line("【用在哪】构造参数多、大部分可选、且有构造后不可变需求的对象。");
        demo::line("现代替代：参数少的时候 C++20 指定初始化器更轻量：");
        demo::line("  struct Opt { int retries = 3; bool verbose = false; };");
        demo::line("  configure({.retries = 5});");
    }

    // =========================================================================
    demo::title("7.7 结构型：适配器 / 装饰器 / 代理 / 桥接 / 组合 / 享元");
    // =========================================================================
    {
        demo::section("适配器 —— 包一层，让老接口符合新接口");
        LegacyPrinter lp;
        LegacyAdapter adapter{&lp};
        IWriter& w = adapter;
        w.write("通过适配器写出去");
        demo::line("标准库例子：stack / queue 就是 deque 的适配器。");

        demo::section("装饰器 —— 层层包裹添加职责");
        std::unique_ptr<DataSource> src =
            std::make_unique<Encrypted>(
                std::make_unique<Compressed>(
                    std::make_unique<RawSource>()));
        std::cout << "    " << src->read() << "\n";
        demo::line("调用链: Encrypted::read -> Compressed::read -> RawSource::read");
        demo::line("【用在哪】IO 流（缓冲/加密/压缩）、HTTP 中间件、UI 控件加边框滚动条。");

        demo::section("代理 —— 延迟加载");
        LazyImage img{"huge.png"};
        std::cout << "    （此时还没有真正加载）\n";
        img.draw();                                  // 第一次 draw 才加载
        img.draw();                                  // 第二次直接用
        demo::line("其它代理类型：远程代理(RPC stub)、保护代理(权限)、缓存代理、智能指针。");

        demo::section("桥接 —— 避免 N×M 类爆炸");
        ConsoleRenderer cr;
        SvgRenderer     sr;
        BridgeCircle{cr, 5}.draw();
        BridgeCircle{sr, 5}.draw();
        demo::line("3 种形状 × 4 种渲染器：继承要写 12 个类，桥接只要 3+4=7 个。");

        demo::section("组合 —— 树形结构统一处理");
        auto root = std::make_unique<FsDir>("project");
        auto src_dir = std::make_unique<FsDir>("src");
        src_dir->add(std::make_unique<FsFile>("main.cpp", 1200));
        src_dir->add(std::make_unique<FsFile>("util.cpp", 800));
        root->add(std::move(src_dir));
        root->add(std::make_unique<FsFile>("README.md", 300));
        root->print(0);
        std::cout << "    总大小: " << root->size() << " B\n";

        demo::section("享元 —— 共享不变的内部状态");
        GlyphCache gc;
        std::string text = "aabbaa";
        for (char c : text) gc.get(c);
        std::cout << "    文本 \"" << text << "\" 共 " << text.size()
                  << " 个字符，实际只生成了 " << gc.distinct() << " 个字形\n";
        demo::line("【用在哪】字体渲染、游戏里的树/草模型、字符串驻留(intern)、连接池。");
    }

    // =========================================================================
    demo::title("7.8 策略 —— 三种实现方式对比");
    // =========================================================================
    {
        demo::section("(a) 虚函数：运行期可切换");
        Archiver arc;
        arc.set_policy(std::make_unique<GzipPolicy>());
        std::cout << "    " << arc.archive("data") << "\n";
        arc.set_policy(std::make_unique<ZstdPolicy>());
        std::cout << "    " << arc.archive("data") << "\n";

        demo::section("(b) std::function：最灵活，可以直接塞 lambda");
        std::function<std::string(std::string_view)> policy =
            [](std::string_view s) { return "lambda(" + std::string(s) + ")"; };
        std::cout << "    " << policy("data") << "\n";

        demo::section("(c) 模板策略：编译期确定，可内联，零开销");
        FastArchiver<GzipTag> fast;
        std::cout << "    " << fast.archive("data") << "\n";

        demo::line("怎么选：");
        demo::line("  运行期要换（用户配置、插件）      -> 虚函数 或 std::function");
        demo::line("  编译期就定死，且在热点路径        -> 模板策略");
        demo::line("  只是一个小回调                    -> 直接传 lambda，别搞类");
    }

    // =========================================================================
    demo::title("7.9 观察者（信号槽）");
    // =========================================================================
    {
        Button btn;
        auto t1 = btn.on_click.connect([](int x, int y) {
            std::cout << "    观察者A: 点击于 (" << x << "," << y << ")\n";
        });
        btn.on_click.connect([](int, int) { std::cout << "    观察者B: 记录日志\n"; });

        btn.click(10, 20);
        SHOW(btn.on_click.count());

        btn.on_click.disconnect(t1);
        std::cout << "    断开 A 之后:\n";
        btn.click(30, 40);

        demo::line("【用在哪】GUI 事件、模型-视图同步、发布订阅、状态变更通知。");
        demo::line("生命周期陷阱：观察者先于被观察者析构 -> 悬垂回调。");
        demo::line("解决：connect 返回 token 让调用方 disconnect（上面这样），");
        demo::line("      或者观察者列表存 weak_ptr，emit 时 lock 一下，失效的自动清理。");
    }

    // =========================================================================
    demo::title("7.10 命令 + 撤销/重做");
    // =========================================================================
    {
        Document doc;
        History h;
        h.run(std::make_unique<InsertText>(doc, 0, "Hello"));
        h.run(std::make_unique<InsertText>(doc, 5, " World"));
        SHOW(doc.text);
        h.undo();  SHOW(doc.text);
        h.undo();  SHOW(doc.text);
        h.redo();  SHOW(doc.text);
        h.undo();  h.undo();

        demo::line("【用在哪】编辑器撤销栈、事务、任务队列、宏录制、请求重放。");
        demo::line("要点：命令对象要自带「怎么撤销」所需的全部信息。");
    }

    // =========================================================================
    demo::title("7.11 模板方法");
    // =========================================================================
    {
        UpperProcessor p;
        p.run();
        demo::line("父类定死【流程】，子类填【步骤】。和策略的区别：");
        demo::line("  模板方法：继承，编译期绑定，控制反转在父类");
        demo::line("  策略    ：组合，可运行期换，控制权在调用方");
        demo::line("【用在哪】框架的生命周期钩子（onInit/onUpdate/onDestroy）、测试夹具。");
    }

    // =========================================================================
    demo::title("7.12 状态机（variant 版）");
    // =========================================================================
    {
        ConnState s = Idle{};
        for (auto ev : {"connect", "connect", "success", "close"}) {
            std::cout << "    " << state_name(s) << "  --[" << ev << "]-->  ";
            s = on_event(s, ev);
            std::cout << state_name(s) << "\n";
        }
        demo::line("好处：");
        demo::line("  1) 每个状态可以带自己独有的数据（Connecting 有 attempts，Connected 有 fd）");
        demo::line("  2) 不合法的状态根本表示不出来（不像 enum + 一堆散落的成员变量）");
        demo::line("  3) 漏处理某个状态，visit 编译期就报错");
        demo::line("  4) 没有堆分配，全在栈上");
    }

    // =========================================================================
    demo::title("7.13 访问者（variant 版）");
    // =========================================================================
    {
        std::vector<ShapeV> shapes{CircleV{1.0}, SquareV{2.0}, TriV{3.0, 4.0}};
        for (const auto& s : shapes) std::cout << "    面积 = " << area_of(s) << "\n";

        demo::line("两种写法的取舍（这是个经典的「表达式问题」）：");
        demo::line("  虚函数继承：加【新类型】容易（新写一个派生类），");
        demo::line("              加【新操作】难（要改所有类）");
        demo::line("  variant   ：加【新操作】容易（新写一个 visit 函数），");
        demo::line("              加【新类型】难（要改所有 visit）");
        demo::line("类型集合固定（AST 节点、协议消息、状态）-> variant");
        demo::line("类型会不断增加（插件、图形对象）        -> 虚函数");
    }

    // =========================================================================
    demo::title("7.14 责任链（中间件管道）");
    // =========================================================================
    {
        Pipeline pipe;
        pipe.use([](Request& r, Next next) {
                r.trace.push_back("日志:开始");
                next();                                    // 调用下一层
                r.trace.push_back("日志:结束");            // 下一层返回后还能做事
            })
            .use([](Request& r, Next next) {
                if (r.path.rfind("/admin", 0) == 0 && !r.authed) {
                    r.trace.push_back("鉴权:拒绝");
                    return;                                // 不调 next()，链就断了
                }
                r.trace.push_back("鉴权:通过");
                next();
            })
            .use([](Request& r, Next) {
                r.trace.push_back("处理:返回200");
            });

        for (auto [path, authed] : {std::pair{"/public", false}, std::pair{"/admin", false}}) {
            Request r{path, authed, {}};
            pipe.run(r);
            std::cout << "    " << path << " (authed=" << authed << "): ";
            for (const auto& t : r.trace) std::cout << t << " -> ";
            std::cout << "END\n";
        }

        demo::line("【用在哪】Web 框架中间件、日志/鉴权/限流/压缩层、事件处理链、拦截器。");
    }

    // =========================================================================
    demo::title("7.15 模式选择速查 & 反模式提醒");
    // =========================================================================
    {
        demo::line("管资源                        -> RAII（永远先想这个）");
        demo::line("运行期换算法                  -> 策略（std::function / 虚函数）");
        demo::line("编译期定死算法且在热点路径    -> 模板策略");
        demo::line("一对多通知                    -> 观察者 / 信号槽");
        demo::line("需要撤销重做 / 请求排队       -> 命令");
        demo::line("隐藏实现、降低编译依赖        -> Pimpl");
        demo::line("类型集合封闭 + 多种操作       -> variant + visit");
        demo::line("类型集合开放 + 操作固定       -> 虚函数继承");
        demo::line("要值语义又要多态              -> 类型擦除");
        demo::line("两个维度各自变化(N×M)         -> 桥接");
        demo::line("树形结构统一处理              -> 组合");
        demo::line("动态叠加职责                  -> 装饰器");
        demo::line("请求可能被多个处理者处理      -> 责任链 / 中间件");
        demo::line("");
        demo::line("【反模式提醒】");
        demo::line("不要为了用模式而用模式。很多 GoF 模式在现代 C++ 里已经被语言吃掉了：");
        demo::line("  简单 Strategy / Command  -> 一个 lambda 就够");
        demo::line("  Visitor                  -> std::variant + std::visit");
        demo::line("  比较运算符 mixin (CRTP)  -> auto operator<=>() = default");
        demo::line("  简单 Builder             -> C++20 指定初始化器");
        demo::line("  Iterator                 -> 直接满足 ranges 概念");
        demo::line("模式是用来沟通的词汇，不是必须完成的仪式。");
    }

    std::cout << "\n第 7 章结束。\n";
    return 0;
}
