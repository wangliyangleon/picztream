#!/bin/bash
# M4 增量一子增量 A：headless 命令面的黑盒 smoke 测试。这些命令是给
# agent/ 子进程调用用的，不像 pzt open 那样有 cbreak 交互循环挡在自动
# 化测试路上——JSON 进出，纯文本 I/O，天然可以用普通 shell 脚本驱动，
# 不需要 pty/expect。见 docs/M4_Eng_Design.md"测试策略落地"一节。
#
# 隔离 XDG_CONFIG_HOME 到一个临时目录，不碰真实 ~/.config/pzt——跟项目
# 里所有 pty 真机验证脚本同一个约定。PZT 环境变量指定要测的二进制，默
# 认指向 build_release（跟 agent/ 未来实际调用的路径一致）。
#
# 用法：bash cli/tests/headless_smoke.sh [path/to/pzt]

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PZT="${1:-$REPO_ROOT/build_release/cli/pzt}"

if [ ! -x "$PZT" ]; then
  echo "FAIL: pzt binary not found or not executable at $PZT" >&2
  exit 1
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

export XDG_CONFIG_HOME="$WORKDIR/config"
PHOTOS="$WORKDIR/photos"
mkdir -p "$PHOTOS"

pass_count=0
fail_count=0

# assert_json_has <json_string> <python_expr_on_j> <description>
# python_expr_on_j 是一段以 j(已解析的 JSON 对象) 为输入、返回 truthy
# 才算通过的表达式，用 python3 现成的 json 模块判断，不在 shell 里手
# 写脆弱的字符串匹配。expr 经环境变量传给 python(不是拼进脚本文本)，
# 避免 expr 本身含单引号(比如 j['project'])时和外层拼接冲突。
assert_json_has() {
  local json="$1" expr="$2" desc="$3"
  if echo "$json" | PZT_SMOKE_EXPR="$expr" python3 -c "
import json, os, sys
j = json.load(sys.stdin)
expr = os.environ['PZT_SMOKE_EXPR']
assert eval(expr), 'assertion failed: ' + expr
" 2>/tmp/headless_smoke_err; then
    echo "PASS: $desc"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: $desc"
    cat /tmp/headless_smoke_err >&2
    echo "  json was: $json" >&2
    fail_count=$((fail_count + 1))
  fi
}

assert_nonzero_exit_with_error() {
  local desc="$1"
  shift
  if "$@" >/tmp/headless_smoke_out 2>/tmp/headless_smoke_stderr; then
    echo "FAIL: $desc (expected nonzero exit, got 0)"
    fail_count=$((fail_count + 1))
    return
  fi
  if python3 -c "
import json
with open('/tmp/headless_smoke_stderr') as f:
    j = json.load(f)
assert 'error' in j
" 2>/tmp/headless_smoke_err; then
    echo "PASS: $desc"
    pass_count=$((pass_count + 1))
  else
    echo "FAIL: $desc (stderr not a JSON error object)"
    cat /tmp/headless_smoke_stderr >&2
    fail_count=$((fail_count + 1))
  fi
}

# --- fixtures ---
printf 'x%.0s' {1..30} > "$PHOTOS/a.jpg"
printf 'x%.0s' {1..30} > "$PHOTOS/b.jpg"
printf 'x%.0s' {1..30} > "$PHOTOS/c.jpg"

# --- pzt new (Task A8 会给它加 --json；A2 只用它建 fixture，不断言输出) ---
"$PZT" new smoke "$PHOTOS" >/dev/null

# --- pzt images ---
out="$("$PZT" images smoke --json)"
assert_json_has "$out" "j['project'] == 'smoke'" "images: project name echoed back"
assert_json_has "$out" "len(j['images']) == 3" "images: lists all 3 fixture photos"
assert_json_has "$out" "all('path' in i and 'evaluated' in i and 'tags' in i for i in j['images'])" \
  "images: each entry has path/evaluated/tags"
assert_json_has "$out" "all(i['evaluated'] == False for i in j['images'])" \
  "images: freshly-imported photos are unevaluated"

assert_nonzero_exit_with_error "images: unknown project fails with JSON error" \
  "$PZT" images does-not-exist --json

# --- pzt tag apply ---
out="$("$PZT" tag apply smoke a.jpg 精选 --json)"
assert_json_has "$out" "j['applied'] == True" "tag apply: lazily creates tag and applies it"

out="$("$PZT" images smoke --json)"
assert_json_has "$out" \
  "any(i['path'] == 'a.jpg' and '精选' in i['tags'] for i in j['images'])" \
  "tag apply: tag shows up on the image via pzt images"

# 幂等：同一张图重复打同一个标签，不报错、仍然 applied:true。
out="$("$PZT" tag apply smoke a.jpg 精选 --json)"
assert_json_has "$out" "j['applied'] == True" "tag apply: idempotent on re-apply"

assert_nonzero_exit_with_error "tag apply: unknown image path fails with JSON error" \
  "$PZT" tag apply smoke nope.jpg 精选 --json

echo ""
echo "== headless smoke: $pass_count passed, $fail_count failed =="
if [ "$fail_count" -ne 0 ]; then
  exit 1
fi
