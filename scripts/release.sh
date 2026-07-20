#!/usr/bin/env bash
# 发布一个新版本,把散落的步骤收成一条命令:
#   1. bump 顶层 CMakeLists 的 project(VERSION)
#   2. git-cliff 重新生成 CHANGELOG.md
#   3. commit + 打 tag vX.Y.Z + 推 main 与 tag 到 GitHub
#   4. 拉 GitHub 生成的 source tarball,算 sha256
#   5. 回填 packaging/homebrew/pzt.rb 的 url 与 sha256,再 commit
#   6. 打印同步到 tap 仓(wangliyangleon/homebrew-pzt)的步骤
#
# 用法: scripts/release.sh 0.2.0
# 前提: 在 main 分支、工作树干净、git-cliff 已装(brew install git-cliff)。
set -euo pipefail

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
  echo "用法: scripts/release.sh <version>   例如 scripts/release.sh 0.2.0" >&2
  exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "版本号要形如 X.Y.Z(不带 v 前缀): $VERSION" >&2
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

TAG="v$VERSION"
REPO_SLUG="wangliyangleon/picztream"
TARBALL_URL="https://github.com/${REPO_SLUG}/archive/refs/tags/${TAG}.tar.gz"
FORMULA="packaging/homebrew/pzt.rb"

# --- 前置检查 ---
[[ "$(git rev-parse --abbrev-ref HEAD)" == "main" ]] || { echo "必须在 main 分支" >&2; exit 1; }
[[ -z "$(git status --porcelain)" ]] || { echo "工作树不干净,先提交/清理" >&2; exit 1; }
command -v git-cliff >/dev/null || { echo "缺 git-cliff: brew install git-cliff" >&2; exit 1; }
git rev-parse "$TAG" >/dev/null 2>&1 && { echo "tag $TAG 已存在" >&2; exit 1; }

echo "==> bump project(VERSION) -> $VERSION"
sed -i '' -E "s/(project\(picztream VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1${VERSION}/" CMakeLists.txt
grep -q "project(picztream VERSION ${VERSION}" CMakeLists.txt || { echo "CMakeLists 版本替换失败" >&2; exit 1; }

echo "==> 生成 CHANGELOG.md(git-cliff, 把未发布提交归入 $TAG)"
git-cliff --tag "$TAG" -o CHANGELOG.md

echo "==> commit + tag $TAG"
git add CMakeLists.txt CHANGELOG.md
git commit -m "Release $TAG"
git tag "$TAG"

echo "==> 推 main 与 tag"
git push origin main
git push origin "$TAG"

echo "==> 拉 tarball 算 sha256: $TARBALL_URL"
SHA="$(curl -fsSL "$TARBALL_URL" | shasum -a 256 | awk '{print $1}')"
[[ -n "$SHA" ]] || { echo "算 sha256 失败" >&2; exit 1; }
echo "    sha256=$SHA"

echo "==> 回填 formula url/sha256"
sed -i '' -E "s#(url \").*(\")#\1${TARBALL_URL}\2#" "$FORMULA"
sed -i '' -E "s#(sha256 \").*(\")#\1${SHA}\2#" "$FORMULA"
git add "$FORMULA"
git commit -m "Deploy: 回填 pzt formula 到 $TAG (sha256)"
git push origin main

cat <<EOF

发布本地部分完成:$TAG 已推,formula 已回填。
同步到 tap 仓让 brew install 生效:
  git clone git@github.com:wangliyangleon/homebrew-pzt.git /tmp/homebrew-pzt   # 首次
  cp ${FORMULA} /tmp/homebrew-pzt/Formula/pzt.rb
  cd /tmp/homebrew-pzt && git add Formula/pzt.rb && git commit -m "pzt $TAG" && git push
之后用户: brew update && brew upgrade pzt
EOF
