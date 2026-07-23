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


def test_crash_after_stage_done_resumes_without_rerunning_it(tmp_path):
    ingest = FakeStage(name="Ingest")
    stage_b = FakeStage(name="StageB", inputs=["Ingest"])
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="StageB")])
    run = make_run(plan)
    store_a = RunStore(tmp_path)
    driver_a = Driver(stages={"Ingest": ingest, "StageB": stage_b}, store=store_a)
    driver_a.advance(run)  # Ingest 跑完并落盘；此后模拟进程崩溃，run/driver_a 不再使用

    # 模拟重启：全新 Driver + 全新 RunStore 指向同一个落盘目录，Stage
    # 也是全新对象（call 计数从零开始，用来验证 Ingest 不会被重跑）。
    ingest_after_restart = FakeStage(name="Ingest")
    store_b = RunStore(tmp_path)
    driver_b = Driver(stages={"Ingest": ingest_after_restart, "StageB": stage_b}, store=store_b)
    reloaded = store_b.load("run-1")

    driver_b.advance(reloaded)

    assert len(ingest_after_restart.calls) == 0
    assert reloaded.stage_states["StageB"] == StageStatus.DONE


def test_list_active_finds_run_stuck_mid_run_after_restart(tmp_path):
    ingest = FakeStage(name="Ingest")
    stage_b = FakeStage(name="StageB", inputs=["Ingest"])
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="StageB")])
    run = make_run(plan)
    store = RunStore(tmp_path)
    Driver(stages={"Ingest": ingest, "StageB": stage_b}, store=store).advance(run)

    active = RunStore(tmp_path).list_active()

    assert [r.run_id for r in active] == ["run-1"]
