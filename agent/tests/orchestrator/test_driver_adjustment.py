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
