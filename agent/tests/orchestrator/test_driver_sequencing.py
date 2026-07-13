from orchestrator.driver import Driver
from orchestrator.types import Plan, RunState, RunStatus, StageSpec, StageStatus
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
