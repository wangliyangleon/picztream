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

// 两行版本:line1 画在 banner_row,line2 画在 banner_row+1(调用方所在的
// 布局要预留这第二行,见 cli/commands/browse.cpp 的 kBannerRows),读一个
// 字节的语义跟单行版本一样。用于选项太多、一行放不下的顶层二级菜单(带
// 编号的选项 + 字母/Esc 这些固定操作分两行放),不是所有 prompt 都需要
// 这个,大多数子菜单/确认提示继续用单行版本。
char prompt_and_read_key_2line(const std::string& line1, const std::string& line2, int banner_row,
                                int start_col, int content_cols);

// poll() stdin,timeout_ms 内有可读数据返回 true(--debug 面板刷新用)。
bool stdin_ready(int timeout_ms);

// F-25：`/dedup`、大批量导出这类长阻塞操作(几秒到几十秒)冻结主循环期
// 间,用户习惯性按的键会一直留在 tty 的输入缓冲区里,操作结束、循环继
// 续读键时会被一次性回放、当成正常按键处理(可能连按出误标签/误退
// 出)——见 docs/M3_Dedup_PRD.md"风险与待确认问题"里"阻塞期间的输入
// 缓冲行为"那一条,一直没收口。长阻塞调用返回之后、继续读键之前调用
// 这个函数清空缓冲区,把冻结期间的按键当成没发生过处理。
void flush_pending_input();

// 从 banner 那一行读一整行 UTF-8 文本。Esc/EOF 返回 nullopt(取消);Enter
// 返回缓冲区内容(可能是空串,调用方决定空值是否合法)。
std::optional<std::string> read_text_line(const std::string& prompt, int banner_row, int start_col,
                                           int content_cols);

// buffer 为空时整行显示 placeholder，用户一开始输入(buffer 非空)
// placeholder 就整个让位给 buffer 本身，不像 read_text_line 那样有个常
// 驻前缀。Esc 仍然返回 nullopt(取消)；buffer 为空时按 Enter 返回空字符
// 串(不是取消)——调用方决定空字符串是不是合法输入。
std::optional<std::string> read_text_line_with_placeholder(const std::string& placeholder,
                                                             int banner_row, int start_col,
                                                             int content_cols);

}  // namespace pzt::cli::ui
