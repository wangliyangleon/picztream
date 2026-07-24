# PicZTream CLI 的 Homebrew formula(源码构建版)。
# 这份是版本管理里的真相源;发布时由 scripts/release.sh 回填 url/sha256,
# 并同步到独立的 tap 仓库 wangliyangleon/homebrew-pzt 的 Formula/pzt.rb。
# 用户侧:brew tap wangliyangleon/pzt && brew install pzt
#
# 注:仓库暂无 LICENSE 文件,故未声明 license(brew audit 会提示);补 LICENSE 后加上。
class Pzt < Formula
  desc "Terminal keyboard-driven photo culling and local color pipeline"
  homepage "https://github.com/wangliyangleon/picztream"
  url "https://github.com/wangliyangleon/picztream/archive/refs/tags/v2026.7.24.tar.gz"
  sha256 "2df68afb8af711bb0ee4dff955bb54dcd27c80de54fe68990804bd5aa65c1496"
  head "https://github.com/wangliyangleon/picztream.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "ninja" => :build
  depends_on "pkg-config" => :build
  depends_on "libomp"
  depends_on "libraw"
  depends_on :macos
  depends_on "nlohmann-json"
  depends_on "sqlite"

  def install
    # core/CMakeLists.txt 硬编码 /opt/homebrew/opt/libomp,只在 Apple Silicon 成立。
    odie "pzt currently supports Apple Silicon (arm64) only" unless Hardware::CPU.arm?

    # 顶层 CMakeLists 未指定 build type 时默认 Debug(带 ASan),发布必须显式 Release。
    system "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
           "-DCMAKE_BUILD_TYPE=Release", *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "pzt #{version}", shell_output("#{bin}/pzt --version")
  end
end
