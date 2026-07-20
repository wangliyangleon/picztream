#!/usr/bin/env bash
# 发布一个新版本,把散落的步骤收成一条命令:
#   1. bump 顶层 CMakeLists 的 project(VERSION) 与 agent/pyproject.toml 的 version
#   2. git-cliff 重新生成 CHANGELOG.md
#   3. commit + 打 tag vX.Y.Z + 推 main 与 tag 到 GitHub
#   4. 拉 GitHub 生成的 source tarball,算 sha256
#   5. 回填 pzt.rb 与 pzt-agent.rb 两个 formula 的 url 与 sha256(同一 tarball),再 commit
#   6. 打印同步到 tap 仓(wangliyangleon/homebrew-pzt)的步骤
#
# 版本号从 Phase B 起用 CalVer 日期形态 YYYY.M.D(无前导零),例: scripts/release.sh 2026.7.20
# 前提: 在 main 分支、工作树干净、git-cliff 已装(brew install git-cliff)。
set -euo pipefail

VERSION="${1:-}"
if [[ -z "$VERSION" ]]; then
  echo "用法: scripts/release.sh <version>   例如 scripts/release.sh 2026.7.20" >&2
  exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "版本号要形如 X.Y.Z(不带 v 前缀,CalVer 如 2026.7.20): $VERSION" >&2
  exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

TAG="v$VERSION"
REPO_SLUG="wangliyangleon/picztream"
TARBALL_URL="https://github.com/${REPO_SLUG}/archive/refs/tags/${TAG}.tar.gz"
CLI_FORMULA="packaging/homebrew/pzt.rb"
AGENT_FORMULA="packaging/homebrew/pzt-agent.rb"

# 只回填 formula 里第一处 url/sha256(主包块,位于 resource 段之前),不碰 resource 的 url/sha256。
backfill_formula() {
  local file="$1" url="$2" sha="$3"
  awk -v url="$url" -v sha="$sha" '
    !u && /^[[:space:]]*url "/    { sub(/url "[^"]*"/, "url \"" url "\"");       u=1 }
    !s && /^[[:space:]]*sha256 "/ { sub(/sha256 "[^"]*"/, "sha256 \"" sha "\""); s=1 }
    { print }
  ' "$file" > "$file.tmp" && mv "$file.tmp" "$file"
}

# --- 前置检查 ---
[[ "$(git rev-parse --abbrev-ref HEAD)" == "main" ]] || { echo "必须在 main 分支" >&2; exit 1; }
[[ -z "$(git status --porcelain)" ]] || { echo "工作树不干净,先提交/清理" >&2; exit 1; }
command -v git-cliff >/dev/null || { echo "缺 git-cliff: brew install git-cliff" >&2; exit 1; }
git rev-parse "$TAG" >/dev/null 2>&1 && { echo "tag $TAG 已存在" >&2; exit 1; }

echo "==> bump 版本 -> $VERSION (CMakeLists + agent/pyproject.toml)"
sed -i '' -E "s/(project\(picztream VERSION )[0-9]+\.[0-9]+\.[0-9]+/\1${VERSION}/" CMakeLists.txt
grep -q "project(picztream VERSION ${VERSION}" CMakeLists.txt || { echo "CMakeLists 版本替换失败" >&2; exit 1; }
sed -i '' -E "s/^version = \"[^\"]*\"/version = \"${VERSION}\"/" agent/pyproject.toml
grep -q "^version = \"${VERSION}\"" agent/pyproject.toml || { echo "agent/pyproject 版本替换失败" >&2; exit 1; }

echo "==> 生成 CHANGELOG.md(git-cliff, 把未发布提交归入 $TAG)"
git-cliff --tag "$TAG" -o CHANGELOG.md

echo "==> commit + tag $TAG"
git add CMakeLists.txt agent/pyproject.toml CHANGELOG.md
git commit -m "Release $TAG"
git tag "$TAG"

echo "==> 推 main 与 tag"
git push origin main
git push origin "$TAG"

echo "==> 拉 tarball 算 sha256: $TARBALL_URL"
# GitHub 刚打完 tag 后 archive 可能有几秒延迟(CI 里尤其),带重试。
EMPTY_SHA="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
SHA=""
for attempt in $(seq 1 6); do
  if got="$(curl -fsSL "$TARBALL_URL" 2>/dev/null | shasum -a 256 | awk '{print $1}')"; then
    if [[ -n "$got" && "$got" != "$EMPTY_SHA" ]]; then SHA="$got"; break; fi
  fi
  echo "    tarball 未就绪,重试 $attempt/6…" >&2; sleep 3
done
[[ -n "$SHA" ]] || { echo "算 sha256 失败" >&2; exit 1; }
echo "    sha256=$SHA"

echo "==> 回填两个 formula 的 url/sha256(同一 tarball)"
backfill_formula "$CLI_FORMULA" "$TARBALL_URL" "$SHA"
backfill_formula "$AGENT_FORMULA" "$TARBALL_URL" "$SHA"
git add "$CLI_FORMULA" "$AGENT_FORMULA"
git commit -m "Deploy: 回填 pzt/pzt-agent formula 到 $TAG (sha256)"
git push origin main

cat <<EOF

发布本地部分完成:$TAG 已推,两个 formula 已回填。
同步到 tap 仓让 brew install 生效:
  git clone git@github.com:wangliyangleon/homebrew-pzt.git /tmp/homebrew-pzt   # 首次
  cp ${CLI_FORMULA} ${AGENT_FORMULA} /tmp/homebrew-pzt/Formula/
  cd /tmp/homebrew-pzt && git add Formula/ && git commit -m "pzt $TAG" && git push
之后用户: brew update && brew upgrade pzt pzt-agent
EOF
