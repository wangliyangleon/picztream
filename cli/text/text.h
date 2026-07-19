#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// cli 层的纯文本工具:UTF-8 解码、终端显示宽度计算、按显示宽度截断/补齐、
// 文件大小格式化、`~` 路径展开。全部是确定性纯函数,不碰终端 io、不依赖
// core——显示列宽是终端呈现层的概念(core 禁止渲染依赖),所以放 cli/ 而
// 不是 core/。这一层单独成库(cli_text)是因为它是唯一值得独立单元测试的
// 一块(见 cli/tests/text_test.cpp);历史上 6.4.3 的 banner 截断 bug 就
// 出在这里的显示宽度逻辑上。
namespace pzt::cli::text {

// 判断一个 Unicode 码点在终端里是否按"宽字符"(占 2 列)显示。
bool is_wide_codepoint(char32_t cp);

// 解码 s[pos] 起的一个 UTF-8 字符,返回(码点, 字节数)。
std::pair<char32_t, int> decode_utf8_at(const std::string& s, std::size_t pos);

// 给定一个 UTF-8 码点的起始字节,返回后面还需要几个续字节(0-3)。
int utf8_continuation_bytes(unsigned char lead);

// 按终端实际显示宽度截断到 max_width 列以内(宽字符占 2 列)。
std::string truncate_text(const std::string& s, std::size_t max_width);

// 字符串的终端显示宽度(宽字符计 2 列)。
std::size_t display_width(const std::string& s);

// 截断/补空格到固定的显示宽度。
std::string pad_to(const std::string& s, std::size_t width);

// 按显示宽度硬换行(宽字符占 2 列)，超出的部分接到下一行，不丢字。
std::vector<std::string> wrap_text(const std::string& s, std::size_t max_width);

// 文件大小格式化成 B/KB/MB/GB。
std::string format_size(std::int64_t bytes);

// 展开路径开头的 `~`/`~/...`(只处理这两种形式,不处理 `~user`)。
std::string expand_home_path(const std::string& path);

// 按 token 边界换行(只在 token 之间的空格上断,不切断单个 token),宽度按
// display_width 计(CJK token 占 2 列)。跟 wrap_text 不同——那个按显示宽度
// 硬换行、允许把一个词从中间切断。
std::vector<std::string> wrap_tokens(const std::vector<std::string>& tokens, std::size_t max_width);

// `/` 开头的控制台输入解析成命令名(不含前导 `/`) + 剩余参数,第一个空白
// 是分界,命令名与参数之间允许多个空格。
std::pair<std::string, std::string> split_console_command(const std::string& input);

// 从串首取一个"范围 token":普通按第一个空白切;`#"..."` 带引号的标签名整
// 体当一个 token(引号内空格不算分界),未闭合引号时退化成按空格切。返回值
// 保留开头的 `#` 和引号,解引号交给调用方。
std::pair<std::string, std::string> take_scope_token(const std::string& s);

}  // namespace pzt::cli::text
