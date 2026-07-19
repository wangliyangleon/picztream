#include "cli/text/text.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace pzt::cli::text {

// 判断一个 Unicode 码点在终端里是否按"宽字符"(占 2 列)显示——覆盖这个
// 项目实际会用到的范围(CJK 统一表意文字、全角标点、假名、谚文等),不追
// 求覆盖 Unicode East Asian Width 规范的每一个区间。
bool is_wide_codepoint(char32_t cp) {
  return (cp >= 0x1100 && cp <= 0x115F) ||    // Hangul Jamo
         (cp >= 0x2E80 && cp <= 0xA4CF) ||    // CJK 部首/符号/假名/统一表意文字等
         (cp >= 0xAC00 && cp <= 0xD7A3) ||    // Hangul 音节
         (cp >= 0xF900 && cp <= 0xFAFF) ||    // CJK 兼容表意文字
         (cp >= 0xFF00 && cp <= 0xFF60) ||    // 全角字符
         (cp >= 0xFFE0 && cp <= 0xFFE6) ||
         (cp >= 0x1F300 && cp <= 0x1FAFF) ||  // F-35：emoji(符号与象形、补充符号等),终端按宽字符渲染
         (cp >= 0x20000 && cp <= 0x3FFFD);    // CJK 扩展区(增补平面)
}

// 解码 s[pos] 起的一个 UTF-8 字符,返回(码点, 字节数)。非法/截断的字节序
// 列当成 1 字节宽字符处理,不崩溃、不越界。
std::pair<char32_t, int> decode_utf8_at(const std::string& s, std::size_t pos) {
  unsigned char c0 = static_cast<unsigned char>(s[pos]);
  if (c0 < 0x80) return {c0, 1};
  int len;
  char32_t cp;
  if ((c0 & 0xE0) == 0xC0) {
    len = 2;
    cp = c0 & 0x1F;
  } else if ((c0 & 0xF0) == 0xE0) {
    len = 3;
    cp = c0 & 0x0F;
  } else if ((c0 & 0xF8) == 0xF0) {
    len = 4;
    cp = c0 & 0x07;
  } else {
    return {c0, 1};
  }
  if (pos + static_cast<std::size_t>(len) > s.size()) return {c0, 1};
  for (int i = 1; i < len; ++i) {
    unsigned char c = static_cast<unsigned char>(s[pos + static_cast<std::size_t>(i)]);
    if ((c & 0xC0) != 0x80) return {c0, 1};
    cp = (cp << 6) | (c & 0x3F);
  }
  return {cp, len};
}

// 给定一个 UTF-8 码点的起始字节,返回后面还需要几个续字节才能凑成一个完
// 整码点(0-3)。非法起始字节按 0 处理,当成单字节字符,不阻塞输入。
int utf8_continuation_bytes(unsigned char lead) {
  if (lead < 0x80) return 0;
  if ((lead & 0xE0) == 0xC0) return 1;
  if ((lead & 0xF0) == 0xE0) return 2;
  if ((lead & 0xF8) == 0xF0) return 3;
  return 0;
}

// 按终端实际显示宽度截断到 max_width 列以内——中文等宽字符占 2 列,不是简
// 单按字节数近似(6.4.2/6.4.3 早期版本的简化,extended 6.4.3 的 banner 文
// 案变长后暴露出这个近似会把不该截断的文字截断掉,这次改成按码点正确计
// 算)。放不下最后一个字符时就地停止,不做"半个宽字符"截断,也不强行拼
// "..."(这里是终端 UI 元素,不是给人读的完整句子,截断了就是没画完,不需
// 要额外标记)。
std::string truncate_text(const std::string& s, std::size_t max_width) {
  std::string out;
  std::size_t display_w = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto [cp, len] = decode_utf8_at(s, pos);
    std::size_t w = is_wide_codepoint(cp) ? 2 : 1;
    if (display_w + w > max_width) break;
    out += s.substr(pos, static_cast<std::size_t>(len));
    display_w += w;
    pos += static_cast<std::size_t>(len);
  }
  return out;
}

// 按显示宽度硬换行(宽字符占 2 列)，超出 max_width 就另起一行接着放,不
// 是截断丢字——AI 点评、debug 日志这些内容不能说没就没。不做单词边界折
// 行,中英文混排场景下没有统一的分词规则,硬换行简单且够用,真出现体验
// 问题再改。max_width 为 0 时退化成把整个字符串当一行,不做特殊处理
// (调用方目前都保证传正数,不为这个防御性分支单独写测试)。
std::vector<std::string> wrap_text(const std::string& s, std::size_t max_width) {
  std::vector<std::string> lines;
  if (max_width == 0) {
    lines.push_back(s);
    return lines;
  }
  std::string current;
  std::size_t current_w = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto [cp, len] = decode_utf8_at(s, pos);
    std::size_t w = is_wide_codepoint(cp) ? 2 : 1;
    if (current_w + w > max_width && !current.empty()) {
      lines.push_back(current);
      current.clear();
      current_w = 0;
    }
    current += s.substr(pos, static_cast<std::size_t>(len));
    current_w += w;
    pos += static_cast<std::size_t>(len);
  }
  if (!current.empty() || lines.empty()) lines.push_back(current);
  return lines;
}

std::size_t display_width(const std::string& s) {
  std::size_t w = 0;
  std::size_t pos = 0;
  while (pos < s.size()) {
    auto [cp, len] = decode_utf8_at(s, pos);
    w += is_wide_codepoint(cp) ? 2 : 1;
    pos += static_cast<std::size_t>(len);
  }
  return w;
}

// 截断/补空格到固定的显示宽度——按边框内容区写字的地方都要用这个,而不是
// 裸写字符串,否则这一帧内容比上一帧短的时候,会在文字和右边框之间留下
// 上一帧的残留字符。
std::string pad_to(const std::string& s, std::size_t width) {
  std::string t = truncate_text(s, width);
  std::size_t w = display_width(t);
  if (w < width) t += std::string(width - w, ' ');
  return t;
}

std::string format_size(std::int64_t bytes) {
  double v = static_cast<double>(bytes);
  const char* units[] = {"B", "KB", "MB", "GB"};
  int i = 0;
  while (v >= 1024.0 && i < 3) {
    v /= 1024.0;
    ++i;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f%s", v, units[i]);
  return buf;
}

// increment 6.6:导出路径展开 `~`/`~/...`。标准子命令的路径参数从 argv 来,
// shell 早就在我们看到之前展开过 `~`(除非用户自己加引号);但 `g e` 走的
// 是 read_text_line 直接读键盘字节,完全绕过 shell,不会有人替我们展开——
// 真机验证发现不展开的话 `~` 会被字面创建成一个真的叫 `~` 的目录,"导出成
// 功"但成功到了一个用户没想到的地方。两个入口(cmd_export 和
// handle_g_export_flow)统一走这个函数,行为保持一致。只处理 `~` 和
// `~/...` 这两种形式,不处理 `~user`——M0 单用户场景不需要。
std::string expand_home_path(const std::string& path) {
  if (path != "~" && path.rfind("~/", 0) != 0) return path;
  const char* home = std::getenv("HOME");
  if (!home || home[0] == '\0') return path;  // 环境异常,原样返回,不猜测
  return path == "~" ? std::string(home) : std::string(home) + path.substr(1);
}

// 跟同文件的 wrap_text 不同——那个按字符硬换行,这里的换行只能发生在 token
// 之间的空格上,不能把一个 token(标签)从中间切断。宽度用 display_width 算,
// CJK 标签名占两列。单个 token 本身就比 max_width 还宽这种极端情况,让它单独
// 占一行、可能超出边框——比强行切断标签名更不容易让人误解内容,这次不特殊处理。
std::vector<std::string> wrap_tokens(const std::vector<std::string>& tokens, std::size_t max_width) {
  std::vector<std::string> lines;
  std::string current;
  std::size_t current_w = 0;
  for (const auto& tok : tokens) {
    std::size_t tok_w = display_width(tok);
    if (!current.empty() && current_w + 1 + tok_w > max_width) {
      lines.push_back(current);
      current.clear();
      current_w = 0;
    }
    if (!current.empty()) {
      current += " ";
      ++current_w;
    }
    current += tok;
    current_w += tok_w;
  }
  if (!current.empty() || lines.empty()) lines.push_back(current);
  return lines;
}

// `/` 开头的输入解析成命令名(不含前导 `/`) + 剩余参数——第一个空白就是命令名
// 和参数的分界。命令名和参数之间允许多个空格。
std::pair<std::string, std::string> split_console_command(const std::string& input) {
  std::string body = input.substr(1);  // 去掉前导 '/'
  auto space = body.find(' ');
  if (space == std::string::npos) return {body, ""};
  std::string rest = body.substr(space + 1);
  std::size_t start = rest.find_first_not_of(' ');
  return {body.substr(0, space), start == std::string::npos ? "" : rest.substr(start)};
}

// 从字符串最前面取一个"范围 token":普通情况下按第一个空白切;`#"..."` 这种带
// 引号的标签名整体当一个 token,引号内的空格不算分界——输入语法照抄 tag_token
// (browse.cpp 顶部,信息栏展示标签用的那个)的输出语法,用户不需要为"怎么打带
// 空格的标签名"另外学一套写法。返回值保留开头的 `#` 和引号,解引号交给
// resolve_console_scope(browse.cpp)。没有找到闭合引号时(用户漏打了后一个引号)
// 放弃引号语义,退化成普通按空格切,交给 resolve_console_scope 报"范围写法不对",
// 不是这个函数的职责。
std::pair<std::string, std::string> take_scope_token(const std::string& s) {
  std::size_t start = s.find_first_not_of(' ');
  if (start == std::string::npos) return {"", ""};
  if (s.compare(start, 2, "#\"") == 0) {
    std::size_t close = s.find('"', start + 2);
    if (close != std::string::npos) {
      std::string token = s.substr(start, close - start + 1);
      std::size_t rest_start = s.find_first_not_of(' ', close + 1);
      return {token, rest_start == std::string::npos ? "" : s.substr(rest_start)};
    }
  }
  std::size_t space = s.find(' ', start);
  if (space == std::string::npos) return {s.substr(start), ""};
  std::size_t rest_start = s.find_first_not_of(' ', space);
  return {s.substr(start, space - start), rest_start == std::string::npos ? "" : s.substr(rest_start)};
}

}  // namespace pzt::cli::text
