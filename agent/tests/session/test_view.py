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


def _planned_run(ai_enabled: bool = False, selection_brief: str = "") -> RunState:
    params = {"count": 5, "apply_tag": "精选", "ai_enabled": ai_enabled}
    if selection_brief:
        params["selection_brief"] = selection_brief
    plan = Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params=params),
        StageSpec(name="Deliver", gate="required"),
    ])
    return RunState(
        run_id="tg-abc", project_id="tg-abc", plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=RunStatus.PLANNED,
    )


def _planned_run_deferred_curate() -> RunState:
    # W2026-07-21 目标三案例二：Curate 待定（count=None, gate="required"）。
    plan = Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": None, "apply_tag": "精选", "ai_enabled": False},
                  gate="required"),
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

    assert view.plan_summary == {"count": 5, "apply_tag": "精选", "ai_enabled": False,
                                 "selection_brief": ""}
    # 没有题材要求时主语是光秃秃的"照片"，跟确认文案同一条规则。
    assert view.describe() == (
        "目前收到 1 张照片，方案是：帮你选择 5 张照片，标签叫\"精选\""
    )


def test_from_run_planned_describe_mentions_ai_when_enabled(tmp_path):
    incoming_root = tmp_path / "incoming"
    run = _planned_run(ai_enabled=True)
    (incoming_dir_for(incoming_root, run.run_id) / "a.jpg").write_bytes(b"a")

    view = view_from_run(run, incoming_root=incoming_root)

    assert view.plan_summary == {"count": 5, "apply_tag": "精选", "ai_enabled": True,
                                 "selection_brief": ""}
    assert "使用AI帮你选择" in view.describe()


def test_from_run_planned_describe_carries_the_selection_brief(tmp_path):
    # 状态查询是用户核对简述的第二个入口：确认那条消息可能已经被后面几十
    # 条进度顶上去了，问一句"现在什么情况"必须还能看到简述原文，否则又回
    # 到"brief 不可见"那个缺陷上（真机反馈 2026-08-02）。
    incoming_root = tmp_path / "incoming"
    run = _planned_run(ai_enabled=True, selection_brief="发朋友圈用，有人有景，人物表情活泼")
    (incoming_dir_for(incoming_root, run.run_id) / "a.jpg").write_bytes(b"a")

    view = view_from_run(run, incoming_root=incoming_root)

    assert view.plan_summary["selection_brief"] == "发朋友圈用，有人有景，人物表情活泼"
    assert view.describe() == (
        "目前收到 1 张照片，方案是：使用AI帮你选择 5 张发朋友圈用，有人有景，"
        "人物表情活泼的照片，标签叫\"精选\""
    )


def test_from_run_awaiting_gate_restores_selected_count(tmp_path):
    run = _planned_run()
    run.status = RunStatus.AWAITING_GATE
    run.outputs["Curate"] = StageOutput(ok=True, data={"selected": ["a.jpg", "b.jpg"]})

    view = view_from_run(run, incoming_root=tmp_path / "incoming")

    assert view.selected_count == 2
    assert view.describe() == "已经选好了 2 张，等你回复"


def test_from_run_planned_deferred_curate_describe_mentions_dedup_first(tmp_path):
    incoming_root = tmp_path / "incoming"
    run = _planned_run_deferred_curate()
    (incoming_dir_for(incoming_root, run.run_id) / "a.jpg").write_bytes(b"a")

    view = view_from_run(run, incoming_root=incoming_root)

    assert view.plan_summary["count"] is None
    text = view.describe()
    assert "先帮你去重" in text
    assert "去重完再问要不要接着筛" not in text
    assert "None" not in text


def test_describe_awaiting_gate_curate_says_dedup_done_not_selected_count(tmp_path):
    run = _planned_run_deferred_curate()
    run.status = RunStatus.AWAITING_GATE

    view = view_from_run(run, incoming_root=tmp_path / "incoming")
    view.gate_stage = "Curate"

    assert view.describe() == "去重完了，等你说要不要再筛选一下"


def test_describe_running_with_progress_mentions_counts(tmp_path):
    view = SessionView(incoming_root=tmp_path / "incoming", run_id="tg-r1",
                       status=RunStatus.RUNNING, current_stage="Curate",
                       stage_progress=(34, 120, "comparisons"))

    text = view.describe()

    assert "34/120次" in text
    assert "取消" not in text  # 不再提示取消（真机反馈）


def test_describe_running_picks_the_wording_by_what_is_being_counted(tmp_path):
    # 真机验收踩到的：Curate 的本地分簇阶段报"正在筛选，已完成 1/1 张"，
    # 用户明明要选 3 张。那个 1/1 是候选簇不是照片，单位和主语双错。整句
    # 由"数的是什么"决定，不由 stage 决定 —— 同一个 Curate 两个阶段数的
    # 东西就不一样。
    def describe(kind):
        return SessionView(incoming_root=tmp_path / "incoming", run_id="tg-r1",
                           status=RunStatus.RUNNING, current_stage="Curate",
                           stage_progress=(1, 1, kind)).describe()

    assert describe("groups") == "正在处理需要比较筛选的照片组，已完成 1/1组"
    assert describe("comparisons") == "正在两两比较、挑出更好的那张，已完成 1/1次"
    assert describe("photos") == "正在套滤镜，已完成 1/1张"
    # 票 09 的第四类。单位跟 photos 一样是"张"，但活动不同 - 复用 photos
    # 会让用户在评估阶段看到"正在套滤镜"。
    assert describe("evaluations") == "正在逐张看照片、记下画面内容与优缺点，已完成 1/1张"


def test_describe_running_falls_back_to_the_stage_wording_for_an_unknown_kind(tmp_path):
    # 未知 kind 不该炸也不该说错话，退回 stage 的通用句子。
    view = SessionView(incoming_root=tmp_path / "incoming", run_id="tg-r1",
                       status=RunStatus.RUNNING, current_stage="Dedup",
                       stage_progress=(2, 5, "something-new"))

    assert view.describe() == "正在执行去重，已完成 2/5张"


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
