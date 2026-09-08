# 第 16 章：Reactor 网络库 + 三个示例程序

# 把库单独编译成静态库，三个 exe 共用
add_library(reactor_lib STATIC
    reactor/reactor.cpp
    reactor/reactor.h
)
target_include_directories(reactor_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}           # 让别的章节能写 #include "reactor/reactor.h"
    ${CMAKE_CURRENT_SOURCE_DIR}/reactor   # 库内部可以写 #include "reactor.h"
    ${CMAKE_SOURCE_DIR}/src/common
)
set_target_properties(reactor_lib PROPERTIES FOLDER "Chapters/ch16")

if (WIN32)
    target_link_libraries(reactor_lib PUBLIC ws2_32)
    # WSAPoll 需要 Vista 及以上；同时抬高 FD_SETSIZE 以防回退到 select
    target_compile_definitions(reactor_lib PUBLIC _WIN32_WINNT=0x0601 FD_SETSIZE=1024)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(reactor_lib PUBLIC Threads::Threads)
endif()

# 三个可执行文件
foreach(app reactor_selftest echo_server chat_server)
    add_executable(ch16_${app} ${app}.cpp)
    target_link_libraries(ch16_${app} PRIVATE reactor_lib)
    set_target_properties(ch16_${app} PROPERTIES FOLDER "Chapters/ch16")
endforeach()
