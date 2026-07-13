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

# --- pzt dedup ---
# 真实的分组/去重算法已经在 core/tests/dedup_test.cpp 里用可解码的假
# JPEG 详尽覆盖了(候选聚类、汉明距离分组、keep_id 选择等)，这里的
# fixture 是不可解码的假字节，验证的不是算法本身，是命令这一层的接线
# (scope 解析、Settings 参数传递、JSON 形状)——三张新导入的图都没有
# captured_at，dedup 应该把三张全部计入 skipped_no_capture_time，
# groups/tagged 都是 0，这是确定性的、不依赖真实可解码图片内容。
out="$("$PZT" dedup smoke --scope '*' --json)"
assert_json_has "$out" "j['skipped_no_capture_time'] == 3" \
  "dedup: images with no captured_at are all counted as skipped"
assert_json_has "$out" "j['groups'] == 0 and j['tagged'] == 0" \
  "dedup: no groups formed when nothing has a capture time"

assert_nonzero_exit_with_error "dedup: unknown tag scope fails with JSON error" \
  "$PZT" dedup smoke --scope '#不存在的标签' --json

# --- pzt export-images ---
# a.jpg/b.jpg/c.jpg 都是不可解码的假字节，但 kind="jpeg" 且没有 recipe
# 的图片导出走字节级拷贝(core/export/export.cpp 的 write_one_export)，
# 不需要真的能解码——所以这里能测到真实的导出成功路径,不是靠错误路径
# 打幌子。
OUT1="$WORKDIR/out1"
out="$("$PZT" export-images smoke a.jpg b.jpg "$OUT1" --json)"
assert_json_has "$out" "j['exported'] == 2" "export-images: exports the given paths"
assert_json_has "$out" "j['created_dir'] == True" "export-images: reports newly-created output dir"
if [ -f "$OUT1/a.jpg" ] && [ -f "$OUT1/b.jpg" ]; then
  echo "PASS: export-images: files actually written to disk"
  pass_count=$((pass_count + 1))
else
  echo "FAIL: export-images: files actually written to disk"
  fail_count=$((fail_count + 1))
fi

# F-26 默认排除：c.jpg 打上(已存在的系统)废片标签之后，即便显式点名
# 也不会被导出，除非 Settings.export_reject 打开——跟交互侧 cmd_export
# 同一份默认排除规则(见 docs/Fix_It_Night_Review.md F-26)，这里验证
# export-images 这条新命令也遵守它。
"$PZT" tag apply smoke c.jpg 废片 --json >/dev/null
out="$("$PZT" export-images smoke a.jpg c.jpg "$WORKDIR/out2" --json)"
assert_json_has "$out" "j['exported'] == 1" \
  "export-images: reject-tagged image excluded by default (F-26)"

assert_nonzero_exit_with_error "export-images: unknown image path fails with JSON error" \
  "$PZT" export-images smoke nope.jpg "$WORKDIR/out3" --json

# --- pzt eval ---
# 计划原本设想用"已经全部评估过"的 fixture 测 submitted==0，但要造出
# "已评估"状态要么真的发一次网络请求、要么直接戳库，都不是黑盒 smoke
# 测试该干的事。这里换一个同样不碰真网络、但覆盖面更大的路径：a/b/c
# 都是不可解码的假字节，process_request 里 decode_preview_file 这一步
# 就会失败(EvaluationError::ImageUnavailable)，根本走不到真正调用
# evaluation_fn_(真实 AI 请求)那一步——照样测到了完整的提交/轮询/收
# 尾归类逻辑，不是只测"跳过"这条最短路径。
out="$("$PZT" eval smoke --scope '*' --provider gemini --json)"
assert_json_has "$out" "j['submitted'] == 3" "eval: submits all unevaluated images in scope"
assert_json_has "$out" "len(j['evaluated']) == 0" "eval: none succeed (fixtures aren't real jpegs)"
# 三个都在解码这一步失败，理由通常是 image_unavailable；但
# take_last_failure() 只保留"最近一次"失败，三个都在毫秒级内几乎同时
# 完成时，有极小概率某一个的失败原因在被取走前就被下一个覆盖，兜底归
# 类成 unknown(cmd_eval 里有详细说明)——两者都算测试通过，不接受任何
# 图片被漏报或者错误地进了 evaluated。
assert_json_has "$out" \
  "len(j['failed']) == 3 and all(f['error'] in ('image_unavailable', 'unknown') for f in j['failed'])" \
  "eval: undecodable fixtures fail at decode, not at the network call"

assert_nonzero_exit_with_error "eval: unknown provider fails with JSON error" \
  "$PZT" eval smoke --scope '*' --provider bogus --json

assert_nonzero_exit_with_error "eval: unknown tag scope fails with JSON error" \
  "$PZT" eval smoke --scope '#不存在的标签' --provider gemini --json

# --- pzt curate ---
# a/b/c 手动写评估分数(全部通过 gate)和分散的 captured_at(避免互相聚
# 簇)，验证候选过滤 + 每张各自成簇时按分数选 top N + 默认落"精选"标签
# 的接线，不测多样性算法本身的细节(那部分已经在 core/tests/curate_test.cpp
# 用可解码 JPEG 详尽覆盖)。
DBPATH="$XDG_CONFIG_HOME/pzt/pzt.db"
sqlite3 "$DBPATH" "UPDATE images SET captured_at = 1000 WHERE file_path = 'a.jpg';"
sqlite3 "$DBPATH" "UPDATE images SET captured_at = 100000 WHERE file_path = 'b.jpg';"
sqlite3 "$DBPATH" "UPDATE images SET captured_at = 200000 WHERE file_path = 'c.jpg';"
sqlite3 "$DBPATH" "INSERT INTO image_evaluations (image_id, exposure_score, exposure_note,
    composition_score, composition_note, focus_score, focus_note, comment, extra_guidance, provider)
  SELECT id, 9, '', 9, '', 9, '', '', '', 'gemini' FROM images WHERE file_path = 'a.jpg';"
sqlite3 "$DBPATH" "INSERT INTO image_evaluations (image_id, exposure_score, exposure_note,
    composition_score, composition_note, focus_score, focus_note, comment, extra_guidance, provider)
  SELECT id, 8, '', 8, '', 8, '', '', '', 'gemini' FROM images WHERE file_path = 'b.jpg';"
# c.jpg 保持不评估——验证候选过滤排除未评估图

out="$("$PZT" curate smoke --count 2 --json)"
assert_json_has "$out" "j['requested'] == 2 and j['returned'] == 2" \
  "curate: selects up to count from evaluated, passing candidates"
assert_json_has "$out" "sorted(j['selected']) == ['a.jpg', 'b.jpg']" \
  "curate: excludes unevaluated c.jpg, picks a/b by score"

out="$("$PZT" images smoke --json)"
assert_json_has "$out" \
  "all('精选' in i['tags'] for i in j['images'] if i['path'] in ('a.jpg', 'b.jpg'))" \
  "curate: applies the default apply-tag (精选) to selected images"

out="$("$PZT" curate smoke --count 1 --apply-tag ins --json)"
assert_json_has "$out" "j['returned'] == 1 and j['selected'] == ['a.jpg']" \
  "curate: --apply-tag uses a custom tag name, still picks by score"

out="$("$PZT" images smoke --json)"
assert_json_has "$out" "any(i['path'] == 'a.jpg' and 'ins' in i['tags'] for i in j['images'])" \
  "curate: custom apply-tag actually gets applied"

assert_nonzero_exit_with_error "curate: unknown scope tag fails with JSON error" \
  "$PZT" curate smoke --count 1 --tag 不存在的标签 --json

assert_nonzero_exit_with_error "curate: missing --count fails with JSON error" \
  "$PZT" curate smoke --json

# --- pzt tag clear ---
# 承接上面 curate 段留下的状态：a.jpg 同时打了"精选"和"ins"。清掉
# "精选"应该只摘掉打了它的那些图，不动"ins"，也不影响标签本身不存在
# 的情况(幂等)——是 agent 想"重新 curate 一批"时的清场命令。
out="$("$PZT" tag clear smoke 精选 --json)"
assert_json_has "$out" "j['cleared'] == 2" "tag clear: removes the tag from every image carrying it"

out="$("$PZT" images smoke --json)"
assert_json_has "$out" "all('精选' not in i['tags'] for i in j['images'])" \
  "tag clear: 精选 is gone from every image"
assert_json_has "$out" "any('ins' in i['tags'] for i in j['images'])" \
  "tag clear: unrelated tags (ins) are untouched"

out="$("$PZT" tag clear smoke 从没用过的标签 --json)"
assert_json_has "$out" "j['cleared'] == 0" "tag clear: unknown tag name is idempotent, not an error"

assert_nonzero_exit_with_error "tag clear: unknown project fails with JSON error" \
  "$PZT" tag clear does-not-exist 精选 --json

# --- pzt new --json ---
PHOTOS2="$WORKDIR/photos2"
mkdir -p "$PHOTOS2"
printf 'x%.0s' {1..30} > "$PHOTOS2/d.jpg"
printf 'x%.0s' {1..30} > "$PHOTOS2/e.jpg"
out="$("$PZT" new smoke2 "$PHOTOS2" --json)"
assert_json_has "$out" "j['project'] == 'smoke2'" "new --json: echoes back the project name"
assert_json_has "$out" "j['image_count'] == 2" "new --json: reports the scanned image count"

assert_nonzero_exit_with_error "new --json: duplicate project name fails with JSON error" \
  "$PZT" new smoke2 "$PHOTOS2" --json

EMPTY="$WORKDIR/empty_folder"
mkdir -p "$EMPTY"
assert_nonzero_exit_with_error "new --json: empty folder fails with JSON error" \
  "$PZT" new smoke3 "$EMPTY" --json

echo ""
echo "== headless smoke: $pass_count passed, $fail_count failed =="
if [ "$fail_count" -ne 0 ]; then
  exit 1
fi
