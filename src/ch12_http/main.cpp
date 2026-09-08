// =============================================================================
// 第 12 章 —— 综合实战：迷你 HTTP 服务器
//
// 把前面所有东西串起来：
//   现代 C++    : RAII / string_view / optional / 结构化绑定 / lambda / 智能指针
//   设计模式    : 责任链(中间件) / 策略(路由处理器) / RAII / 工厂
//   网络编程    : socket / 非阻塞 / 线程池 / HTTP 协议解析 / 粘包处理
//
// 运行:
//   ch12_http_server.exe 8080
// 然后浏览器打开 http://127.0.0.1:8080
//   或 curl http://127.0.0.1:8080/api/time
//   或 curl -X POST -d "hello" http://127.0.0.1:8080/api/echo
// =============================================================================

#include "demo.h"
#include "net_compat.h"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstring>
#include <ctime>
#include <stdexcept>

static std::atomic<bool> g_running{true};
extern "C" void on_signal(int) { g_running = false; }

// #############################################################################
// 一、HTTP 报文的数据结构
// #############################################################################

struct HttpRequest {
    std::string method;                                  // GET / POST / ...
    std::string path;                                    // /api/users
    std::string query;                                   // id=1&name=x
    std::string version;                                 // HTTP/1.1
    std::map<std::string, std::string> headers;          // 头部（键统一转小写）
    std::string body;

    std::optional<std::string> header(std::string_view k) const {
        std::string key(k);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto it = headers.find(key);
        if (it == headers.end()) return std::nullopt;
        return it->second;
    }

    // URL 百分号解码：%E4%B8%96 -> 原始字节，'+' -> 空格
    static std::string url_decode(std::string_view sv) {
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        std::string out;
        out.reserve(sv.size());
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == '+') { out.push_back(' '); continue; }
            if (sv[i] == '%' && i + 2 < sv.size()) {
                int hi = hex(sv[i + 1]), lo = hex(sv[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    out.push_back(static_cast<char>(hi * 16 + lo));
                    i += 2;
                    continue;
                }
            }
            out.push_back(sv[i]);
        }
        return out;
    }

    // 解析 query string  id=1&name=x  ->  {{"id","1"},{"name","x"}}
    std::map<std::string, std::string> params() const {
        std::map<std::string, std::string> out;
        size_t start = 0;
        while (start < query.size()) {
            size_t amp = query.find('&', start);
            if (amp == std::string::npos) amp = query.size();
            std::string_view kv(query.data() + start, amp - start);
            size_t eq = kv.find('=');
            if (eq != std::string_view::npos) {
                out.emplace(url_decode(kv.substr(0, eq)), url_decode(kv.substr(eq + 1)));
            } else if (!kv.empty()) {
                out.emplace(url_decode(kv), "");
            }
            start = amp + 1;
        }
        return out;
    }
};

struct HttpResponse {
    int         status = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    // 链式设置，用起来像 Builder
    HttpResponse& set_status(int code, std::string text) {
        status = code; status_text = std::move(text); return *this;
    }
    HttpResponse& set_header(std::string k, std::string v) {
        headers[std::move(k)] = std::move(v); return *this;
    }
    HttpResponse& text(std::string s) {
        body = std::move(s);
        headers["Content-Type"] = "text/plain; charset=utf-8";
        return *this;
    }
    HttpResponse& html(std::string s) {
        body = std::move(s);
        headers["Content-Type"] = "text/html; charset=utf-8";
        return *this;
    }
    HttpResponse& json(std::string s) {
        body = std::move(s);
        headers["Content-Type"] = "application/json; charset=utf-8";
        return *this;
    }

    // 序列化成真正要发出去的字节
    std::string serialize() const {
        std::ostringstream os;
        os << "HTTP/1.1 " << status << " " << status_text << "\r\n";
        for (const auto& [k, v] : headers) os << k << ": " << v << "\r\n";
        // Content-Length 告诉对方 body 有多长 —— 这是 HTTP 解决「粘包」的办法
        os << "Content-Length: " << body.size() << "\r\n";
        os << "Connection: keep-alive\r\n";
        os << "\r\n";                    // 空行分隔头部和正文
        os << body;
        return os.str();
    }
};

// #############################################################################
// 二、HTTP 请求解析
//
// HTTP/1.1 报文格式：
//   GET /path?query HTTP/1.1\r\n          <- 请求行
//   Host: example.com\r\n                 <- 头部，一行一个
//   Content-Length: 5\r\n
//   \r\n                                  <- 空行，表示头部结束
//   hello                                 <- 正文（长度由 Content-Length 决定）
//
// 【关键】这就是为什么解析器必须处理「粘包」：
//   TCP 只给你字节流，你得先找到 \r\n\r\n 确定头部完整了，
//   再根据 Content-Length 判断 body 是否收全。
// #############################################################################

// 返回值：nullopt = 数据还不完整，请继续收；有值 = 解析成功，并从 buf 里消费掉
std::optional<HttpRequest> try_parse(std::string& buf) {
    // ① 找头部结束标志
    size_t header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        if (buf.size() > 16 * 1024) throw std::runtime_error("请求头过大");
        return std::nullopt;                                    // 头部还没收全
    }

    HttpRequest req;
    std::istringstream is(buf.substr(0, header_end));
    std::string line;

    // ② 请求行： METHOD SP PATH SP VERSION
    if (!std::getline(is, line)) return std::nullopt;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream ls(line);
        std::string target;
        ls >> req.method >> target >> req.version;
        if (req.method.empty() || target.empty()) throw std::runtime_error("请求行格式错误");

        size_t q = target.find('?');
        if (q == std::string::npos) {
            req.path = target;
        } else {
            req.path  = target.substr(0, q);
            req.query = target.substr(q + 1);
        }
    }

    // ③ 头部： Key: Value
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // 键统一转小写（HTTP 头部名不区分大小写）
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // 去掉值前后的空白
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        val.erase(val.begin(), std::find_if(val.begin(), val.end(), not_space));
        val.erase(std::find_if(val.rbegin(), val.rend(), not_space).base(), val.end());
        req.headers[std::move(key)] = std::move(val);
    }

    // ④ 正文：长度由 Content-Length 决定
    size_t body_start = header_end + 4;
    size_t content_length = 0;
    if (auto cl = req.header("content-length")) {
        try { content_length = static_cast<size_t>(std::stoull(*cl)); }
        catch (...) { throw std::runtime_error("Content-Length 非法"); }
        if (content_length > 8 * 1024 * 1024) throw std::runtime_error("请求体过大");
    }

    if (buf.size() < body_start + content_length) {
        return std::nullopt;                                    // body 还没收全，继续等
    }

    req.body = buf.substr(body_start, content_length);
    buf.erase(0, body_start + content_length);                  // 消费掉这个请求
    return req;
}

// #############################################################################
// 三、路由 + 中间件（策略模式 + 责任链模式）
// #############################################################################

using Handler    = std::function<void(const HttpRequest&, HttpResponse&)>;
using Next       = std::function<void()>;
using Middleware = std::function<void(const HttpRequest&, HttpResponse&, Next)>;

class Router {
public:
    Router& get(std::string path, Handler h)  { routes_[{"GET",  std::move(path)}] = std::move(h); return *this; }
    Router& post(std::string path, Handler h) { routes_[{"POST", std::move(path)}] = std::move(h); return *this; }
    Router& use(Middleware m) { middlewares_.push_back(std::move(m)); return *this; }

    void handle(const HttpRequest& req, HttpResponse& res) const {
        // 责任链：依次穿过所有中间件，最后走到真正的处理器
        run_chain(req, res, 0);
    }

private:
    void run_chain(const HttpRequest& req, HttpResponse& res, size_t idx) const {
        if (idx < middlewares_.size()) {
            middlewares_[idx](req, res, [this, &req, &res, idx] {
                run_chain(req, res, idx + 1);
            });
            return;
        }
        dispatch(req, res);
    }

    void dispatch(const HttpRequest& req, HttpResponse& res) const {
        auto it = routes_.find({req.method, req.path});
        if (it != routes_.end()) { it->second(req, res); return; }

        // 405：路径存在但方法不对，比笼统的 404 更友好
        for (const auto& [key, h] : routes_) {
            if (key.second == req.path) {
                res.set_status(405, "Method Not Allowed").text("405 方法不允许\n");
                return;
            }
        }
        res.set_status(404, "Not Found").html(
            "<h1>404 Not Found</h1><p>路径 " + req.path + " 不存在</p>");
    }

    std::map<std::pair<std::string, std::string>, Handler> routes_;
    std::vector<Middleware>                                middlewares_;
};

// #############################################################################
// 四、线程池（复用第 9 章的实现）
// #############################################################################
class ThreadPool {
public:
    explicit ThreadPool(size_t n) {
        for (size_t i = 0; i < n; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lk(m_);
                        cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }
    ~ThreadPool() {
        { std::lock_guard lk(m_); stop_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) if (t.joinable()) t.join();
    }
    void submit(std::function<void()> f) {
        { std::lock_guard lk(m_); tasks_.push(std::move(f)); }
        cv_.notify_one();
    }
private:
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        m_;
    std::condition_variable           cv_;
    bool                              stop_ = false;
};

// #############################################################################
// 五、连接处理
// #############################################################################
struct Stats {
    std::atomic<long long> requests{0};
    std::atomic<long long> connections{0};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

void serve_connection(netc::Socket conn, const std::string& peer,
                      const Router& router, Stats& stats) {
    netc::set_tcp_nodelay(conn.get(), true);
    ++stats.connections;

    std::string buf;
    char        chunk[8192];
    int         kept_alive = 0;

    for (;;) {
        // 先尝试从已有缓冲里解析（可能上一轮读进来了多个请求 —— 这就是 HTTP 流水线）
        for (;;) {
            std::optional<HttpRequest> req;
            try {
                req = try_parse(buf);
            } catch (const std::exception& e) {
                HttpResponse bad;
                bad.set_status(400, "Bad Request").text(std::string("400 ") + e.what() + "\n");
                netc::send_all(conn.get(), bad.serialize());
                return;
            }
            if (!req) break;                             // 数据不完整，去读更多

            HttpResponse res;
            auto t0 = std::chrono::steady_clock::now();
            router.handle(*req, res);
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count();

            ++stats.requests;
            std::cout << "[" << peer << "] " << req->method << " " << req->path
                      << " -> " << res.status << "  (" << us << " us)\n";

            if (!netc::send_all(conn.get(), res.serialize())) return;

            // HTTP/1.1 默认长连接；客户端要求 close 就断开
            if (auto c = req->header("connection")) {
                std::string v = *c;
                std::transform(v.begin(), v.end(), v.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (v == "close") return;
            }
            if (++kept_alive > 100) return;              // 防止单连接无限占用
        }

        long n = netc::recv_n(conn.get(), chunk, sizeof(chunk));
        if (n == 0) return;                              // 对端关闭
        if (n < 0) {
            if (netc::interrupted()) continue;
            return;
        }
        buf.append(chunk, static_cast<size_t>(n));
    }
}

// #############################################################################
// 六、组装并启动
// #############################################################################
int main(int argc, char** argv) {
    try {
        netc::Startup net_guard;
        std::signal(SIGINT, on_signal);

        uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;
        Stats    stats;

        // ---------------------------------------------------------------------
        // 中间件（责任链）—— 注册顺序就是执行顺序
        // ---------------------------------------------------------------------
        Router router;

        router.use([](const HttpRequest& req, HttpResponse& res, Next next) {
            // 中间件 1：统一加安全响应头
            next();                                       // 先让后面的处理
            res.set_header("Server", "MiniHttp/1.0");     // 再统一加工结果
            res.set_header("X-Content-Type-Options", "nosniff");
            (void)req;
        });

        router.use([](const HttpRequest& req, HttpResponse& res, Next next) {
            // 中间件 2：简单鉴权 —— /admin 开头的路径需要 token
            if (req.path.rfind("/admin", 0) == 0) {
                auto auth = req.header("authorization");
                if (!auth || *auth != "Bearer secret123") {
                    res.set_status(401, "Unauthorized")
                       .set_header("WWW-Authenticate", "Bearer")
                       .json(R"({"error":"需要有效的 token"})");
                    return;                               // 不调 next()，链在这里断开
                }
            }
            next();
        });

        // ---------------------------------------------------------------------
        // 路由
        // ---------------------------------------------------------------------
        router.get("/", [](const HttpRequest&, HttpResponse& res) {
            res.html(R"(<!doctype html>
<html><head><meta charset="utf-8"><title>Mini HTTP Server</title>
<style>body{font-family:system-ui,sans-serif;max-width:44rem;margin:3rem auto;line-height:1.7}
code{background:#f0f0f0;padding:.15rem .4rem;border-radius:4px}
a{color:#0066cc}</style></head>
<body>
<h1>C++ 迷你 HTTP 服务器</h1>
<p>这是《C++ 复习与网络编程》第 12 章的实战产物，用不到 400 行 C++ 写成。</p>
<h2>可用接口</h2>
<ul>
  <li><a href="/api/time">GET /api/time</a> — 返回服务器时间 (JSON)</li>
  <li><a href="/api/stats">GET /api/stats</a> — 运行统计</li>
  <li><a href="/api/hello?name=世界">GET /api/hello?name=世界</a> — query 参数演示</li>
  <li><code>POST /api/echo</code> — 原样回显请求体</li>
  <li><a href="/admin/panel">GET /admin/panel</a> — 需要 token，会返回 401</li>
  <li><a href="/nope">GET /nope</a> — 演示 404</li>
</ul>
<h2>命令行测试</h2>
<pre>curl http://127.0.0.1:8080/api/time
curl -X POST -d "hello world" http://127.0.0.1:8080/api/echo
curl -H "Authorization: Bearer secret123" http://127.0.0.1:8080/admin/panel</pre>
</body></html>)");
        });

        router.get("/api/time", [](const HttpRequest&, HttpResponse& res) {
            std::time_t t = std::time(nullptr);
            char buf[64]{};
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
            res.json(std::string(R"({"time":")") + buf + R"(","unix":)" +
                     std::to_string(static_cast<long long>(t)) + "}");
        });

        router.get("/api/stats", [&stats](const HttpRequest&, HttpResponse& res) {
            auto up = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now() - stats.start).count();
            res.json("{\"requests\":" + std::to_string(stats.requests.load()) +
                     ",\"connections\":" + std::to_string(stats.connections.load()) +
                     ",\"uptime_seconds\":" + std::to_string(up) + "}");
        });

        router.get("/api/hello", [](const HttpRequest& req, HttpResponse& res) {
            auto p = req.params();
            auto it = p.find("name");
            std::string name = (it == p.end() || it->second.empty()) ? "陌生人" : it->second;
            res.json(R"({"greeting":"你好, )" + name + R"("})");
        });

        router.post("/api/echo", [](const HttpRequest& req, HttpResponse& res) {
            res.text("你发来的 " + std::to_string(req.body.size()) + " 字节:\n" + req.body + "\n");
        });

        router.get("/admin/panel", [](const HttpRequest&, HttpResponse& res) {
            res.json(R"({"secret":"只有带对 token 才看得到这行"})");
        });

        // ---------------------------------------------------------------------
        // 监听并 accept
        // ---------------------------------------------------------------------
        auto listener = netc::Socket::tcp();
        netc::set_reuse_addr(listener.get(), true);
        sockaddr_in addr = netc::make_addr_v4(nullptr, port);
        if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
                == netc::kSocketError) netc::throw_socket_error("bind()");
        if (::listen(listener.get(), 128) == netc::kSocketError)
            netc::throw_socket_error("listen()");

        unsigned nthreads = std::thread::hardware_concurrency();
        ThreadPool pool(nthreads ? nthreads : 4);

        demo::title("迷你 HTTP 服务器已启动");
        std::cout << "  地址: http://127.0.0.1:" << port << "\n";
        std::cout << "  线程: " << (nthreads ? nthreads : 4) << "\n";
        std::cout << "  按 Ctrl+C 退出\n\n";

        while (g_running) {
            sockaddr_in peer{};
            socklen_t   len = sizeof(peer);
            netc::socket_t c = ::accept(listener.get(),
                                        reinterpret_cast<sockaddr*>(&peer), &len);
            if (c == netc::kInvalidSocket) {
                if (netc::interrupted()) continue;
                if (!g_running) break;
                continue;
            }
            auto sp   = std::make_shared<netc::Socket>(netc::Socket{c});
            auto peer_str = netc::addr_to_string(peer);
            pool.submit([sp, peer_str, &router, &stats] {
                serve_connection(std::move(*sp), peer_str, router, stats);
            });
        }

        std::cout << "\n[服务器] 已停止。共处理 " << stats.requests.load()
                  << " 个请求，" << stats.connections.load() << " 条连接。\n";

    } catch (const std::exception& e) {
        std::cerr << "[错误] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
