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
