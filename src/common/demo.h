// =============================================================================
// demo.h —— 教学示例用的小工具（打印分节标题等）
// =============================================================================
#pragma once

#include <iostream>
#include <string>
#include <string_view>

namespace demo {

// 打印一级标题
inline void title(std::string_view t) {
    std::cout << "\n";
    std::cout << "==============================================================\n";
    std::cout << "  " << t << "\n";
    std::cout << "==============================================================\n";
}

// 打印二级小节
inline void section(std::string_view t) {
    std::cout << "\n--- " << t << " ---\n";
}

// 打印 "表达式 = 值"
template <typename T>
void show(std::string_view expr, const T& value) {
    std::cout << "  " << expr << " = " << value << "\n";
}

inline void line(std::string_view s = "") { std::cout << "  " << s << "\n"; }

} // namespace demo

// 方便宏：SHOW(x) 会打印 "x = <值>"
#define SHOW(...) demo::show(#__VA_ARGS__, (__VA_ARGS__))
