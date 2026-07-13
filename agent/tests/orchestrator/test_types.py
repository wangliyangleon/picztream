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
        StageSpec(name="Evaluate", params={"provider": "gemini"}, gate="courtesy", gate_on_timeout="proceed"),
    ])
    return RunState(
        run_id="run-1",
        project_id="proj-1",
        plan=plan,
        stage_states={"Ingest": StageStatus.DONE, "Evaluate": StageStatus.PENDING},
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
    assert restored.stage_states["Evaluate"] == StageStatus.PENDING
    assert restored.plan.stages[1].gate == "courtesy"
    assert restored.plan.stages[1].gate_on_timeout == "proceed"
    assert restored.outputs["Ingest"].data == {"image_count": 40}


def test_run_state_round_trip_preserves_gate_state():
    run = make_sample_run()
    run.gate_state = GateState(stage_name="Evaluate", setting="courtesy", decision=None)

    restored = run_state_from_dict(json.loads(json.dumps(asdict(run))))

    assert restored.gate_state == GateState(stage_name="Evaluate", setting="courtesy", decision=None)


def test_stage_status_is_json_serializable_directly_as_its_string_value():
    # StageStatus(str, Enum) 混入 str，asdict() 出来的字典里它还是
    # Enum 实例，但 json.dumps 应该把它当纯字符串序列化，不需要自定义
    # encoder——这条测试锁住这个隐含依赖，以后别人改成非 str 混入的
    # Enum 会在这里炸。
    assert json.dumps({"s": StageStatus.DONE}) == '{"s": "done"}'
