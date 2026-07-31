"""Driver 把进度回调接到 stage 上 - T-8 进度链路的第一段。

Driver 自己不知道进度要送去哪，它只负责把 progress_sink（由 worker 挂
上，跟 client.cancel_event 同一个套路）绑上当前 stage 名之后塞进
StageContext。stage 只管调 ctx.on_progress(done, total)：不需要知道自
己叫什么（名字由 Driver 绑，避免 stage 自报名字跟 Plan 里的 key 对不
上），也不需要判空（没挂 sink 时默认是个 no-op）。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, List, Literal, Tuple

from orchestrator.driver import Driver
from orchestrator.stage import StageContext
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus
from store.run_store import RunStore


@dataclass
class ProgressingStage:
    name: str
    steps: int = 3
    inputs: List[str] = field(default_factory=list)
    cost_class: Literal["local", "cloud"] = "local"
    criticality: Literal["critical", "optional"] = "critical"

    def run(self, ctx: StageContext, params: dict[str, Any]) -> StageOutput:
        for i in range(1, self.steps + 1):
            ctx.on_progress(i, self.steps, "photos")
        return StageOutput(ok=True)


def make_run(plan: Plan) -> RunState:
    return RunState(
        run_id="run-1",
        project_id="proj-1",
        plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.RUNNING,
    )


def test_progress_sink_receives_stage_name_and_counts(tmp_path):
    stage = ProgressingStage(name="StyleApplyAll", steps=3)
    plan = Plan(stages=[StageSpec(name="StyleApplyAll")])
    driver = Driver(stages={"StyleApplyAll": stage}, store=RunStore(tmp_path))
    seen: List[Tuple[str, int, int, str]] = []
    driver.progress_sink = lambda name, done, total, kind: seen.append((name, done, total, kind))

    driver.advance(make_run(plan))

    assert seen == [("StyleApplyAll", 1, 3, "photos"), ("StyleApplyAll", 2, 3, "photos"),
                    ("StyleApplyAll", 3, 3, "photos")]


def test_stage_can_report_progress_with_no_sink_attached(tmp_path):
    # run_watchfolder / run_intent / 大量既有单测都不挂 sink。默认值必须
    # 是个能调的 no-op，否则 stage 里每处上报都要写一遍判空。
    stage = ProgressingStage(name="StyleApplyAll", steps=2)
    plan = Plan(stages=[StageSpec(name="StyleApplyAll")])
    driver = Driver(stages={"StyleApplyAll": stage}, store=RunStore(tmp_path))

    run = make_run(plan)
    driver.advance(run)

    assert run.stage_states["StyleApplyAll"] == StageStatus.DONE


def test_sink_is_rebound_per_stage(tmp_path):
    # 同一个 sink 服务整条 Plan，stage 名必须每次重新绑，不能在 Driver 上
    # 缓存一份 - 否则第二个 stage 的进度会顶着第一个的名字上报。
    first = ProgressingStage(name="Dedup", steps=1)
    second = ProgressingStage(name="Curate", steps=1)
    plan = Plan(stages=[StageSpec(name="Dedup"), StageSpec(name="Curate")])
    driver = Driver(stages={"Dedup": first, "Curate": second}, store=RunStore(tmp_path))
    seen: List[Tuple[str, int, int, str]] = []
    driver.progress_sink = lambda name, done, total, kind: seen.append((name, done, total, kind))

    run = make_run(plan)
    driver.advance(run)
    driver.advance(run)

    assert [row[0] for row in seen] == ["Dedup", "Curate"]
