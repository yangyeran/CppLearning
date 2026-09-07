# CppLearning —— C++ 复习 + Linux 网络编程 完整工程

从「语法忘光了」到「手写 HTTP 服务器」的一整条学习链路。
**15 个可独立运行的程序**，全部在 Visual Studio 2022 (MSVC 14.40) 上编译验证通过，
同一份代码也能在 Linux / WSL 上编译运行。

---

## 快速开始

### 方式 A：Visual Studio 打开文件夹（最省事）

1. Visual Studio 2022 → **文件 → 打开 → 文件夹** → 选中本目录
2. VS 自动识别 `CMakeLists.txt` 并配置
3. 顶部启动项下拉框选任意 `chXX_...`，按 **F5** 运行/调试

### 方式 B：生成 .sln 解决方案

```bat
scripts\build_vs.bat
```

产物：

- `build\CppLearning.sln` —— 双击用 VS 打开，15 个项目一目了然
- `build\bin\*.exe` —— 所有可执行文件

右键任一项目 → **设为启动项目** → F5 单步调试。

### 方式 C：命令行

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
build\bin\ch01_cpp11.exe
```

### Linux / WSL

```bash
cmake -B build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build build-linux -j$(nproc)
./build-linux/bin/ch11_multiplex 8890 epoll     # epoll 只有 Linux 有
```

没装 WSL 的话：管理员 PowerShell 执行 `wsl --install`，重启即可。

---

## 学习文档

```
docs/CppLearningGuide.pdf     ← 完整手册（125 页），打印/平板阅读
docs/CppLearningGuide.html    ← 同内容 HTML，带目录，浏览器直接看
docs/CppLearningGuide.md      ← 同内容 Markdown，编辑器里看
docs/parts/*.md               ← 分章节源文件
```

改了 `docs/parts/` 里的内容后重新生成：

```bat
python scripts\build_pdf.py
```

（需要 `python -m pip install markdown pygments`；PDF 用本机 Edge/Chrome 无头模式打印）

---

## 章节一览

| 章 | 主题 | 程序 | 怎么跑 |
|----|------|------|--------|
| 0 | 语法回忆速通 | `ch00_refresher` | 直接运行 |
| 1 | C++11 | `ch01_cpp11` | 直接运行 |
| 2 | C++14 | `ch02_cpp14` | 直接运行 |
| 3 | C++17 | `ch03_cpp17` | 直接运行 |
| 4 | C++20 | `ch04_cpp20` | 直接运行 |
| 5 | C++23 | `ch05_cpp23` | 直接运行（不支持的特性自动跳过并提示） |
| 6 | 标准库 STL | `ch06_stl` | 直接运行 |
| 7 | 设计模式 | `ch07_patterns` | 直接运行 |
| 8 | 网络基础 | `ch08_net_basics` | 直接运行 |
| 9 | TCP | `ch09_tcp_server` `ch09_tcp_client` | 见下 |
| 10 | UDP | `ch10_udp_server` `ch10_udp_client` | 见下 |
| 11 | IO 多路复用 | `ch11_multiplex` | 见下 |
| 12 | 迷你 HTTP 服务器 | `ch12_http_server` | 见下 |

### 网络章节的跑法

**第 9 章 TCP** —— 开两个终端：

```bat
:: 终端 1（并发模型可选 iterative / thread / pool）
build\bin\ch09_tcp_server.exe 8888 thread

:: 终端 2
build\bin\ch09_tcp_client.exe 127.0.0.1 8888          :: 交互模式，打字回车
build\bin\ch09_tcp_client.exe 127.0.0.1 8888 bench    :: 粘包演示 + RTT 压测
```

**第 10 章 UDP**：

```bat
build\bin\ch10_udp_server.exe 9999
build\bin\ch10_udp_client.exe 127.0.0.1 9999 demo     :: 消息边界 / 超时 / 吞吐
```

**第 11 章 IO 多路复用** —— 聊天室，开 2~3 个 telnet 互相广播：

```bat
build\bin\ch11_multiplex.exe explain          :: 只看原理讲解，不启动服务
build\bin\ch11_multiplex.exe 8890 select
build\bin\ch11_multiplex.exe 8890 poll
:: 另开终端: telnet 127.0.0.1 8890   （随便打字，会广播给其他终端）
```

epoll 模式需要 Linux：

```bash
./build-linux/bin/ch11_multiplex 8890 epoll
```

**第 12 章 HTTP 服务器**：

```bat
build\bin\ch12_http_server.exe 8080
```

然后浏览器打开 <http://127.0.0.1:8080>，或者：

```bash
curl http://127.0.0.1:8080/api/time
curl "http://127.0.0.1:8080/api/hello?name=%E4%B8%96%E7%95%8C"
curl -X POST -d "hello world" http://127.0.0.1:8080/api/echo
curl http://127.0.0.1:8080/admin/panel                              # 401
curl -H "Authorization: Bearer secret123" http://127.0.0.1:8080/admin/panel
```

---

## 目录结构

```
CppLearning/
├── CMakeLists.txt                顶层构建（含 add_chapter 辅助函数）
├── README.md                     本文件
├── docs/
│   ├── CppLearningGuide.pdf      完整手册
│   ├── CppLearningGuide.html
│   ├── CppLearningGuide.md
│   └── parts/                    分章节 Markdown 源
├── scripts/
│   ├── build_vs.bat              生成 .sln 并编译全部
│   └── build_pdf.py              parts/*.md -> md + html + pdf
└── src/
    ├── common/
    │   ├── demo.h                示例用的打印工具（title/section/SHOW 宏）
    │   └── net_compat.h          ★ 跨平台 socket 兼容层（POSIX 语义 + Winsock 适配）
    ├── ch00_refresher/           语法回忆
    ├── ch01_cpp11/ … ch05_cpp23/ 各版本新特性
    ├── ch06_stl/                 标准库
    ├── ch07_patterns/            设计模式
    ├── ch08_net_basics/          网络基础（字节序 / 地址 / DNS / 握手图解）
    ├── ch09_tcp/                 tcp_server.cpp + tcp_client.cpp
    ├── ch10_udp/                 udp_server.cpp + udp_client.cpp
    ├── ch11_multiplex/           select / poll / epoll 聊天室
    └── ch12_http/                迷你 HTTP 服务器
```

---

## 关于 `net_compat.h`

网络代码按 **Linux/POSIX 语义**编写（这是行业标准写法），
`src/common/net_compat.h` 负责在 Windows 上抹平差异：

| 项目 | Linux | Windows |
|------|-------|---------|
| 初始化 | 不需要 | `WSAStartup` / `WSACleanup` |
| 句柄 | `int` | `SOCKET` |
| 无效值 | `-1` | `INVALID_SOCKET` |
| 关闭 | `close()` | `closesocket()` |
| 错误码 | `errno` | `WSAGetLastError()` |
| 非阻塞 | `fcntl(O_NONBLOCK)` | `ioctlsocket(FIONBIO)` |
| 多路复用 | select/poll/**epoll** | select/WSAPoll/**IOCP** |

它还提供了 RAII 的 `netc::Socket` 和 `netc::Startup`，以及
`send_all` / `set_nonblocking` / `set_reuse_addr` / `set_tcp_nodelay` 等常用封装。

**唯一无法抹平的是 `epoll`** —— 那是 Linux 独有的，Windows 对应技术是 IOCP，
模型完全不同（Proactor vs Reactor）。第 11 章的 epoll 模式必须在 Linux/WSL 上跑。

---

## 编译选项

```cmake
/utf-8            源码和执行字符集都按 UTF-8（中文注释与输出不乱码）
/W4               高警告等级
/Zc:__cplusplus   让 __cplusplus 宏报告真实标准版本
/permissive-      严格标准一致性
```

第 5 章（C++23）单独用 `/std:c++latest`，源码里用 `__has_include` /
`__cpp_lib_xxx` 做了特性探测，**编译器不支持的特性会打印提示而不是编译失败**。

GCC/Clang 侧建议：

```bash
g++ -std=c++20 -Wall -Wextra -Wpedantic -g -fsanitize=address,undefined main.cpp
```

---

## 遇到问题

| 现象 | 处理 |
|------|------|
| `Address already in use` | 端口被占。换个端口，或等 TIME_WAIT 结束（约 60s） |
| 中文输出乱码 | 终端代码页问题，执行 `chcp 65001`；VS 内置终端默认就是 UTF-8 |
| 服务端 Ctrl+C 后端口没释放 | 代码已设 `SO_REUSEADDR`，直接重启即可 |
| `ch05_cpp23` 大量「暂不支持」 | 正常，MSVC 14.40 只实现了部分 C++23；升级 VS 可解锁更多 |
| epoll 模式提示不支持 | Windows 没有 epoll，装 WSL 后在 Linux 侧编译运行 |
| PDF 生成失败 | 关掉正在阅读该 PDF 的程序再重跑；或用浏览器打开 HTML 后 Ctrl+P 另存 |
