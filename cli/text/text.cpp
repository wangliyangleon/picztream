#include "cli/text/text.h"

#include <cstdio>
#include <cstdlib>

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

}  // namespace pzt::cli::text
