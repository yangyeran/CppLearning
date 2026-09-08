cmake_minimum_required(VERSION 3.20)
project(CppLearning LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)          # 主体用 C++20；C++23 章节单独提升
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件按配置分目录输出。
# 必须分开：否则 Debug 与 Release 同名 exe 会互相覆盖，
# 你以为在跑 Release，实际跑的是上一次构建的 Debug 版本（性能数据全废）。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY               ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG         ${CMAKE_BINARY_DIR}/bin/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE       ${CMAKE_BINARY_DIR}/bin/Release)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/bin/RelWithDebInfo)

if (MSVC)
    # /utf-8      源文件与执行字符集都按 UTF-8 处理（中文注释不乱码）
    # /Zc:__cplusplus  让 __cplusplus 宏报告真实标准版本
    # /permissive- 严格标准一致性
    # /EHsc       标准 C++ 异常模型
    add_compile_options(/utf-8 /W4 /Zc:__cplusplus /permissive- /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# 辅助函数：添加一个「章节」可执行文件
#   add_chapter(名字 源文件...)
# ---------------------------------------------------------------------------
function(add_chapter name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src/common)
    set_target_properties(${name} PROPERTIES FOLDER "Chapters")
    if (WIN32)
        # 网络章节需要链接 Winsock2
        target_link_libraries(${name} PRIVATE ws2_32)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(${name} PRIVATE Threads::Threads)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 各章节
# ---------------------------------------------------------------------------
add_subdirectory(src/ch00_refresher)     # 0. 语法回忆速通
add_subdirectory(src/ch01_cpp11)         # 1. C++11
add_subdirectory(src/ch02_cpp14)         # 2. C++14
add_subdirectory(src/ch03_cpp17)         # 3. C++17
add_subdirectory(src/ch04_cpp20)         # 4. C++20
add_subdirectory(src/ch05_cpp23)         # 5. C++23
add_subdirectory(src/ch06_stl)           # 6. 标准库
add_subdirectory(src/ch07_patterns)      # 7. 设计模式
add_subdirectory(src/ch08_net_basics)    # 8. 网络基础 / 字节序 / 地址
add_subdirectory(src/ch09_tcp)           # 9. TCP 回显 服务端+客户端
add_subdirectory(src/ch10_udp)           # 10. UDP
add_subdirectory(src/ch11_multiplex)     # 11. IO 多路复用 select/poll/epoll
add_subdirectory(src/ch12_http)          # 12. 综合实战：迷你 HTTP 服务器
add_subdirectory(src/ch13_pitfalls)      # 13. C++ 易错点大全
add_subdirectory(src/ch14_smartptr)      # 14. 智能指针深入 + 手写实现
add_subdirectory(src/ch15_stl_source)    # 15. STL 源码剖析（手写实现）
add_subdirectory(src/ch16_reactor)       # 16. Reactor 完整实现 + 回显/聊天室
add_subdirectory(src/ch17_mysql)         # 17. MySQL 命令 + 原理（B+树/MVCC/LRU）
add_subdirectory(src/ch18_solutions)     # 18. 练习题答案：语法/STL/设计模式
add_subdirectory(src/ch19_net_solutions) # 19. 练习题答案：网络（复用 ch16 库）

message(STATUS "")
message(STATUS "==========================================================")
message(STATUS " CppLearning 配置完成")
message(STATUS " 可执行文件输出目录: ${CMAKE_BINARY_DIR}/bin/<配置名>")
message(STATUS " 编译器: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "==========================================================")
message(STATUS "")
