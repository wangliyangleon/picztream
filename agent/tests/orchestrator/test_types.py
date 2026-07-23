import json
from dataclasses import asdict

from orchestrator.types import (
    GateState,
    Plan,
    RunState,
    RunStatus,
    StageOutput,
    StageSpec,
    StageStatus,
    run_state_from_dict,
)


def make_sample_run() -> RunState:
    plan = Plan(stages=[
        StageSpec(name="Ingest", params={"folder": "/tmp/x"}, gate="off"),
        StageSpec(name="StageB", params={"provider": "gemini"}, gate="courtesy", gate_on_timeout="proceed"),
    ])
    return RunState(
        run_id="run-1",
        project_id="proj-1",
        plan=plan,
        stage_states={"Ingest": StageStatus.DONE, "StageB": StageStatus.PENDING},
        outputs={"Ingest": StageOutput(ok=True, data={"image_count": 40})},
        gate_state=None,
        intent_raw="选 9 张发朋友圈",
        status=RunStatus.RUNNING,
    )


def test_run_state_round_trips_through_json():
    run = make_sample_run()
    text = json.dumps(asdict(run))
    restored = run_state_from_dict(json.loads(text))

    assert restored.run_id == run.run_id
    assert restored.status == RunStatus.RUNNING
    assert restored.stage_states["Ingest"] == StageStatus.DONE
    assert restored.stage_states["StageB"] == StageStatus.PENDING
    assert restored.plan.stages[1].gate == "courtesy"
    assert restored.plan.stages[1].gate_on_timeout == "proceed"
    assert restored.outputs["Ingest"].data == {"image_count": 40}


def test_run_state_round_trip_preserves_gate_state():
    run = make_sample_run()
    run.gate_state = GateState(stage_name="StageB", setting="courtesy", decision=None)

    restored = run_state_from_dict(json.loads(json.dumps(asdict(run))))

    assert restored.gate_state == GateState(stage_name="StageB", setting="courtesy", decision=None)


def test_stage_status_is_json_serializable_directly_as_its_string_value():
    # StageStatus(str, Enum) 混入 str，asdict() 出来的字典里它还是
    # Enum 实例，但 json.dumps 应该把它当纯字符串序列化，不需要自定义
    # encoder——这条测试锁住这个隐含依赖，以后别人改成非 str 混入的
    # Enum 会在这里炸。
    assert json.dumps({"s": StageStatus.DONE}) == '{"s": "done"}'


def test_run_state_round_trips_activity_tracking_fields():
    run = make_sample_run()
    run.last_activity_at = 1700000000.5
    run.reminder_sent = True

    restored = run_state_from_dict(json.loads(json.dumps(asdict(run))))

    assert restored.last_activity_at == 1700000000.5
    assert restored.reminder_sent is True


def test_run_state_from_dict_defaults_activity_fields_when_missing():
    run = make_sample_run()
    data = json.loads(json.dumps(asdict(run)))
    del data["last_activity_at"]
    del data["reminder_sent"]

    restored = run_state_from_dict(data)

    assert restored.last_activity_at is None
    assert restored.reminder_sent is False


def test_run_state_round_trips_last_progress_notified_at():
    run = make_sample_run()
    run.last_progress_notified_at = 1700000100.0

    restored = run_state_from_dict(json.loads(json.dumps(asdict(run))))

    assert restored.last_progress_notified_at == 1700000100.0


def test_run_state_from_dict_defaults_last_progress_notified_at_when_missing():
    run = make_sample_run()
    data = json.loads(json.dumps(asdict(run)))
    del data["last_progress_notified_at"]

    restored = run_state_from_dict(data)

    assert restored.last_progress_notified_at is None
