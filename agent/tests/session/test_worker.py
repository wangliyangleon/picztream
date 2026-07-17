"""SessionWorker：单队列串行 job 执行器（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第五、六节）。测四类协议：classify/compose job 的事件映
射、DriveJob 的推进与事件序列、取消（loop-top 检查 + 可杀布防 +
PztCancelledError 收尾）、未预期异常的 JobCrashed 兜底。全部单步
step() 驱动，不起真线程。
"""
from __future__ import annotations

import threading

from compose.adjustment_parser import (
    AdjustmentError,
    CollectingReply,
    GateReply,
    PlanConfirmationReply,
)
from compose.llm_client import LlmRequestError
from orchestrator.types import RunStatus, StageStatus
from session.protocol import (
    ClassifyDone,
    ClassifyFailed,
    ClassifyJob,
    ComposeDone,
    ComposeFailed,
    ComposeJob,
    DriveJob,
    GateReached,
    JobCrashed,
    RunFinished,
    StageStarted,
)

from session_fakes import FakeClient, make_worker


def test_step_returns_false_on_empty_queue(tmp_path):
    env = make_worker(tmp_path)
    assert env.step() is False


def test_classify_collecting_emits_done_with_result(tmp_path):
    env = make_worker(tmp_path, classify_collecting_message_fn=lambda text, n: CollectingReply(action="query"))
    env.put_classify(ClassifyJob(generation=3, kind="collecting", text="收到几张了",
                              context={"photo_count": 2}))

    assert env.step() is True

    [event] = env.drain_events()
    assert isinstance(event, ClassifyDone)
    assert event.generation == 3
    assert event.kind == "collecting"
    assert event.result.action == "query"


def test_classify_adjustment_error_maps_to_not_retryable(tmp_path):
    env = make_worker(tmp_path)  # 默认 classify fn 抛 AdjustmentError
    env.put_classify(ClassifyJob(generation=1, kind="collecting", text="???",
                              context={"photo_count": 0}))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ClassifyFailed)
    assert event.retryable is False


def test_classify_llm_error_maps_to_retryable(tmp_path):
    def broken(text, n):
        raise LlmRequestError("network", "connection refused")

    env = make_worker(tmp_path, classify_collecting_message_fn=broken)
    env.put_classify(ClassifyJob(generation=1, kind="collecting", text="筛一下",
                              context={"photo_count": 1}))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ClassifyFailed)
    assert event.retryable is True


def test_classify_gate_reply_loads_run_from_store(tmp_path):
    seen = {}

    def fake_gate_reply(text, run):
        seen["run_id"] = run.run_id
        return GateReply(action="approve")

    env = make_worker(tmp_path, classify_gate_reply_fn=fake_gate_reply)
    run = env.make_running_run("tg-g1")
    env.put_classify(ClassifyJob(generation=1, kind="gate_reply", text="挺好的",
                              context={"run_id": run.run_id}))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ClassifyDone)
    assert event.result.action == "approve"
    assert seen["run_id"] == "tg-g1"


def test_classify_refine_plan_passes_context(tmp_path):
    seen = {}

    def fake_refine(intent_raw, current_params, text):
        seen["args"] = (intent_raw, current_params, text)
        return PlanConfirmationReply(action="approve")

    env = make_worker(tmp_path, refine_plan_confirmation_fn=fake_refine)
    env.put_classify(ClassifyJob(generation=1, kind="refine_plan", text="改成6张",
                              context={"intent_raw": "筛一下", "current_params": {"count": 2}}))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ClassifyDone)
    assert seen["args"] == ("筛一下", {"count": 2}, "改成6张")


def test_compose_success_emits_validated_plan(tmp_path):
    env = make_worker(tmp_path)
    env.put_classify(ComposeJob(generation=2, intent_text="筛一下留2张"))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ComposeDone)
    assert event.generation == 2
    assert [s.name for s in event.plan.stages][0] == "Ingest"


def test_compose_llm_failure_emits_compose_failed(tmp_path):
    def broken(intent, profile, last):
        raise LlmRequestError("bad_response", "not json")

    env = make_worker(tmp_path, compose_plan_fn=broken)
    env.put_classify(ComposeJob(generation=2, intent_text="???"))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ComposeFailed)
    assert "not json" in event.message


def test_drive_start_runs_to_style_gate(tmp_path):
    env = make_worker(tmp_path)
    run = env.make_running_run()
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))

    env.step()

    events = env.drain_events()
    started = [e.stage for e in events if isinstance(e, StageStarted)]
    assert started == ["Ingest", "Evaluate", "Dedup", "Curate", "Style"]
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "Style"
    assert gate.payload == {}
    assert env.store.load(run.run_id).status == RunStatus.AWAITING_GATE


def test_full_gate_walk_style_then_apply_all_then_deliver(tmp_path):
    env = make_worker(tmp_path)
    run = env.make_running_run()
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))
    env.step()
    env.drain_events()

    # Style 闸门收到描述 -> rerun_style -> 停在 StyleApplyAll 预览闸门
    env.put_drive(DriveJob(generation=1, action="rerun_style", run_id=run.run_id,
                           args={"style_description": "复古暖色调"}))
    env.step()
    events = env.drain_events()
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "StyleApplyAll"
    assert gate.payload["chosen_recipe"] == "Havana 1959"
    assert gate.payload["preview_sent"] is True
    assert gate.payload["export_error"] is None
    assert len(env.transport.sent_photos) == 1  # 代表图预览

    # 确认风格 -> resolve_gate 跑 StyleApplyAll -> 停在 Deliver 闸门(带选片预览)
    env.put_drive(DriveJob(generation=1, action="resolve_gate", run_id=run.run_id))
    env.step()
    events = env.drain_events()
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "Deliver"
    assert gate.payload["selected_count"] == 2
    assert gate.payload["applied_recipe"] == "Havana 1959"
    assert gate.payload["preview_failed_count"] == 0
    assert len(env.transport.sent_photos) == 3  # 又发了 2 张选片预览

    # 确认交付 -> Deliver 真正执行(stage 自己发文件) -> AWAITING_REVIEW 自动 approve
    env.put_drive(DriveJob(generation=1, action="resolve_gate", run_id=run.run_id))
    env.step()
    events = env.drain_events()
    finished = events[-1]
    assert isinstance(finished, RunFinished)
    assert finished.status == "done"
    assert len(env.transport.sent_files) == 2
    assert env.store.load(run.run_id).status == RunStatus.DONE


def test_pre_set_cancel_stops_before_any_stage(tmp_path):
    env = make_worker(tmp_path)
    run = env.make_running_run()
    job = DriveJob(generation=1, action="start", run_id=run.run_id)
    job.cancel_event.set()
    env.put_drive(job)

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, RunFinished)
    assert event.status == "cancelled"
    assert env.client.calls == []  # 一个 stage 都没开跑
    assert env.store.load(run.run_id).status == RunStatus.CANCELLED


def test_client_is_armed_only_during_killable_stages(tmp_path):
    env = make_worker(tmp_path)
    run = env.make_running_run()
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))

    env.step()

    assert env.client.armed_during["new"] is False       # Ingest 不可杀
    assert env.client.armed_during["eval"] is True       # Evaluate 可杀
    assert env.client.armed_during["dedup"] is True      # Dedup 可杀
    assert env.client.armed_during["curate"] is False    # Curate 不可杀
    assert env.client.cancel_event is None               # 结束后摘除


def test_cancelled_error_mid_eval_finishes_run_as_cancelled(tmp_path):
    env = make_worker(tmp_path, client=FakeClient(raise_cancelled_on=("eval",)))
    run = env.make_running_run()
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))

    env.step()

    events = env.drain_events()
    finished = events[-1]
    assert isinstance(finished, RunFinished)
    assert finished.status == "cancelled"
    saved = env.store.load(run.run_id)
    assert saved.status == RunStatus.CANCELLED
    assert saved.stage_states["Ingest"] == StageStatus.DONE  # 已完成的不回滚
    assert env.client.cancel_event is None


def test_unexpected_exception_emits_job_crashed(tmp_path):
    def exploding(text, n):
        raise RuntimeError("boom")

    env = make_worker(tmp_path, classify_collecting_message_fn=exploding)
    env.put_classify(ClassifyJob(generation=7, kind="collecting", text="x",
                              context={"photo_count": 0}))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, JobCrashed)
    assert event.generation == 7
    assert "boom" in event.error
