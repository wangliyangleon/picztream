"""SessionView 是 consumer 私有的内存视图（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第四节）：从 RunState 重建、被事件更新、渲染 describe()
快速应答。这里测三件事：from_run 重建各状态、describe 各分支文案（
COLLECTING/PLANNED/AWAITING_GATE 逐字对齐旧 _status_snapshot_text，
RUNNING 是新增分支）、photo_count 的目录现算。
"""
from __future__ import annotations

from orchestrator.types import (
    Plan,
    RunState,
    RunStatus,
    StageOutput,
    StageSpec,
    StageStatus,
)
from router.collecting import incoming_dir_for, new_collecting_run
from session.protocol import DriveJob
from session.view import SessionView, view_from_run


def _planned_run() -> RunState:
    plan = Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Evaluate", params={"provider": "local", "auto_reject": True}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 5, "apply_tag": "精选"}),
        StageSpec(name="Deliver", gate="required"),
    ])
    return RunState(
        run_id="tg-abc", project_id="tg-abc", plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.PLANNED,
    )


def test_from_run_on_collecting_placeholder(tmp_path):
    run = new_collecting_run("tg-c1")
    view = view_from_run(run, incoming_root=tmp_path / "incoming")

    assert view.run_id == "tg-c1"
    assert view.status == RunStatus.COLLECTING
    assert view.plan_summary is None
    assert view.selected_count is None


def test_describe_collecting_counts_staged_photos(tmp_path):
    run = new_collecting_run("tg-c1")
    incoming_root = tmp_path / "incoming"
    d = incoming_dir_for(incoming_root, "tg-c1")
    (d / "a.jpg").write_bytes(b"a")
    (d / "b.jpg").write_bytes(b"b")

    view = view_from_run(run, incoming_root=incoming_root)

    assert view.photo_count() == 2
    assert view.describe() == "目前收到 2 张照片，还没告诉我想怎么处理"


def test_from_run_planned_fills_plan_summary_and_describe(tmp_path):
    incoming_root = tmp_path / "incoming"
    run = _planned_run()
    (incoming_dir_for(incoming_root, run.run_id) / "a.jpg").write_bytes(b"a")

    view = view_from_run(run, incoming_root=incoming_root)

    assert view.plan_summary == {
        "provider": "local", "auto_reject": True, "count": 5, "apply_tag": "精选",
    }
    assert view.describe() == (
        "目前收到 1 张照片，方案是：留 5 张，标签叫\"精选\""
    )


def test_from_run_awaiting_gate_restores_selected_count(tmp_path):
    run = _planned_run()
    run.status = RunStatus.AWAITING_GATE
    run.outputs["Curate"] = StageOutput(ok=True, data={"selected": ["a.jpg", "b.jpg"]})

    view = view_from_run(run, incoming_root=tmp_path / "incoming")

    assert view.selected_count == 2
    assert view.describe() == "已经选好了 2 张，等你回复"


def test_describe_running_with_progress_mentions_counts(tmp_path):
    view = SessionView(incoming_root=tmp_path / "incoming", run_id="tg-r1",
                       status=RunStatus.RUNNING, current_stage="Evaluate",
                       stage_progress=(34, 120))

    text = view.describe()

    assert "正在执行 AI 评估" in text
    assert "34/120" in text
    assert "取消" not in text  # 不再提示取消（真机反馈）


def test_describe_running_without_progress_still_names_stage(tmp_path):
    view = SessionView(incoming_root=tmp_path / "incoming", run_id="tg-r1",
                       status=RunStatus.RUNNING, current_stage="Dedup")

    text = view.describe()

    assert "正在执行去重" in text
    assert "/" not in text


def test_describe_empty_view_and_photo_count_without_run(tmp_path):
    view = SessionView(incoming_root=tmp_path / "incoming")

    assert view.photo_count() == 0
    assert view.describe() == "没什么可说的"


def test_drive_jobs_get_independent_cancel_events():
    a = DriveJob(generation=1, action="start", run_id="tg-1")
    b = DriveJob(generation=1, action="start", run_id="tg-1")

    a.cancel_event.set()

    assert not b.cancel_event.is_set()
