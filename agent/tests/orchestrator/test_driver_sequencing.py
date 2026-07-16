from orchestrator.driver import Driver
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus
from store.run_store import RunStore

from fakes import FakeStage


def make_run(plan: Plan) -> RunState:
    return RunState(
        run_id="run-1",
        project_id="proj-1",
        plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.RUNNING,
    )


def test_advance_runs_stages_in_dependency_order(tmp_path):
    a = FakeStage(name="Ingest")
    b = FakeStage(name="Evaluate", inputs=["Ingest"])
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="Evaluate")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a, "Evaluate": b}, store=RunStore(tmp_path))

    driver.advance(run)
    assert run.stage_states["Ingest"] == StageStatus.DONE
    assert run.stage_states["Evaluate"] == StageStatus.PENDING

    driver.advance(run)
    assert run.stage_states["Evaluate"] == StageStatus.DONE
    assert b.calls[0]["outputs_seen"] == {"Ingest"}


def test_advance_transitions_to_awaiting_review_when_all_stages_done(tmp_path):
    a = FakeStage(name="Ingest")
    plan = Plan(stages=[StageSpec(name="Ingest")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a}, store=RunStore(tmp_path))

    driver.advance(run)
    driver.advance(run)

    assert run.status == RunStatus.AWAITING_REVIEW


def test_approve_moves_awaiting_review_to_done(tmp_path):
    a = FakeStage(name="Ingest")
    plan = Plan(stages=[StageSpec(name="Ingest")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a}, store=RunStore(tmp_path))
    driver.advance(run)
    driver.advance(run)

    driver.approve(run)

    assert run.status == RunStatus.DONE


def test_cancel_moves_run_to_cancelled(tmp_path):
    a = FakeStage(name="Ingest")
    plan = Plan(stages=[StageSpec(name="Ingest")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a}, store=RunStore(tmp_path))

    driver.cancel(run)

    assert run.status == RunStatus.CANCELLED


def test_each_stage_boundary_persists_run_state(tmp_path):
    a = FakeStage(name="Ingest")
    plan = Plan(stages=[StageSpec(name="Ingest")])
    run = make_run(plan)
    store = RunStore(tmp_path)
    driver = Driver(stages={"Ingest": a}, store=store)

    driver.advance(run)

    assert store.load("run-1").stage_states["Ingest"] == StageStatus.DONE


def test_peek_next_stage_returns_first_stage_name_without_side_effects(tmp_path):
    a = FakeStage(name="Ingest")
    b = FakeStage(name="Evaluate", inputs=["Ingest"])
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="Evaluate")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a, "Evaluate": b}, store=RunStore(tmp_path))

    result = driver.peek_next_stage(run)

    assert result == "Ingest"
    # 纯查询，不该产生任何副作用。
    assert run.stage_states == {"Ingest": StageStatus.PENDING, "Evaluate": StageStatus.PENDING}
    assert run.outputs == {}
    assert a.calls == []


def test_peek_next_stage_tracks_advance_through_the_plan(tmp_path):
    a = FakeStage(name="Ingest")
    b = FakeStage(name="Evaluate", inputs=["Ingest"])
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="Evaluate")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a, "Evaluate": b}, store=RunStore(tmp_path))

    assert driver.peek_next_stage(run) == "Ingest"
    driver.advance(run)
    assert run.stage_states["Ingest"] == StageStatus.DONE

    assert driver.peek_next_stage(run) == "Evaluate"
    driver.advance(run)
    assert run.stage_states["Evaluate"] == StageStatus.DONE


def test_peek_next_stage_returns_none_when_nothing_is_pending(tmp_path):
    a = FakeStage(name="Ingest")
    plan = Plan(stages=[StageSpec(name="Ingest")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": a}, store=RunStore(tmp_path))

    driver.advance(run)
    driver.advance(run)
    assert run.status == RunStatus.AWAITING_REVIEW

    assert driver.peek_next_stage(run) is None


def test_rerun_stage_runs_the_target_immediately_without_triggering_its_own_gate(tmp_path):
    a = FakeStage(name="Style")
    plan = Plan(stages=[StageSpec(name="Style", gate="required")])
    run = make_run(plan)
    driver = Driver(stages={"Style": a}, store=RunStore(tmp_path))

    driver.rerun_stage(run, "Style", {"style_description": "暖色调怀旧"})

    # 闸门是 required，但 rerun_stage 已经拿到答案了，不该再停下来问一遍；
    # 跑完之后是不是继续推进到 AWAITING_REVIEW/下一个闸门是调用方（router
    # 的 _drive_to_stop_and_notify）的事，跟 resolve_gate 的既有行为一致，
    # rerun_stage 本身只负责把目标 stage 跑完。
    assert run.status == RunStatus.RUNNING
    assert run.stage_states["Style"] == StageStatus.DONE
    assert a.calls[0]["params"]["style_description"] == "暖色调怀旧"


def test_rerun_stage_resets_downstream_state_and_outputs(tmp_path):
    a = FakeStage(name="Style")
    b = FakeStage(name="StyleApplyAll", inputs=["Style"])
    plan = Plan(stages=[StageSpec(name="Style", gate="required"), StageSpec(name="StyleApplyAll", gate="required")])
    run = make_run(plan)
    driver = Driver(stages={"Style": a, "StyleApplyAll": b}, store=RunStore(tmp_path))
    driver.rerun_stage(run, "Style", {"style_description": "第一版描述"})
    driver.advance(run)  # 触发 StyleApplyAll 的闸门
    assert run.status == RunStatus.AWAITING_GATE
    assert run.stage_states["StyleApplyAll"] == StageStatus.PENDING

    driver.rerun_stage(run, "Style", {"style_description": "重新描述一次"})

    assert run.stage_states["Style"] == StageStatus.DONE
    assert run.stage_states["StyleApplyAll"] == StageStatus.PENDING
    assert "StyleApplyAll" not in run.outputs
    assert run.gate_state is None
    assert a.calls[-1]["params"]["style_description"] == "重新描述一次"


def test_rerun_stage_marks_run_failed_when_target_stage_fails(tmp_path):
    a = FakeStage(name="Style", result=StageOutput(ok=False, error="boom"), criticality="critical")
    plan = Plan(stages=[StageSpec(name="Style", gate="required")])
    run = make_run(plan)
    driver = Driver(stages={"Style": a}, store=RunStore(tmp_path))

    driver.rerun_stage(run, "Style", {"style_description": "暖色调怀旧"})

    assert run.status == RunStatus.FAILED
    assert run.stage_states["Style"] == StageStatus.FAILED
