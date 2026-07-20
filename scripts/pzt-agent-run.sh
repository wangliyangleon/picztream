#!/usr/bin/env bash
# 便捷启动 pzt-agent:读一个 .env 再前台跑。给源码用户/长驻用。
# 用法: scripts/pzt-agent-run.sh [env 文件, 默认 ~/.pzt-agent.env] [-- pzt-agent 额外参数]
# 长驻建议放 tmux: tmux new -s pzt 'scripts/pzt-agent-run.sh'
set -euo pipefail

ENV_FILE="${1:-$HOME/.pzt-agent.env}"
shift || true
[[ "${1:-}" == "--" ]] && shift || true

if [[ ! -f "$ENV_FILE" ]]; then
  echo "找不到 env 文件: $ENV_FILE" >&2
  echo "从样例拷一份:cp \"\$(brew --prefix)/share/pzt-agent/pzt-agent.env.example\" \"$ENV_FILE\"" >&2
  exit 1
fi

set -a
# shellcheck disable=SC1090
source "$ENV_FILE"
set +a

exec pzt-agent "$@"
