# PicZTream Telegram agent 的 Homebrew formula(Python virtualenv)。
# 与 pzt.rb 一样,真相源在本仓,发布时由 scripts/release.sh 回填 url/sha256、
# 同步到 tap 仓 wangliyangleon/homebrew-pzt。装它会顺带装 pzt(depends_on)。
# 用户侧:brew tap wangliyangleon/pzt && brew trust wangliyangleon/pzt && brew install pzt-agent
#
# resource 段是 python-telegram-bot 20.x + httpx 栈的固定依赖闭包,按字母序。
class PztAgent < Formula
  include Language::Python::Virtualenv

  desc "Telegram half-automated culling/delivery agent for PicZTream"
  homepage "https://github.com/wangliyangleon/picztream"
  url "https://github.com/wangliyangleon/picztream/archive/refs/tags/v2026.7.20.tar.gz"
  sha256 "c83c246b6de9e0b3f57125d19486bb023f586eb46db35d38d1a32909c064085b"
  head "https://github.com/wangliyangleon/picztream.git", branch: "main"

  depends_on "python@3.14"
  depends_on "pzt" # 装 agent 会顺带装 CLI,统一 brew 故事

  resource "anyio" do
    url "https://files.pythonhosted.org/packages/96/f0/5eb65b2bb0d09ac6776f2eb54adee6abe8228ea05b20a5ad0e4945de8aac/anyio-4.12.1.tar.gz"
    sha256 "41cfcc3a4c85d3f05c932da7c26d0201ac36f72abd4435ba90d0464a3ffed703"
  end

  resource "certifi" do
    url "https://files.pythonhosted.org/packages/c9/c7/424b75da314c1045981bd9777432fad05a9e0c69daa4ed7e308bbaffe405/certifi-2026.6.17.tar.gz"
    sha256 "024c88eeec92ca068db80f02b8b07c9cef7b9fe261d1d535abfd5abd6f6af432"
  end

  resource "h11" do
    url "https://files.pythonhosted.org/packages/01/ee/02a2c011bdab74c6fb3c75474d40b3052059d95df7e73351460c8588d963/h11-0.16.0.tar.gz"
    sha256 "4e35b956cf45792e4caa5885e69fba00bdbc6ffafbfa020300e549b208ee5ff1"
  end

  resource "httpcore" do
    url "https://files.pythonhosted.org/packages/06/94/82699a10bca87a5556c9c59b5963f2d039dbd239f25bc2a63907a05a14cb/httpcore-1.0.9.tar.gz"
    sha256 "6e34463af53fd2ab5d807f399a9b45ea31c3dfa2276f15a2c3f00afff6e176e8"
  end

  resource "httpx" do
    url "https://files.pythonhosted.org/packages/bd/26/2dc654950920f499bd062a211071925533f821ccdca04fa0c2fd914d5d06/httpx-0.26.0.tar.gz"
    sha256 "451b55c30d5185ea6b23c2c793abf9bb237d2a7dfb901ced6ff69ad37ec1dfaf"
  end

  resource "idna" do
    url "https://files.pythonhosted.org/packages/cd/63/9496c57188a2ee585e0f1db071d75089a11e98aa86eb99d9d7618fc1edce/idna-3.18.tar.gz"
    sha256 "ffb385a7e039654cef1ab9ef32c6fafe283c0c0467bba1d9029738ce4a14a848"
  end

  # 用 wheel 而非 sdist:python-telegram-bot 20.8 的 setup.py 在新 setuptools/py3.14
  # 下 sdist 构建报 KeyError('__version__');它是纯 python 通用 wheel,Homebrew 原生
  # 支持 py3-none-any.whl resource(见 Homebrew language/python.rb)。其余 8 个 sdist 正常。
  resource "python-telegram-bot" do
    url "https://files.pythonhosted.org/packages/6f/8e/4e4ed06986557fce0c41c3dfc60c5495b1095cf8a552bdc4c56e96aefdac/python_telegram_bot-20.8-py3-none-any.whl"
    sha256 "a98ddf2f237d6584b03a2f8b20553e1b5e02c8d3a1ea8e17fd06cc955af78c14"
  end

  resource "sniffio" do
    url "https://files.pythonhosted.org/packages/a2/87/a6771e1546d97e7e041b6ae58d80074f81b7d5121207425c964ddf5cfdbd/sniffio-1.3.1.tar.gz"
    sha256 "f4324edc670a0f49750a81b895f35c3adb843cca46f0530f79fc1babb23789dc"
  end

  resource "typing-extensions" do
    url "https://files.pythonhosted.org/packages/f6/cc/6253133b5bb138fc3306cebfbda2c520f545d36b5be2c7255cc528bb45d6/typing_extensions-4.16.0.tar.gz"
    sha256 "dc983d19a509c94dba722ee6abd33940f7c05a89e243c47e907eb4db6f1a43e5"
  end

  def install
    # 主包在 tarball 的 agent/ 子目录,不能用 virtualenv_install_with_resources
    # (它装 buildpath 根)。用底层三步:建 venv、装 resources、从子目录装并链接入口。
    venv = virtualenv_create(libexec, "python3.14")
    venv.pip_install resources
    venv.pip_install_and_link buildpath/"agent"
    pkgshare.install "contrib/pzt-agent.env.example"
  end

  def caveats
    <<~EOS
      运行前置(单用户自托管):
        1. Telegram: @BotFather 建 bot 拿 TELEGRAM_BOT_TOKEN, 取自己的 TELEGRAM_CHAT_ID
        2. AI: 本地 Ollama(brew install --cask ollama; ollama pull gemma4:e2b)或配云端 key
        3. 配置样例: #{opt_pkgshare}/pzt-agent.env.example
      启动: 填好环境变量后跑 `pzt-agent`(长驻建议放 tmux)。详见项目 agent/README.md
    EOS
  end

  test do
    assert_match "usage", shell_output("#{bin}/pzt-agent --help")
  end
end
