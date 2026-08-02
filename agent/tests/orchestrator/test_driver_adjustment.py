from orchestrator.driver import Driver
from orchestrator.types import Plan, PlanDelta, RunState, RunStatus, StageSpec, StageStatus
from store.run_store import RunStore

from fakes import FakeStage


def make_pipeline_run():
    stages = {
        "Ingest": FakeStage(name="Ingest"),
        "StageB": FakeStage(name="StageB", inputs=["Ingest"]),
        "Dedup": FakeStage(name="Dedup", inputs=["StageB"]),
        "Curate": FakeStage(name="Curate", inputs=["Dedup"]),
        "Style": FakeStage(name="Style", inputs=["Curate"]),
        "Caption": FakeStage(name="Caption", inputs=["Curate"]),
        "Deliver": FakeStage(name="Deliver", inputs=["Style", "Caption"]),
    }
    plan = Plan(stages=[StageSpec(name=n) for n in stages])
    run = RunState(
        run_id="run-1", project_id="proj-1", plan=plan,
        stage_states={n: StageStatus.DONE for n in stages},
        status=RunStatus.AWAITING_REVIEW,
    )
    return run, stages


def test_adjusting_curate_invalidates_curate_and_downstream_only(tmp_path):
    run, stages = make_pipeline_run()
    driver = Driver(stages=stages, store=RunStore(tmp_path))

    driver.apply_adjustment(run, PlanDelta(stage_name="Curate", params={"count": 12}))

    assert run.stage_states["Curate"] == StageStatus.PENDING
    assert run.stage_states["Style"] == StageStatus.PENDING
    assert run.stage_states["Caption"] == StageStatus.PENDING
    assert run.stage_states["Deliver"] == StageStatus.PENDING
    assert run.stage_states["Ingest"] == StageStatus.DONE
    assert run.stage_states["StageB"] == StageStatus.DONE
    assert run.stage_states["Dedup"] == StageStatus.DONE
    curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
    assert curate_spec.params == {"count": 12}


def test_adjusting_caption_does_not_rerun_eval_dedup_curate_style(tmp_path):
    run, stages = make_pipeline_run()
    driver = Driver(stages=stages, store=RunStore(tmp_path))

    driver.apply_adjustment(run, PlanDelta(stage_name="Caption", params={"tone": "lively"}))

    assert run.stage_states["Caption"] == StageStatus.PENDING
    assert run.stage_states["Deliver"] == StageStatus.PENDING
    for untouched in ("Ingest", "StageB", "Dedup", "Curate", "Style"):
        assert run.stage_states[untouched] == StageStatus.DONE


def test_rerun_stage_leaves_its_own_gate_armed_so_a_later_adjustment_reasks(tmp_path):
    """钉住**当前**行为，它是一个已知缺陷，见 docs/issues/intent-curation/
    12-rerun-stage-leaves-its-gate-armed.md（票 11 落地时发现）。

    `rerun_stage` 的契约写的是"闸门已经问过、这次给的就是答案，不需要闸门
    再问一遍"，但它只跳过**这一次**，`spec.gate` 原封不动。于是"只说去重没
    给数量"那条流程（Curate `gate="required"`）里，用户之后在选片闸门上做
    任何一次调整，`apply_adjustment` 把 Curate 重置成 PENDING，下一次
    `advance()` 又拿 `gate="required"` 把"去重后还剩 N 张，要不要再筛选一
    下？"问出来 - Curate 压根没重跑（下面 calls 仍是 1）。

    修它要动 `spec.gate` 的生命周期，而那条路还被票 10 的 rewind→rearm_gate
    和 AG-01 的 Style 重问共用，不适合在票 11 里顺手改。票 12 落地时这个
    测试应当被**有意**翻过来。
    """
    run, stages = make_pipeline_run()
    curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
    curate_spec.gate = "required"
    curate_spec.params["count"] = None
    driver = Driver(stages=stages, store=RunStore(tmp_path))

    driver.rerun_stage(run, "Curate", {"count": 5})  # 追问答完："留5张"
    driver.apply_adjustment(  # 选片闸门上改题材要求
        run, PlanDelta(stage_name="Curate", params={"selection_brief": "表情活泼"}))
    driver.advance(run)

    assert curate_spec.gate == "required"
    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "Curate"
    assert len(stages["Curate"].calls) == 1  # 没重跑，只是又问了一遍


def test_adjustment_then_advance_reruns_only_invalidated_stages(tmp_path):
    run, stages = make_pipeline_run()
    driver = Driver(stages=stages, store=RunStore(tmp_path))

    driver.apply_adjustment(run, PlanDelta(stage_name="Curate", params={"count": 12}))
    for _ in range(10):
        if run.status != RunStatus.RUNNING:
            break
        driver.advance(run)

    assert len(stages["Ingest"].calls) == 0
    assert len(stages["StageB"].calls) == 0
    assert len(stages["Dedup"].calls) == 0
    assert len(stages["Curate"].calls) == 1
    assert len(stages["Style"].calls) == 1
    assert len(stages["Caption"].calls) == 1
    assert len(stages["Deliver"].calls) == 1
    assert run.status == RunStatus.AWAITING_REVIEW
