add_chapter(ch05_cpp23 main.cpp)

# C++23 需要最新标准开关。不同编译器的支持程度差异较大，
# 源码里已经用 __has_include / __cpp_lib_xxx 做了保护，缺特性只会跳过不会编译失败。
set_property(TARGET ch05_cpp23 PROPERTY CXX_STANDARD 23)
if (MSVC)
    target_compile_options(ch05_cpp23 PRIVATE /std:c++latest)
endif()
