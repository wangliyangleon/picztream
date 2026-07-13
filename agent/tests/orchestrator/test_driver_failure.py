from orchestrator.driver import Driver
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus
from store.run_store import RunStore

from fakes import FakeStage


def make_run(plan: Plan) -> RunState:
    return RunState(
        run_id="run-1", project_id="proj-1", plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.RUNNING,
    )


def test_critical_stage_failure_fails_the_run_and_stops(tmp_path):
    ingest = FakeStage(name="Ingest", result=StageOutput(ok=False, error="disk full"), criticality="critical")
    evaluate = FakeStage(name="Evaluate", inputs=["Ingest"])
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="Evaluate")])
    run = make_run(plan)
    driver = Driver(stages={"Ingest": ingest, "Evaluate": evaluate}, store=RunStore(tmp_path))

    driver.advance(run)

    assert run.status == RunStatus.FAILED
    assert run.stage_states["Ingest"] == StageStatus.FAILED
    assert len(evaluate.calls) == 0


def test_optional_stage_failure_degrades_and_run_continues(tmp_path):
    style = FakeStage(name="Style", result=StageOutput(ok=False, error="no matching preset"), criticality="optional")
    deliver = FakeStage(name="Deliver", inputs=["Style"])
    plan = Plan(stages=[StageSpec(name="Style"), StageSpec(name="Deliver")])
    run = make_run(plan)
    driver = Driver(stages={"Style": style, "Deliver": deliver}, store=RunStore(tmp_path))

    driver.advance(run)
    assert run.stage_states["Style"] == StageStatus.SKIPPED
    assert run.status == RunStatus.RUNNING

    driver.advance(run)
    assert run.stage_states["Deliver"] == StageStatus.DONE


def test_partial_item_failure_inside_a_stage_is_not_a_stage_failure(tmp_path):
    evaluate = FakeStage(
        name="Evaluate",
        result=StageOutput(ok=True, data={"evaluated": 38}, skipped=["bad1.jpg", "bad2.jpg"]),
    )
    plan = Plan(stages=[StageSpec(name="Evaluate")])
    run = make_run(plan)
    driver = Driver(stages={"Evaluate": evaluate}, store=RunStore(tmp_path))

    driver.advance(run)

    assert run.stage_states["Evaluate"] == StageStatus.DONE
    assert run.outputs["Evaluate"].skipped == ["bad1.jpg", "bad2.jpg"]
