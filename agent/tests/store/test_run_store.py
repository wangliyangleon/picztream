import pytest

from orchestrator.types import Plan, RunState, RunStatus, StageSpec, StageStatus
from store.run_store import RunStore


def make_run(run_id: str, status: RunStatus) -> RunState:
    plan = Plan(stages=[StageSpec(name="Ingest")])
    return RunState(
        run_id=run_id,
        project_id="proj-1",
        plan=plan,
        stage_states={"Ingest": StageStatus.PENDING},
        status=status,
    )


def test_save_then_load_round_trips(tmp_path):
    store = RunStore(tmp_path)
    run = make_run("run-1", RunStatus.RUNNING)

    store.save(run)
    loaded = store.load("run-1")

    assert loaded.run_id == "run-1"
    assert loaded.status == RunStatus.RUNNING
    assert loaded.stage_states["Ingest"] == StageStatus.PENDING


def test_load_missing_run_raises(tmp_path):
    store = RunStore(tmp_path)
    with pytest.raises(FileNotFoundError):
        store.load("does-not-exist")


def test_list_active_excludes_terminal_runs(tmp_path):
    store = RunStore(tmp_path)
    store.save(make_run("running", RunStatus.RUNNING))
    store.save(make_run("gated", RunStatus.AWAITING_GATE))
    store.save(make_run("done", RunStatus.DONE))
    store.save(make_run("failed", RunStatus.FAILED))
    store.save(make_run("cancelled", RunStatus.CANCELLED))

    active_ids = {r.run_id for r in store.list_active()}

    assert active_ids == {"running", "gated"}


def test_save_overwrites_previous_state_for_same_run_id(tmp_path):
    store = RunStore(tmp_path)
    run = make_run("run-1", RunStatus.RUNNING)
    store.save(run)

    run.status = RunStatus.AWAITING_REVIEW
    store.save(run)

    assert store.load("run-1").status == RunStatus.AWAITING_REVIEW
