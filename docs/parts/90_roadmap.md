# 附录 A · 复习计划（4 周主线 + 3 周进阶）

按每天 2~3 小时估算。赶时间就跳过标 ☆ 的部分。

## 第 1 周：语法回炉

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 1 | 第 0 章全部 | 跑 `ch00_refresher`，把「指针 vs 引用」那节的代码默写一遍 |
| 2 | 1.1~1.4（auto / decltype / 移动语义 / 完美转发） | 自己写一个带资源的类，实现五个特殊成员函数，用打印验证什么时候调哪个 |
| 3 | 1.5~1.7（初始化 / lambda / nullptr 等） | 用 lambda 重写几个 STL 算法调用 |
| 4 | 1.8~1.10（constexpr / 范围 for / 智能指针） | 把一段用 `new/delete` 的老代码改成智能指针 |
| 5 | 1.11~1.14（变参模板 / default delete / 多线程） | 写一个 10 行的线程安全计数器，对比有锁/无锁/无保护三种结果 |
| 6 | 第 2 章 C++14 全部 | 写一个用移动捕获的异步任务 |
| 7 | 复习 + 第 1 周小结 | 合上文档，默写：移动构造、完美转发模板、RAII 类 |

## 第 2 周：现代特性 + 标准库

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 8 | 3.1~3.5（结构化绑定 / if 初始化 / CTAD / if constexpr / 折叠） | 用 `if constexpr` 写一个通用的 `to_string` |
| 9 | 3.6~3.8（optional / variant / string_view） | 用 `variant` 写一个表达式求值器 |
| 10 | 3.9~3.11（filesystem / 并行算法 / 杂项） | 写个小工具：递归统计目录下各类文件的数量和大小 |
| 11 | 4.1~4.2（concepts / ranges） | 用 ranges 重写一段 for 循环密集的数据处理 |
| 12 | 4.3~4.6（协程 / `<=>` / span / format） | 手写一个 Generator，产出素数序列 |
| 13 | 第 6 章 STL 上半（容器） | 写个小 benchmark：vector vs list vs deque 遍历/插入耗时 |
| 14 | 第 6 章 STL 下半（算法 / 字符串 / chrono / random） | 不看文档，用算法库完成：找出数组中出现次数最多的 3 个元素 |

## 第 3 周：设计模式 + 网络基础

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 15 | 7.1~7.3（RAII / Pimpl / CRTP / 类型擦除） | 给一个现有类加 Pimpl |
| 16 | 7.4~7.5（创建型 + 结构型） | 实现一个注册式工厂，加新类型不改工厂 |
| 17 | 7.6~7.11（行为型） | 用 `variant` 实现一个订单状态机 |
| 18 | 8.1~8.5（什么是网络 / 分层 / IP 端口 / 字节序） | 跑 `ch08_net_basics`，用 hexdump 观察字节序 |
| 19 | 8.6~8.10（sockaddr / DNS / TCP vs UDP / 握手挥手） | 用 Wireshark 抓一次 `curl example.com`，找出三次握手的三个包 |
| 20 | 8.11~8.16（状态机 / socket API / 选项 / 排障） | `ss -tanp` 观察本机各种连接状态；故意不设 SO_REUSEADDR 重现「地址已占用」 |
| 21 | 复习 + 第 3 周小结 | 手画：三次握手、四次挥手、TCP 状态机 |

## 第 4 周：网络编程实战

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 22 | 第 9 章 TCP 上半（服务端套路 / 三种并发模型） | 跑三种模式，用两个客户端验证 `iterative` 模式确实会阻塞 |
| 23 | 第 9 章 TCP 下半（粘包 / 客户端 / 实测） | **自己实现长度前缀协议**，替换掉回显逻辑 |
| 24 | 第 10 章 UDP | 用 UDP 写一个简易「局域网设备发现」（广播 + 应答） |
| 25 | 11.1~11.3（为什么要多路复用 / 三者对比 / 三种写法） | 跑 select 和 poll 版聊天室，加一个 `/who` 命令列出在线用户 |
| 26 | 11.4~11.6（LT vs ET / Reactor / 实际难点） | ☆ 在 WSL 上跑 epoll 版；把 LT 改成 ET，故意只读一次，观察连接假死 |
| 27 | 第 12 章 HTTP 上半（协议 / 解析器 / 架构） | 加一个新路由 + 一个新中间件（比如请求计数限流） |
| 28 | 第 12 章 HTTP 下半 + 总复习 | ☆ 把线程池换成 epoll 事件循环；用 `wrk` 压测对比 QPS |

至此主线完成：你能独立写出一个多线程 HTTP 服务器，并说清 TCP 的每个状态。

---

## 第 5 周：查漏补缺 + 智能指针

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 29 | 第 13 章 1~2 组（初始化与类型 / 指针与生命周期） | 把前 4 周自己写的代码全部开 `-fsanitize=address,undefined` 重跑一遍 |
| 30 | 第 13 章 3 组（类与对象） | 给第 7 章的类补齐五法则；用 `copy-and-swap` 重写一个赋值运算符 |
| 31 | 第 13 章 4 组（现代特性的坑） | 找出自己代码里所有 `[&]` 捕获，判断哪些会悬垂 |
| 32 | 第 13 章 5~6 组（并发 / 其它） | 把第 9 章的 `thread` 版服务器改成 `jthread`，加 `stop_token` 优雅退出 |
| 33 | 第 14 章 A~B（为什么需要 / 三种指针用法） | 给 `FILE*`、`socket` 各写一个 RAII 封装 |
| 34 | 第 14 章 C（手写实现） | **不看源码，自己写一遍 `MyUniquePtr`**，然后对照 |
| 35 | 复习 + 第 5 周小结 | 默写：控制块结构、`weak_ptr::lock()` 的 CAS 循环、`enable_shared_from_this` 原理 |

## 第 6 周：STL 源码

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 36 | 第 15 章 1~3（六大组件 / 迭代器 traits / allocator） | 用 tag dispatch、`if constexpr`、concepts 三种方式各写一遍 `advance` |
| 37 | 第 15 章 4（vector） | **手写 `MyVector`**，用探针类型验证 reserve 和 noexcept 的效果 |
| 38 | 第 15 章 5~6（string SSO / list 哨兵） | 手写带 SSO 的 string，测出你的实现的阈值 |
| 39 | 第 15 章 7（红黑树） | 手写插入 + 不变式校验；☆ 挑战：补上删除 |
| 40 | 第 15 章 8~9（哈希表 / deque） | 手写哈希表；给自定义类型写一个正确的 `hash_combine` |
| 41 | 第 15 章 10~11（introsort / 性能实测） | 在 **Release** 下跑基准，和文档里的数字对比你的机器 |
| 42 | 复习 + 第 6 周小结 | 默写容器选型决策树和各容器的迭代器失效规则 |

## 第 7 周：Reactor + MySQL

| 天 | 内容 | 动手任务 |
|----|------|---------|
| 43 | 第 16 章 1~4（模型演进 / 类结构 / 主从 Reactor / Buffer） | 读 `reactor.h` 全部注释；画出类之间的持有关系图 |
| 44 | 第 16 章 5~6（Channel::tie / Poller / LT vs ET） | 故意注释掉 `disableWriting()`，观察 CPU 打满 |
| 45 | 第 16 章 7（EventLoop / 跨线程唤醒） | 注释掉 `wakeup()`，观察跨线程 send 要等 10 秒才生效 |
| 46 | 第 16 章 8~9（TcpConnection / Acceptor 生产细节） | 给 `TcpConnection` 加一个空闲连接超时踢出功能 |
| 47 | 第 16 章 10~11（广播两种解法 / 自测） | **把聊天室的解法 A 改成解法 B**（无锁广播） |
| 48 | 第 17 章 1~5（SQL / B+ 树 / 聚簇索引 / 索引失效） | 手写 B+ 树的删除；用你的机器算一遍容量表 |
| 49 | 第 17 章 6~11（事务 / MVCC / 锁 / 日志 / Buffer Pool / EXPLAIN） | 装一个 MySQL，亲手制造一次死锁并用 `SHOW ENGINE INNODB STATUS` 读懂它 |

☆ 标记的是可跳过的挑战项。第 17 章与 C++ 部分独立，可以随时插入。

---

# 附录 B · 练习题（由易到难）

## B.1 语法与 STL

1. 写一个 `SafeVector<T>`，`operator[]` 带边界检查（越界抛异常），
   其余接口转发给内部 `std::vector`。要求支持移动、不支持拷贝。

2. 实现 `template <typename... Ts> std::string join(const char* sep, Ts&&... args)`，
   用折叠表达式把任意个可打印参数拼成字符串。

3. 用 `std::variant` 实现一个 JSON 值类型：
   `using Json = std::variant<std::nullptr_t, bool, double, std::string,
   std::vector<Json>, std::map<std::string, Json>>;`
   （提示：递归类型需要包一层 `struct`）写一个 `to_string(const Json&)`。

4. 不用 `std::sort`，自己实现一个满足 `std::sortable` 概念约束的快排。

5. 写一个 LRU 缓存：`get`/`put` 都是 O(1)。
   （提示：`std::list` + `std::unordered_map<K, list::iterator>`）

## B.2 设计模式

6. 实现一个线程安全的**事件总线**：
   `bus.subscribe<OrderCreated>(handler)` / `bus.publish(OrderCreated{...})`。
   要求订阅者用 `weak_ptr` 持有，失效自动清理。

7. 用**类型擦除**实现一个 `AnyCallable`，能装下任意签名兼容的可调用对象，
   支持拷贝，且不用 `std::function`。

8. 给第 12 章的 HTTP 服务器加一个**装饰器**：给任意 Handler 套上「记录耗时」的外壳。

## B.3 网络编程

9. **长度前缀协议**：改造 `ch09_tcp`，实现
   `[4字节长度][JSON 消息体]` 的协议。要求处理任意的拆包/粘包情况
   （可以在客户端故意一次只发 1 字节来测试）。

10. **心跳与超时**：给聊天室服务器加上「30 秒没消息就踢下线」。
    用一个小顶堆管理超时时间，作为 `epoll_wait` 的 timeout 参数。

11. **文件传输**：写一个能传大文件（>2GB）的客户端/服务端。
    要求显示进度、支持断点续传（协议里带 offset）、校验 MD5。

12. **简易 RPC**：定义
    `[4字节长度][4字节请求ID][方法名\0][参数...]`，
    服务端维护一个 `map<string, function>` 分发，客户端支持并发请求
    （靠请求 ID 匹配响应）。

13. ☆ **把 HTTP 服务器改成 epoll 事件驱动**。
    难点：写缓冲区管理（发不完的数据要存起来 + 注册 EPOLLOUT）。
    做完用 `wrk -t4 -c1000 -d30s` 对比改造前后的 QPS。

14. ☆ **实现最小可用的 WebSocket**：
    HTTP Upgrade 握手（算 `Sec-WebSocket-Accept`）+ 帧解析（掩码、分片、opcode）。

---

# 附录 C · 高频面试题清单

**C++ 语言**

- `auto` 的推导规则？`auto&&` 和 `T&&` 有什么关系？
- 移动语义解决了什么问题？为什么移动构造要加 `noexcept`？
- `std::move` 到底做了什么？（答：只是个 static_cast）
- 完美转发的原理？引用折叠规则？为什么必须用 `std::forward`？
- `unique_ptr` 和 `shared_ptr` 的区别？`shared_ptr` 线程安全吗？
  （答：**控制块的引用计数是原子的，但指向的对象不是**）
- 循环引用怎么产生、怎么解决？
- 虚函数怎么实现的？虚表在哪？为什么构造函数不能是虚函数？
- 为什么基类析构要 virtual？什么时候可以不 virtual？
- 什么是对象切片？
- `const` 成员函数能修改成员吗？（答：`mutable` 成员可以）
- 空类的 `sizeof` 是多少？（答：1，为了保证不同对象地址不同）
- 五法则 / 零法则是什么？
- `if constexpr` 和普通 `if` 的区别？
- `std::optional` / `variant` / `any` 各自的适用场景？
- 静态局部变量的初始化是线程安全的吗？（答：C++11 起是）

**STL**

- `vector` 扩容策略？为什么是 1.5 或 2 倍？扩容后迭代器还有效吗？
- `map` 和 `unordered_map` 怎么选？各自最坏复杂度？
- `emplace_back` 和 `push_back` 的区别？
- 什么时候迭代器会失效？（各容器分别说）
- `std::remove` 真的删除元素了吗？
- `accumulate` 和 `reduce` 的区别？

**并发**

- 五种内存序的区别？什么时候能用 `relaxed`？
- 条件变量为什么要带谓词？什么是虚假唤醒？
- 死锁的四个必要条件？怎么避免？
- 什么是伪共享（false sharing）？怎么解决？（答：cache line 对齐）
- 无锁编程的 ABA 问题？

**网络（重点）**

- 三次握手为什么不是两次？不是四次？
- 四次挥手为什么是四次？
- **TIME_WAIT 的作用？为什么是 2MSL？大量 TIME_WAIT 怎么办？**
- **大量 CLOSE_WAIT 说明什么？**（答：应用层收到 FIN 后没 close，是 bug）
- TCP 怎么保证可靠传输？（序号 + 确认 + 重传 + 校验和 + 流控 + 拥塞控制）
- 流量控制和拥塞控制的区别？
  （答：**流控是照顾接收方**，靠滑动窗口；**拥塞控制是照顾网络**，靠拥塞窗口）
- 拥塞控制四个阶段？（慢启动、拥塞避免、快重传、快恢复）
- **粘包是什么？三种解决方案？**
- `select` / `poll` / `epoll` 的区别？epoll 为什么快？
- **LT 和 ET 的区别？用 ET 有什么注意事项？**
- Reactor 和 Proactor 的区别？
- 什么是 C10K 问题？
- `recv` 返回 0 意味着什么？返回 -1 呢？
- 为什么 `send` 要循环调用？
- `SO_REUSEADDR` 和 `SO_REUSEPORT` 的区别？
- Nagle 算法是什么？什么时候要关掉？
- HTTP/1.1 vs HTTP/2 vs HTTP/3 的关键差异？
- 什么是队头阻塞？HTTP/2 和 HTTP/3 分别怎么处理？
- 从浏览器输入 URL 到页面显示，发生了什么？
  （DNS → TCP 握手 → TLS 握手 → HTTP 请求 → 服务端处理 → 响应 →
  解析 HTML → 加载资源 → 渲染）

---

# 附录 D · 推荐资源

## 书

| 书 | 适合阶段 | 说明 |
|----|---------|------|
| 《Effective Modern C++》Scott Meyers | **现在就看** | 42 条现代 C++ 最佳实践，本文档很多内容的出处 |
| 《C++ Primer》(第5版) | 查漏补缺 | 大部头，当字典用 |
| 《UNIX 网络编程 卷1》Stevens | **网络必读** | socket 编程圣经，虽然老但概念不过时 |
| 《TCP/IP 详解 卷1》Stevens | 深入协议 | 想搞懂协议细节看这本 |
| 《Linux 多线程服务端编程》陈硕 | **实战首选** | 中文，muduo 作者，讲清楚了为什么这么设计 |
| 《C++ Concurrency in Action》(第2版) | 并发深入 | 内存模型讲得最清楚 |
| 《C++ Templates》(第2版) | 模板深入 | 需要写库的时候看 |

## 网站

- **cppreference.com** —— 唯一权威的在线手册，有中文版但建议看英文
- **isocpp.org/faq** —— 官方 FAQ
- **C++ Core Guidelines** —— Bjarne 和 Herb Sutter 维护的编码规范
- **Compiler Explorer (godbolt.org)** —— **看代码编译成什么汇编，学优化必备**
- **quick-bench.com** —— 在线微基准测试
- **cppinsights.io** —— **看编译器把 lambda / 范围 for / 模板展开成了什么**，学语法糖神器

## 视频 / 会议

- **CppCon** (YouTube) —— 每年最重要的 C++ 会议
- Back to Basics 系列 —— CppCon 的入门专题，讲得非常清楚

## 开源项目（按学习难度排序）

| 项目 | 学什么 |
|------|--------|
| `cpp-httplib` | 单头文件 HTTP 库，简洁 |
| **muduo** | **Reactor 模型的教科书实现，配套中文书** |
| `libuv` | 跨平台事件循环（Node.js 底座） |
| Redis (`ae.c`) | 极简事件循环，几百行 |
| `fmt` | `std::format` 的原型，学现代 C++ 库设计 |
| Nginx | 工业级主从 Reactor |
| Boost.Asio | 现代异步模型 + 协程 |

---

# 附录 E · 工程速查

## 编译命令

```bash
# MSVC
cl /std:c++20 /utf-8 /W4 /EHsc /Zc:__cplusplus /permissive- main.cpp

# GCC / Clang
g++ -std=c++20 -Wall -Wextra -Wpedantic -O2 main.cpp

# 开发期强烈建议加 sanitizer
g++ -std=c++20 -g -fsanitize=address,undefined main.cpp
```

## CMake 最小模板

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyProject LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(app main.cpp)

if (MSVC)
    target_compile_options(app PRIVATE /utf-8 /W4 /Zc:__cplusplus /permissive-)
    target_link_libraries(app PRIVATE ws2_32)          # 网络程序需要
else()
    target_compile_options(app PRIVATE -Wall -Wextra)
    find_package(Threads REQUIRED)
    target_link_libraries(app PRIVATE Threads::Threads)
endif()
```

## 本工程的命令

```bat
scripts\build_vs.bat              :: 生成 .sln + 编译全部
build\bin\Debug\ch01_cpp11.exe          :: 运行某一章

:: 或用 VS「打开文件夹」直接识别 CMakeLists.txt
```

```bash
# WSL / Linux
cmake -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j$(nproc)
./build-linux/bin/ch11_multiplex 8890 epoll
```

---

**文档到此结束。**

记住一件事：**这份文档的价值不在于读完，而在于配套代码你改过多少遍。**
把 `src/` 下的程序当成沙盒，随便改、随便崩，改坏了 `git checkout` 就回来了。
