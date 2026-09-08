// =============================================================================
// demo.h —— 教学示例用的小工具（打印分节标题等）
// =============================================================================
#pragma once

#include <iostream>
#include <string>
#include <string_view>
#include <cstddef>

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

// -----------------------------------------------------------------------------
// 中文对齐辅助
//
// 为什么需要它：std::setw 数的是**字节数**，而一个中文字在 UTF-8 里占 3 字节、
// 在终端里却只占 2 个字符宽。所以 setw(20) 遇到中文标签会严重错位。
// 这里按「显示宽度」补空格：CJK 字符算 2 列，ASCII 算 1 列。
// -----------------------------------------------------------------------------
inline std::size_t display_width(std::string_view s) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t   len;      // 这个 UTF-8 序列占几个字节
        unsigned long cp;       // 码点
        if (c < 0x80)        { len = 1; cp = c; }
        else if (c < 0xE0)   { len = 2; cp = c & 0x1Fu; }
        else if (c < 0xF0)   { len = 3; cp = c & 0x0Fu; }
        else                 { len = 4; cp = c & 0x07u; }
        for (std::size_t k = 1; k < len && i + k < s.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);

        // 东亚宽字符的主要区间：CJK 统一汉字、全角标点、中文标点、假名等
        bool wide = (cp >= 0x1100  && cp <= 0x115F) ||   // 韩文字母
                    (cp >= 0x2E80  && cp <= 0xA4CF) ||   // CJK 部首 ~ 彝文
                    (cp >= 0xAC00  && cp <= 0xD7A3) ||   // 韩文音节
                    (cp >= 0xF900  && cp <= 0xFAFF) ||   // CJK 兼容汉字
                    (cp >= 0xFE30  && cp <= 0xFE6F) ||   // CJK 兼容form
                    (cp >= 0xFF00  && cp <= 0xFF60) ||   // 全角字符
                    (cp >= 0xFFE0  && cp <= 0xFFE6) ||
                    (cp >= 0x20000 && cp <= 0x3FFFD);    // CJK 扩展 B~
        width += wide ? 2 : 1;
        i += len;
    }
    return width;
}

// 把 s 按显示宽度补足到 cols 列（用于表格左列对齐）
inline std::string pad(std::string_view s, std::size_t cols) {
    std::string out(s);
    std::size_t w = display_width(s);
    if (w < cols) out.append(cols - w, ' ');
    return out;
}

} // namespace demo

// 方便宏：SHOW(x) 会打印 "x = <值>"
#define SHOW(...) demo::show(#__VA_ARGS__, (__VA_ARGS__))
