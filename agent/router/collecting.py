"""Telegram 会话路由的 Collecting 阶段：还没等到足够意图/照片、组不出
Plan 之前的 Run 表示，以及照片落到本地的暂存目录规则。RunState.plan
是必填字段，所以"还没规划"用 Plan(stages=[]) 占位，一样能经
store/run_store.py 原样存取，RunStatus.COLLECTING 不在
_TERMINAL_STATUSES 里所以会出现在 list_active() 里。落盘目录命名对齐
stages/deliver.py 的 <state_dir>/staging/<run_id>/ 风格，用
<incoming_root>/<run_id>/。
"""
from __future__ import annotations

import shutil
import uuid
from pathlib import Path

from orchestrator.types import Plan, RunState, RunStatus


def new_run_id() -> str:
    return f"tg-{uuid.uuid4().hex[:8]}"


def new_collecting_run(run_id: str) -> RunState:
    return RunState(
        run_id=run_id,
        project_id=run_id,
        plan=Plan(stages=[]),
        stage_states={},
        status=RunStatus.COLLECTING,
    )


def incoming_dir_for(incoming_root: Path, run_id: str) -> Path:
    path = Path(incoming_root) / run_id
    path.mkdir(parents=True, exist_ok=True)
    return path


def stage_incoming_photo(incoming_root: Path, run_id: str, src_path: str) -> Path:
    dest_dir = incoming_dir_for(incoming_root, run_id)
    dest_path = dest_dir / Path(src_path).name
    shutil.copy2(src_path, dest_path)
    return dest_path
