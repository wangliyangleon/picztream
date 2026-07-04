#include "cli/kitty/kitty.h"

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

}  // namespace

TerminalMode detect_terminal_mode() {
  TerminalMode mode;
  mode.inside_tmux = is_inside_tmux();
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
    const std::string& tmp_path) {
  using Result = pzt::core::Result<void, RenderError>;

  if (mode.inside_tmux && !mode.passthrough_ok) {
    return Result::Err(RenderError::PassthroughDisabled);
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
  std::string seq = "\x1b_G" + ctrl.str() + ";" + path_b64 + "\x1b\\";
  std::string out = mode.inside_tmux ? tmux_wrap(seq) : seq;

  ssize_t written = write(fd, out.data(), out.size());
  if (written < 0 || static_cast<std::size_t>(written) != out.size()) {
    return Result::Err(RenderError::WriteFailed);
  }
  return Result::Ok();
}

}  // namespace pzt::cli::kitty
