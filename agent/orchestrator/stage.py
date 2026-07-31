from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Literal, Protocol

from .types import StageOutput

# (done, total)。stage 只报数，不报自己叫什么 - 名字由 Driver 在
# _run_stage 里绑好（stage 自报名字会跟 Plan 里的 key 对不上）。
ProgressFn = Callable[[int, int], None]


def _noop_progress(done: int, total: int) -> None:
    """默认 sink。有了它，stage 里可以无条件调 ctx.on_progress，不用每处
    上报都写一遍判空；run_watchfolder/run_intent 和大量既有单测都不挂
    sink，走的就是这条。"""
    del done, total


@dataclass
class StageContext:
    run_id: str
    project_id: str
    outputs: dict[str, StageOutput] = field(default_factory=dict)
    on_progress: ProgressFn = _noop_progress


class Stage(Protocol):
    name: str
    inputs: list[str]
    cost_class: Literal["local", "cloud"]
    criticality: Literal["critical", "optional"]

    def run(self, ctx: StageContext, params: dict[str, Any]) -> StageOutput: ...
