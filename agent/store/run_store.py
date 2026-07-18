"""agent 自己的 Run 持久化：每个 run_id 一个 JSON 文件，绝不进 core 的
pzt.db（core 对编排概念无感知，见 docs/M4_Agent_Workflow_Design.md 第七节）。
"""
from __future__ import annotations

import json
import os
from dataclasses import asdict
from pathlib import Path

from orchestrator.types import RunState, RunStatus, run_state_from_dict

_TERMINAL_STATUSES = {RunStatus.DONE, RunStatus.FAILED, RunStatus.CANCELLED}


class RunStore:
    def __init__(self, root: Path | str) -> None:
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def _path(self, run_id: str) -> Path:
        return self.root / f"{run_id}.json"

    def save(self, run: RunState) -> None:
        # 先写临时文件再 os.replace 原子落盘：崩溃发生在写入过程中不会
        # 留下半份 JSON 挡住下次 load()——这是"每个 Stage 边界持久化"
        # 检查点机制能可靠续跑的前提。
        path = self._path(run.run_id)
        tmp_path = path.with_suffix(".json.tmp")
        tmp_path.write_text(json.dumps(asdict(run), indent=2, sort_keys=True))
        os.replace(tmp_path, path)

    def load(self, run_id: str) -> RunState:
        data = json.loads(self._path(run_id).read_text())
        return run_state_from_dict(data)

    def list_active(self) -> list[RunState]:
        runs = [self.load(p.stem) for p in self.root.glob("*.json")]
        return [r for r in runs if r.status not in _TERMINAL_STATUSES]

    # -- cancelling 标记（AG-12）--
    # drive 期取消是"置 cancel_event + 立即重置会话"，盘上 run 要等 worker
    # 收尾才变 CANCELLED。落一个 sidecar 标记，worker 若在收尾前崩了，下次
    # bootstrap 见标记即补 cancel、不把取消过的批次当"中断"复活。

    def _cancelling_path(self, run_id: str) -> Path:
        return self.root / f"{run_id}.cancelling"

    def mark_cancelling(self, run_id: str) -> None:
        self._cancelling_path(run_id).write_text("")

    def is_cancelling(self, run_id: str) -> bool:
        return self._cancelling_path(run_id).exists()

    def list_cancelling(self) -> list[str]:
        return [p.stem for p in self.root.glob("*.cancelling")]

    def clear_cancelling(self, run_id: str) -> None:
        self._cancelling_path(run_id).unlink(missing_ok=True)

    # -- 保留/清扫（AG-14）--

    def terminal_runs_older_than(self, cutoff_seconds: float, now: float) -> list[str]:
        """终态且其 JSON 落盘时间早于 now-cutoff_seconds 的 run_id（低频清扫
        用）。终态 run 到达终态后不再 save，mtime 即"终态多久了"。"""
        out: list[str] = []
        for p in self.root.glob("*.json"):
            try:
                run = self.load(p.stem)
            except (FileNotFoundError, ValueError, KeyError):
                continue
            if run.status not in _TERMINAL_STATUSES:
                continue
            if p.stat().st_mtime < now - cutoff_seconds:
                out.append(p.stem)
        return out

    def delete_run(self, run_id: str) -> None:
        self._path(run_id).unlink(missing_ok=True)
        self.clear_cancelling(run_id)
