import pytest

from orchestrator.driver import Driver
from orchestrator.types import Plan, RunState, RunStatus, StageSpec, StageStatus
from store.run_store import RunStore

from fakes import FakeStage


def make_run(plan: Plan) -> RunState:
    return RunState(
        run_id="run-1", project_id="proj-1", plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.RUNNING,
    )


def test_courtesy_gate_pauses_then_proceeds_on_resolve(tmp_path):
    stage = FakeStage(name="Style")
    plan = Plan(stages=[StageSpec(name="Style", gate="courtesy")])
    run = make_run(plan)
    driver = Driver(stages={"Style": stage}, store=RunStore(tmp_path))

    driver.advance(run)
    assert run.status == RunStatus.AWAITING_GATE
    assert len(stage.calls) == 0

    driver.resolve_gate(run, "proceed")
    assert run.stage_states["Style"] == StageStatus.DONE
    assert len(stage.calls) == 1


def test_courtesy_gate_timeout_defaults_to_proceed(tmp_path):
    stage = FakeStage(name="Style")
    plan = Plan(stages=[StageSpec(name="Style", gate="courtesy", gate_on_timeout="proceed")])
    run = make_run(plan)
    driver = Driver(stages={"Style": stage}, store=RunStore(tmp_path))

    driver.advance(run)
    driver.timeout_gate(run)

    assert run.stage_states["Style"] == StageStatus.DONE


def test_courtesy_gate_timeout_can_hold_and_cancels_run(tmp_path):
    stage = FakeStage(name="Caption")
    plan = Plan(stages=[StageSpec(name="Caption", gate="courtesy", gate_on_timeout="hold")])
    run = make_run(plan)
    driver = Driver(stages={"Caption": stage}, store=RunStore(tmp_path))

    driver.advance(run)
    driver.timeout_gate(run)

    assert run.status == RunStatus.CANCELLED
    assert len(stage.calls) == 0


def test_required_gate_has_no_timeout_and_waits_for_explicit_answer(tmp_path):
    stage = FakeStage(name="Caption")
    plan = Plan(stages=[StageSpec(name="Caption", gate="required")])
    run = make_run(plan)
    driver = Driver(stages={"Caption": stage}, store=RunStore(tmp_path))

    driver.advance(run)
    assert run.status == RunStatus.AWAITING_GATE

    with pytest.raises(ValueError):
        driver.timeout_gate(run)

    driver.resolve_gate(run, "proceed")
    assert run.stage_states["Caption"] == StageStatus.DONE


def test_off_gate_never_pauses(tmp_path):
    stage = FakeStage(name="Ingest")
    plan = Plan(stages=[StageSpec(name="Ingest", gate="off")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": stage}, store=RunStore(tmp_path))

    driver.advance(run)

    assert run.stage_states["Ingest"] == StageStatus.DONE
    assert run.status != RunStatus.AWAITING_GATE
