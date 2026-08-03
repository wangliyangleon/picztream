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


def _deferred_curate_driver(tmp_path):
    """"只说去重没给数量"那条流程的形状：Curate 带 required 闸门、count 待定。"""
    run, stages = make_pipeline_run()
    curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
    curate_spec.gate = "required"
    curate_spec.params["count"] = None
    return run, stages, curate_spec, Driver(stages=stages, store=RunStore(tmp_path))


def test_answered_gate_stays_answered_so_a_later_adjustment_actually_reruns(tmp_path):
    """票 12：`rerun_stage(mark_gate_answered=True)` 之后，选片闸门上的调整
    必须真的重跑 Curate，而不是把"去重后还剩 N 张，要不要再筛选一下？"再问
    一遍。这个测试此前钉的是缺陷本身（calls 停在 1、status 回到
    AWAITING_GATE），票 12 把它**有意**翻了过来。"""
    run, stages, curate_spec, driver = _deferred_curate_driver(tmp_path)

    driver.rerun_stage(run, "Curate", {"count": 5}, mark_gate_answered=True)  # 追问答完
    driver.apply_adjustment(  # 选片闸门上改题材要求
        run, PlanDelta(stage_name="Curate", params={"selection_brief": "表情活泼"}))
    driver.advance(run)

    assert run.gate_state is None
    assert run.status == RunStatus.RUNNING
    assert len(stages["Curate"].calls) == 2  # 真的重跑了
    assert curate_spec.params["selection_brief"] == "表情活泼"


def test_answering_a_gate_does_not_rewrite_its_gate_setting(tmp_path):
    """`gate`（配置）与 `gate_answered`（这问题已经有答案了）必须是两个东西。

    票 10 的 rewind 路径用 `curate.gate != "off"` 判断要不要重问"要不要用
    AI"（consumer.py `_on_run_rewound`），把 `gate` 直接改成 "off" 会静默
    掐掉那条路径。"""
    run, _, curate_spec, driver = _deferred_curate_driver(tmp_path)

    driver.rerun_stage(run, "Curate", {"count": 5}, mark_gate_answered=True)

    assert curate_spec.gate == "required"  # 配置没被改写
    assert curate_spec.gate_answered is True


def test_rerun_stage_does_not_answer_the_gate_unless_asked(tmp_path):
    """opt-in：默认不动 `gate_answered`。rerun_style 依赖这条 - Style 的闸门
    在描述没匹配上 preset 时要靠 AG-01 重新问一次。"""
    run, stages = make_pipeline_run()
    style_spec = next(s for s in run.plan.stages if s.name == "Style")
    style_spec.gate = "required"
    driver = Driver(stages=stages, store=RunStore(tmp_path))

    driver.rerun_stage(run, "Style", {"style_description": "胶片感"})

    assert style_spec.gate_answered is False
    driver.rearm_gate(run, "Style")
    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "Style"


def test_rearm_gate_reopens_an_already_answered_question(tmp_path):
    """重新挂闸门 = 又要问一遍，之前那个答案不再算数。票 10 的"停下"路径
    正是这个形状：追问答过了，但用户把 Curate 停了，要回到"要不要用 AI"。"""
    run, _, curate_spec, driver = _deferred_curate_driver(tmp_path)
    driver.rerun_stage(run, "Curate", {"count": 5}, mark_gate_answered=True)

    driver.rearm_gate(run, "Curate")

    assert curate_spec.gate_answered is False
    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "Curate"
    assert run.gate_state.setting == "required"


def test_gate_answered_survives_a_store_roundtrip(tmp_path):
    """标记落在 StageSpec 上，必须跟着 run 一起持久化 - 否则进程重启后
    闸门又活过来了。"""
    run, _, _, driver = _deferred_curate_driver(tmp_path)
    store = RunStore(tmp_path)
    driver.rerun_stage(run, "Curate", {"count": 5}, mark_gate_answered=True)

    reloaded = store.load(run.run_id)

    spec = next(s for s in reloaded.plan.stages if s.name == "Curate")
    assert spec.gate_answered is True
    assert spec.gate == "required"


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
