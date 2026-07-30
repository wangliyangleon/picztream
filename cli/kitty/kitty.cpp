#include "cli/kitty/kitty.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <unistd.h>

namespace pzt::cli::kitty {

bool is_inside_tmux() { return std::getenv("TMUX") != nullptr; }

std::string make_tmp_path(const std::string& tag) {
  const char* tmpdir = std::getenv("TMPDIR");
  std::string dir = (tmpdir && *tmpdir) ? tmpdir : "/tmp";
  if (!dir.empty() && dir.back() == '/') dir.pop_back();
  return dir + "/pzt-tty-graphics-protocol-" + tag + ".rgba";
}

std::string tmux_wrap(const std::string& raw) {
  std::string escaped;
  escaped.reserve(raw.size() + 16);
  for (char c : raw) {
    escaped += c;
    if (c == '\x1b') escaped += '\x1b';
  }
  return "\x1bPtmux;" + escaped + "\x1b\\";
}

std::string base64_encode(const unsigned char* data, std::size_t len) {
  static const char* tbl =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  std::size_t i = 0;
  for (; i + 2 < len; i += 3) {
    unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += tbl[(v >> 18) & 0x3F];
    out += tbl[(v >> 12) & 0x3F];
    out += tbl[(v >> 6) & 0x3F];
    out += tbl[v & 0x3F];
  }
  if (len - i == 1) {
    unsigned v = data[i] << 16;
    out += tbl[(v >> 18) & 0x3F];
    out += tbl[(v >> 12) & 0x3F];
    out += "==";
  } else if (len - i == 2) {
    unsigned v = (data[i] << 16) | (data[i + 1] << 8);
    out += tbl[(v >> 18) & 0x3F];
    out += tbl[(v >> 12) & 0x3F];
    out += tbl[(v >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

bool parse_allow_passthrough(const std::string& trimmed_output) {
  return trimmed_output == "on";
}

namespace {

std::string trim(const std::string& s) {
  std::size_t begin = s.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return "";
  std::size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(begin, end - begin + 1);
}

// 设了但为空一律当成没设:空值不该比未设置更宽松。
bool env_present(const EnvLookupFn& lookup, std::string_view name) {
  auto value = lookup(name);
  return value.has_value() && !value->empty();
}

bool env_equals(const EnvLookupFn& lookup, std::string_view name, std::string_view expected) {
  auto value = lookup(name);
  return value.has_value() && *value == expected;
}

// TERM 按前缀比:带后缀的 terminfo 变体(xterm-ghostty-256color 之类)仍是
// 同一个终端。
bool env_starts_with(const EnvLookupFn& lookup, std::string_view name, std::string_view prefix) {
  auto value = lookup(name);
  return value.has_value() && value->rfind(prefix, 0) == 0;
}

}  // namespace

bool kitty_support_likely(const EnvLookupFn& lookup, bool inside_tmux) {
  // 这两类由终端自己注入进环境,是 tmux 内唯一活得下来的信号,所以不管在不
  // 在 tmux 里都先看它们。
  if (env_present(lookup, "KITTY_WINDOW_ID")) return true;         // kitty 本尊
  if (env_present(lookup, "GHOSTTY_RESOURCES_DIR")) return true;   // Ghostty shell integration
  if (env_present(lookup, "GHOSTTY_BIN_DIR")) return true;

  // tmux 内 TERM/TERM_PROGRAM 的值只可能来自 tmux 自己(实测 pane 里分别是
  // screen-256color 和 tmux,Ghostty 的身份被整个擦掉),看它们只会稳定误
  // 判,直接跳过。代价是:从 Ghostty 起的 server 后来换终端 attach 时,
  // GHOSTTY_* 是 stale 的会假阳性 - 但假阳性最坏只是退回"不提示"的现状,
  // 而看 TERM 会在正常使用下稳定假阴性,两者不对称(见 PRD 风险 1、2)。
  if (inside_tmux) return false;

  if (env_equals(lookup, "TERM_PROGRAM", "ghostty")) return true;
  if (env_starts_with(lookup, "TERM", "xterm-ghostty")) return true;
  if (env_starts_with(lookup, "TERM", "xterm-kitty")) return true;
  // WezTerm 等"部分支持"的终端刻意不收:本项目从未在它们上面实测过 t=t 与
  // q=2 的行为,收进来等于承诺一个没验证的环境,比漏判更糟。漏判的代价只是
  // 一行可以关掉的提示。
  return false;
}

namespace {

// 校验 passthrough、按需包 tmux DCS、写到 fd,三个函数共用的收尾步骤。
pzt::core::Result<void, RenderError> send_sequence(int fd, const TerminalMode& mode,
                                                    const std::string& raw_seq) {
  using Result = pzt::core::Result<void, RenderError>;
  if (mode.inside_tmux && !mode.passthrough_ok) {
    return Result::Err(RenderError::PassthroughDisabled);
  }
  std::string out = mode.inside_tmux ? tmux_wrap(raw_seq) : raw_seq;
  ssize_t written = write(fd, out.data(), out.size());
  if (written < 0 || static_cast<std::size_t>(written) != out.size()) {
    return Result::Err(RenderError::WriteFailed);
  }
  return Result::Ok();
}

}  // namespace

TerminalMode detect_terminal_mode() {
  TerminalMode mode;
  mode.inside_tmux = is_inside_tmux();
  mode.kitty_support_likely = kitty_support_likely(
      [](std::string_view name) -> std::optional<std::string> {
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) return std::nullopt;
        return std::string(value);
      },
      mode.inside_tmux);
  if (!mode.inside_tmux) return mode;

  std::string output;
  FILE* pipe = popen("tmux show-options -gqv allow-passthrough 2>/dev/null", "r");
  if (pipe != nullptr) {
    std::array<char, 64> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
      output += buf.data();
    }
    pclose(pipe);
  }
  mode.passthrough_ok = parse_allow_passthrough(trim(output));
  return mode;
}

pzt::core::Result<void, RenderError> render_rgba_via_tmpfile(
    int fd, const TerminalMode& mode, const pzt::core::decode::DecodedImage& img, int image_id,
    const std::string& tmp_path, int target_cols, int target_rows) {
  // passthrough 检查放前面,避免在明知发不出去的情况下还去写临时文件。
  if (mode.inside_tmux && !mode.passthrough_ok) {
    return pzt::core::Result<void, RenderError>::Err(RenderError::PassthroughDisabled);
  }

  {
    std::ofstream f(tmp_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.rgba.data()),
            static_cast<std::streamsize>(img.rgba.size()));
  }

  std::string path_b64 = base64_encode(reinterpret_cast<const unsigned char*>(tmp_path.data()),
                                        tmp_path.size());
  std::ostringstream ctrl;
  ctrl << "a=T,f=32,t=t,q=2,s=" << img.width << ",v=" << img.height << ",i=" << image_id;
  if (target_cols > 0 && target_rows > 0) {
    ctrl << ",c=" << target_cols << ",r=" << target_rows;
  }
  std::string seq = "\x1b_G" + ctrl.str() + ";" + path_b64 + "\x1b\\";
  auto sent = send_sequence(fd, mode, seq);
  // 正常路径下终端读完 t=t 临时文件后会自行删除它;但写控制序列失败时
  // (WriteFailed)终端根本没收到序列、不会去读也不会删,临时文件会残留。
  // 这条失败路径显式清理,避免 TMPDIR 里堆积孤儿 .rgba 文件。
  if (!sent.ok()) {
    std::remove(tmp_path.c_str());
  }
  return sent;
}

pzt::core::Result<void, RenderError> clear_placement(int fd, const TerminalMode& mode,
                                                      int image_id) {
  std::ostringstream ctrl;
  ctrl << "a=d,d=i,q=2,i=" << image_id;
  std::string seq = "\x1b_G" + ctrl.str() + "\x1b\\";
  return send_sequence(fd, mode, seq);
}

FitSize fit_within(int image_w, int image_h, int box_w, int box_h) {
  if (image_w <= 0 || image_h <= 0 || box_w <= 0 || box_h <= 0) return FitSize{0, 0};
  double scale = std::min(static_cast<double>(box_w) / image_w,
                           static_cast<double>(box_h) / image_h);
  int w = std::max(1, static_cast<int>(image_w * scale));
  int h = std::max(1, static_cast<int>(image_h * scale));
  return FitSize{w, h};
}

}  // namespace pzt::cli::kitty
