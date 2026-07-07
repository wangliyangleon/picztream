#pragma once

#include <optional>
#include <string>

// cli 层的终端 io 原语:直接 write()/read()/poll() 到 stdout/stdin,加上
// 光标定位、边框画线、单键读取、整行 UTF-8 文本输入。依赖 cli/text(显示
// 宽度/补齐),但不依赖 core——这些函数都不碰 core 类型,保持 ui -> text
// 这条单向依赖干净。
//
// 全部走 write(fd, ...) 而不是 fprintf/std::cout:render_rgba_via_tmpfile/
// clear_placement 内部也是直接 write() 到同一个 fd,如果光标定位这些文字
// 改用带缓冲的 stdio 输出,两条路径谁先真正落地到终端就不可控了,布局会
// 错位(见 6.4.1)。
namespace pzt::cli::ui {

// 直接把字符串 write() 到 STDOUT。
void write_stdout(const std::string& s);

// 移动光标到 (row, col)(1-based,ANSI CUP)。
void move_cursor(int row, int col);

// 画边框横线:起止两端用 left_char/right_char,mid_offset >= 0 时在那一列
// 插 mid_char(竖线交汇处),其余用 "─" 填满。
void draw_hline(int row, int start_col, int width, const std::string& left_char,
                const std::string& right_char, int mid_offset = -1,
                const std::string& mid_char = "");

// 画一行左右两侧的边框竖线;mid_offset >= 0 时中间(图片区/信息栏分隔处)
// 再画一条。
void draw_vlines(int row, int start_col, int width, int mid_offset = -1);

// 阻塞读一个字节;EOF/出错返回 0x1B(跟 Esc 一样当取消处理)。
char read_one_byte();

// 把提示行画到 banner 那一行(补齐到 content_cols),再读一个字节。
char prompt_and_read_key(const std::string& line, int banner_row, int start_col, int content_cols);

// poll() stdin,timeout_ms 内有可读数据返回 true(--debug 面板刷新用)。
bool stdin_ready(int timeout_ms);

// 从 banner 那一行读一整行 UTF-8 文本。Esc/EOF 返回 nullopt(取消);Enter
// 返回缓冲区内容(可能是空串,调用方决定空值是否合法)。
std::optional<std::string> read_text_line(const std::string& prompt, int banner_row, int start_col,
                                           int content_cols);

}  // namespace pzt::cli::ui
