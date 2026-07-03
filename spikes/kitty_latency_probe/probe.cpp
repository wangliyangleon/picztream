// Throwaway spike: measures the latency of decoding a JPEG and pushing it to
// the terminal via the Kitty graphics protocol (through a Tmux passthrough
// wrapper), and whether raw JPEG bytes can be sent to the protocol directly
// without a decode step. See ../README.md and results.md for findings.
//
// Build: clang++ -std=c++20 -O2 -o probe probe.cpp \
//          -framework CoreGraphics -framework ImageIO -framework CoreFoundation
// Run:   ./probe <jpeg1> [jpeg2 ...]   (run interactively in the target
//                                       Ghostty+Tmux pane for real numbers)

#include <ImageIO/ImageIO.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

using clk = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

namespace {

std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("cannot open " + path);
    auto size = f.tellg();
    std::vector<unsigned char> buf(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

std::string base64_encode(const unsigned char* data, size_t len) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
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

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgba;  // 4 bytes/pixel
};

// Decode via macOS ImageIO/CoreGraphics - zero extra dependency on Apple
// Silicon, hardware-accelerated JPEG path.
DecodedImage decode_jpeg(const std::vector<unsigned char>& bytes) {
    CFDataRef data = CFDataCreate(nullptr, bytes.data(),
                                   static_cast<CFIndex>(bytes.size()));
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    if (!src) throw std::runtime_error("CGImageSourceCreateWithData failed");
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    if (!img) throw std::runtime_error("CGImageSourceCreateImageAtIndex failed");

    DecodedImage out;
    out.width = static_cast<int>(CGImageGetWidth(img));
    out.height = static_cast<int>(CGImageGetHeight(img));
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(
        out.rgba.data(), out.width, out.height, 8, out.width * 4, cs,
        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
    CGContextDrawImage(ctx, CGRectMake(0, 0, out.width, out.height), img);

    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    CGImageRelease(img);
    CFRelease(src);
    CFRelease(data);
    return out;
}

bool is_inside_tmux() { return std::getenv("TMUX") != nullptr; }

// Wrap a raw escape-sequence blob for tmux passthrough (DCS tmux; ... ST),
// doubling every ESC inside per tmux's passthrough escaping rule. Requires
// `set -g allow-passthrough on` in tmux config.
std::string tmux_wrap(const std::string& raw) {
    std::string escaped;
    escaped.reserve(raw.size() + 16);
    for (char c : raw) {
        escaped += c;
        if (c == '\x1b') escaped += '\x1b';
    }
    return "\x1bPtmux;" + escaped + "\x1b\\";
}

// t=t (temporary file) transmission medium: write raw pixels to a local
// file and hand the terminal only the (base64-encoded) *path* - the
// terminal reads the pixel data itself via direct file I/O, skipping the
// base64-over-pty-escape-sequence channel entirely. Terminal deletes the
// file after reading. Returns bytes written to fd (the tiny control seq).
size_t send_kitty_rgba_via_tmpfile(int fd, const DecodedImage& img,
                                    int image_id, const std::string& tmp_path,
                                    double* file_write_ms) {
    auto t0 = clk::now();
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(img.rgba.data()),
                static_cast<std::streamsize>(img.rgba.size()));
    }
    *file_write_ms = ms(clk::now() - t0).count();

    std::string path_b64 = base64_encode(
        reinterpret_cast<const unsigned char*>(tmp_path.data()),
        tmp_path.size());
    std::ostringstream ctrl;
    ctrl << "a=T,f=32,t=t,s=" << img.width << ",v=" << img.height
         << ",i=" << image_id;
    std::string seq = "\x1b_G" + ctrl.str() + ";" + path_b64 + "\x1b\\";
    std::string out = is_inside_tmux() ? tmux_wrap(seq) : seq;
    ssize_t w = write(fd, out.data(), out.size());
    return w > 0 ? static_cast<size_t>(w) : 0;
}

// Emits one Kitty graphics protocol transmit+display command for an RGBA
// buffer, chunked into <=4096-byte base64 payloads per the protocol spec.
// Returns the number of bytes written() to fd, and fills chunk_count.
size_t send_kitty_rgba(int fd, const DecodedImage& img, int image_id,
                        int* chunk_count) {
    std::string b64 = base64_encode(img.rgba.data(), img.rgba.size());
    const size_t CHUNK = 4096;
    size_t written = 0;
    int chunks = 0;
    for (size_t off = 0; off < b64.size(); off += CHUNK) {
        bool first = (off == 0);
        size_t len = std::min(CHUNK, b64.size() - off);
        bool more = (off + len) < b64.size();
        std::ostringstream ctrl;
        if (first) {
            ctrl << "a=T,f=32,s=" << img.width << ",v=" << img.height
                 << ",i=" << image_id << ",m=" << (more ? 1 : 0);
        } else {
            ctrl << "i=" << image_id << ",m=" << (more ? 1 : 0);
        }
        std::string seq =
            "\x1b_G" + ctrl.str() + ";" + b64.substr(off, len) + "\x1b\\";
        std::string out = is_inside_tmux() ? tmux_wrap(seq) : seq;
        ssize_t w = write(fd, out.data(), out.size());
        if (w > 0) written += static_cast<size_t>(w);
        chunks++;
    }
    *chunk_count = chunks;
    return written;
}

// Best-effort: try to read a Kitty protocol ACK/response terminated by ST
// (ESC \). Only meaningful if stdin is a real interactive tty (not the case
// when this probe is launched non-interactively). Returns elapsed ms, or -1
// if no tty / timed out.
double try_read_ack(int timeout_ms) {
    if (!isatty(STDIN_FILENO)) return -1.0;

    termios orig{};
    tcgetattr(STDIN_FILENO, &orig);
    termios raw = orig;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    auto t0 = clk::now();
    std::string buf;
    bool done = false;
    while (!done) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           clk::now() - t0)
                           .count();
        int remaining = timeout_ms - static_cast<int>(elapsed);
        if (remaining <= 0) break;
        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int r = poll(&pfd, 1, remaining);
        if (r <= 0) break;
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) break;
        buf += c;
        if (buf.size() >= 2 && buf[buf.size() - 2] == '\x1b' &&
            buf[buf.size() - 1] == '\\') {
            done = true;
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    if (!done) return -1.0;
    return std::chrono::duration<double, std::milli>(clk::now() - t0).count();
}

struct Sample {
    double read_ms, decode_ms, encode_ms, write_ms, ack_ms, total_ms;
};

double pctl(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(p * (v.size() - 1));
    return v[idx];
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <jpeg> [jpeg...]\n";
        return 1;
    }
    const int ITER = 8;
    bool tty_stdout = isatty(STDOUT_FILENO);
    bool tty_stdin = isatty(STDIN_FILENO);
    std::cerr << "[info] stdout is " << (tty_stdout ? "" : "NOT ")
              << "a tty; stdin is " << (tty_stdin ? "" : "NOT ")
              << "a tty; inside tmux: " << (is_inside_tmux() ? "yes" : "no")
              << "\n";

    for (int argi = 1; argi < argc; ++argi) {
        std::string path = argv[argi];
        std::vector<Sample> samples;
        int image_id = 1000 + argi;

        for (int it = 0; it < ITER; ++it) {
            auto t0 = clk::now();
            auto bytes = read_file(path);
            auto t1 = clk::now();
            DecodedImage img = decode_jpeg(bytes);
            auto t2 = clk::now();
            std::string b64 = base64_encode(img.rgba.data(), img.rgba.size());
            auto t3 = clk::now();
            int chunks = 0;
            size_t wbytes = send_kitty_rgba(STDOUT_FILENO, img, image_id, &chunks);
            auto t4 = clk::now();
            double ack = try_read_ack(1500);
            auto t5 = clk::now();

            Sample s;
            s.read_ms = ms(t1 - t0).count();
            s.decode_ms = ms(t2 - t1).count();
            s.encode_ms = ms(t3 - t2).count();
            s.write_ms = ms(t4 - t3).count();
            s.ack_ms = ack;
            s.total_ms = ms((ack >= 0 ? t5 : t4) - t0).count();
            samples.push_back(s);

            std::cerr << "[" << path << " #" << it << "] " << img.width << "x"
                      << img.height << " raw=" << (img.rgba.size() / 1e6)
                      << "MB b64=" << (b64.size() / 1e6)
                      << "MB chunks=" << chunks << " wbytes=" << wbytes
                      << " read=" << s.read_ms << "ms decode=" << s.decode_ms
                      << "ms encode=" << s.encode_ms
                      << "ms write=" << s.write_ms << "ms ack="
                      << (ack >= 0 ? std::to_string(ack) + "ms" : "n/a")
                      << " total=" << s.total_ms << "ms\n";
        }

        std::vector<double> totals, decodes, encodes, writes;
        for (auto& s : samples) {
            totals.push_back(s.total_ms);
            decodes.push_back(s.decode_ms);
            encodes.push_back(s.encode_ms);
            writes.push_back(s.write_ms);
        }
        // NB: stdout is reserved for the Kitty protocol image bytes when
        // pointed at a real terminal - all human-readable output goes to
        // stderr so it doesn't corrupt the image stream.
        std::cerr << "=== " << path << " summary (n=" << samples.size()
                  << ") ===\n";
        std::cerr << "decode  p50=" << pctl(decodes, 0.5)
                   << "ms p99=" << pctl(decodes, 0.99) << "ms\n";
        std::cerr << "encode  p50=" << pctl(encodes, 0.5)
                   << "ms p99=" << pctl(encodes, 0.99) << "ms\n";
        std::cerr << "write   p50=" << pctl(writes, 0.5)
                   << "ms p99=" << pctl(writes, 0.99) << "ms\n";
        std::cerr << "total(no-ack) p50=" << pctl(totals, 0.5)
                   << "ms p99=" << pctl(totals, 0.99) << "ms\n";

        // Second pass: t=t (file-based) transmission medium, reusing the
        // already-decoded pixels (this models the prefetch-ahead-of-time
        // design where decode happens off the interactive path, so only
        // the "get bytes to the terminal" cost matters here).
        DecodedImage img = decode_jpeg(read_file(path));
        std::vector<double> file_writes, ctrl_writes, file_totals;
        for (int it = 0; it < ITER; ++it) {
            std::string tmp = "/tmp/pzt_probe_" + std::to_string(image_id) +
                               "_" + std::to_string(it) + ".rgba";
            auto t0 = clk::now();
            double file_write_ms = 0;
            size_t wbytes = send_kitty_rgba_via_tmpfile(
                STDOUT_FILENO, img, image_id + 500 + it, tmp, &file_write_ms);
            auto t1 = clk::now();
            double ack = try_read_ack(1500);
            double total = ms(clk::now() - t0).count();
            file_writes.push_back(file_write_ms);
            ctrl_writes.push_back(ms(t1 - t0).count() - file_write_ms);
            file_totals.push_back(total);
            std::cerr << "[t=t " << path << " #" << it
                       << "] file_write=" << file_write_ms
                       << "ms ctrl_seq_write=" << (ms(t1 - t0).count() - file_write_ms)
                       << "ms ctrl_bytes=" << wbytes << " ack="
                       << (ack >= 0 ? std::to_string(ack) + "ms" : "n/a") << "\n";
            unlink(tmp.c_str());  // terminal deletes on success; clean up if it didn't run
        }
        std::cerr << "=== " << path << " t=t summary (n=" << file_writes.size()
                   << ") ===\n";
        std::cerr << "file_write p50=" << pctl(file_writes, 0.5)
                   << "ms p99=" << pctl(file_writes, 0.99) << "ms\n";
        std::cerr << "ctrl_write p50=" << pctl(ctrl_writes, 0.5)
                   << "ms p99=" << pctl(ctrl_writes, 0.99) << "ms\n";
        std::cerr << "total(no-ack) p50=" << pctl(file_totals, 0.5)
                   << "ms p99=" << pctl(file_totals, 0.99) << "ms\n";
    }
    return 0;
}
