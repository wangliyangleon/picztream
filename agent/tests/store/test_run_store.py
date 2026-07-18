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


def test_cancelling_marker_crud(tmp_path):
    # AG-12：cancelling sidecar 标记的 mark/is/list/clear。
    store = RunStore(tmp_path)
    assert store.is_cancelling("run-1") is False
    assert store.list_cancelling() == []

    store.mark_cancelling("run-1")
    store.mark_cancelling("run-2")
    assert store.is_cancelling("run-1") is True
    assert set(store.list_cancelling()) == {"run-1", "run-2"}

    store.clear_cancelling("run-1")
    assert store.is_cancelling("run-1") is False
    assert store.list_cancelling() == ["run-2"]
    store.clear_cancelling("run-1")  # 幂等，不报错


def test_cancelling_marker_does_not_leak_into_list_active(tmp_path):
    store = RunStore(tmp_path)
    store.save(make_run("running", RunStatus.RUNNING))
    store.mark_cancelling("running")
    assert {r.run_id for r in store.list_active()} == {"running"}  # .cancelling 不干扰 *.json


def test_terminal_runs_older_than_only_picks_old_terminal(tmp_path):
    # AG-14：只挑"终态 + JSON mtime 够老"的 run。
    import os as _os
    store = RunStore(tmp_path)
    store.save(make_run("old-done", RunStatus.DONE))
    store.save(make_run("fresh-done", RunStatus.DONE))
    store.save(make_run("old-running", RunStatus.RUNNING))  # 非终态，不该选
    now = 1_000_000.0
    _os.utime(tmp_path / "old-done.json", (now - 10 * 86400, now - 10 * 86400))
    _os.utime(tmp_path / "fresh-done.json", (now - 1 * 86400, now - 1 * 86400))
    _os.utime(tmp_path / "old-running.json", (now - 10 * 86400, now - 10 * 86400))

    stale = store.terminal_runs_older_than(7 * 86400, now)

    assert stale == ["old-done"]


def test_delete_run_removes_json_and_marker(tmp_path):
    store = RunStore(tmp_path)
    store.save(make_run("gone", RunStatus.DONE))
    store.mark_cancelling("gone")

    store.delete_run("gone")

    assert not (tmp_path / "gone.json").exists()
    assert store.is_cancelling("gone") is False
    store.delete_run("gone")  # 幂等，不报错


def test_save_overwrites_previous_state_for_same_run_id(tmp_path):
    store = RunStore(tmp_path)
    run = make_run("run-1", RunStatus.RUNNING)
    store.save(run)

    run.status = RunStatus.AWAITING_REVIEW
    store.save(run)

    assert store.load("run-1").status == RunStatus.AWAITING_REVIEW
