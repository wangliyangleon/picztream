"""Driver 把开销回调接到 stage 上（票 10）- cost 链路的第一段。

跟 test_driver_progress.py 同构：Driver 不知道开销送去哪，只负责绑上当
前 stage 名再塞进 StageContext。分开一个 sink 而不是复用 progress_sink，
理由见 pzt_client.CostFn 的说明。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, List, Literal, Tuple

from orchestrator.driver import Driver
from orchestrator.stage import StageContext
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus
from store.run_store import RunStore


@dataclass
class CostingStage:
    name: str
    comparisons: int = 18
    evaluations: int = 6
    inputs: List[str] = field(default_factory=list)
    cost_class: Literal["local", "cloud"] = "local"
    criticality: Literal["critical", "optional"] = "critical"

    def run(self, ctx: StageContext, params: dict[str, Any]) -> StageOutput:
        ctx.on_cost(self.comparisons, self.evaluations)
        return StageOutput(ok=True)


def make_run(plan: Plan) -> RunState:
    return RunState(
        run_id="run-1",
        project_id="proj-1",
        plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.RUNNING,
    )


def test_cost_sink_receives_stage_name_and_counts(tmp_path):
    stage = CostingStage(name="Curate", comparisons=9, evaluations=6)
    plan = Plan(stages=[StageSpec(name="Curate")])
    driver = Driver(stages={"Curate": stage}, store=RunStore(tmp_path))
    seen: List[Tuple[str, int, int]] = []
    driver.cost_sink = lambda name, comparisons, evaluations: seen.append(
        (name, comparisons, evaluations))

    driver.advance(make_run(plan))

    assert seen == [("Curate", 9, 6)]


def test_stage_can_report_cost_with_no_sink_attached(tmp_path):
    stage = CostingStage(name="Curate")
    plan = Plan(stages=[StageSpec(name="Curate")])
    driver = Driver(stages={"Curate": stage}, store=RunStore(tmp_path))

    run = make_run(plan)
    driver.advance(run)

    assert run.stage_states["Curate"] == StageStatus.DONE


def test_cost_sink_is_rebound_per_stage(tmp_path):
    # 真机场景就是这个：Dedup 先报一次，Curate 跑完分簇之后再报一次。两条
    # 消息要能分辨出是哪个 stage 报的，名字不能是缓存住的第一个。
    first = CostingStage(name="Dedup", comparisons=18, evaluations=0)
    second = CostingStage(name="Curate", comparisons=0, evaluations=6)
    plan = Plan(stages=[StageSpec(name="Dedup"), StageSpec(name="Curate")])
    driver = Driver(stages={"Dedup": first, "Curate": second}, store=RunStore(tmp_path))
    seen: List[Tuple[str, int, int]] = []
    driver.cost_sink = lambda name, comparisons, evaluations: seen.append(
        (name, comparisons, evaluations))

    run = make_run(plan)
    driver.advance(run)
    driver.advance(run)

    assert seen == [("Dedup", 18, 0), ("Curate", 0, 6)]
