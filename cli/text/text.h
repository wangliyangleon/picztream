#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// take_scope_token 切出来的这个 token 是不是一个**显式的作用域标记**
// (`*` / `.` / `#标签名`)。`/ai_eval` 用它分流:是标记就走批量提交，不是
// 就说明用户没写作用域、整段剩余文本是对当前这一张的额外指引。
//
// 这里只认标记的**形状**,不判断作用域是否有效(标签存不存在、有没有视图
// 可指): 那归 core::scope::resolve，是它那份头注释在管的事。
//
// 于是这里与 core 之间有一条**必须一起改**的线:语法的权威出处是
// core/scope/scope.h,再加一支作用域写法时,这个函数要同步认它的形状,否则
// `/ai_eval <新写法>` 会静默走成"对当前这一张"。之所以仍留在 cli:分流发生
// 在**查库之前**(core::resolve 要 db 与 project_id,而这里只是决定调哪个
// handler),让 cli 为了问一句"这是不是个标记"先开库是本末倒置。
bool is_batch_scope_token(const std::string& token);

// `/pick <N>` 这类"命令只接受一个正整数参数"的通用解析器:去掉首尾空格之
// 后必须恰好是一串十进制数字(不接受符号、小数点、多余 token)，值 > 0 且
// 不超过 int 上限。nullopt = 不合法。
//
// 不拆成"是不是数字"/"是不是正数"/"有没有多写"三条各自判断 - D-1 要的是
// 一句"N 不是一个正整数就报错"，四种坏输入(`/pick`、`/pick 0`、
// `/pick -1`、`/pick abc`)在这里是同一件事的四个例子，不是四条分支。
std::optional<int> parse_positive_int_arg(const std::string& rest);

}  // namespace pzt::cli::text
