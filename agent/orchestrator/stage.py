from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Callable, Literal, Protocol

from .types import StageOutput

# 数的是什么。**单位必须跟着数字一起传**：同一个 stage 在不同阶段数的东
# 西不一样（dedup/curate 的本地分簇数候选簇、AI 阶段数比较次数、
# StyleApplyAll 数照片），把它压掉就只能在展示层写死一个单位，然后在另
# 外两种情况下说错话。真机验收就是这么发现的：用户要选 3 张，界面报"已
# 完成 1/1 张"（那其实是 1 个候选簇）。
#
# 传语义 key 而不是"组"/"张"这种词：prose 归 consumer/view 渲染，
# orchestrator 层不碰对话文本。
PROGRESS_PHOTOS = "photos"            # 张（套滤镜）
PROGRESS_GROUPS = "groups"            # 组（候选簇）
PROGRESS_COMPARISONS = "comparisons"  # 次（两两比较）
PROGRESS_EVALUATIONS = "evaluations"  # 张（逐张评估）

# 第四类的单位跟 PROGRESS_PHOTOS 一样是"张"，仍然要单独一类（票 09）：这
# 个维度同时扛着"单位"和"在干什么"两件事，而"张"那一类的活动已经被套滤镜
# 占用，复用会让用户在评估阶段看到"正在套滤镜，已完成 7/18 张"。只加一类
# 不拆这个维度 - 拆是更大的改动，债记在 PRD 决策二十里。

# (done, total, kind)。stage 只报数和单位，不报自己叫什么 - 名字由 Driver
# 在 _run_stage 里绑好（stage 自报名字会跟 Plan 里的 key 对不上）。
ProgressFn = Callable[[int, int, str], None]


def _noop_progress(done: int, total: int, kind: str) -> None:
    """默认 sink。有了它，stage 里可以无条件调 ctx.on_progress，不用每处
    上报都写一遍判空；run_watchfolder/run_intent 和大量既有单测都不挂
    sink，走的就是这条。"""
    del done, total, kind


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
