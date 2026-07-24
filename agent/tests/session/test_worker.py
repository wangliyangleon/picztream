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
    DedupFollowupReply,
    GateReply,
    PlanConfirmationReply,
)
from compose.llm_client import LlmRequestError
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus
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

from session_fakes import FakeClient, _fake_style_http_post, make_worker


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


def test_classify_dedup_followup_passes_remaining(tmp_path):
    # 回归钉子：_execute_classify 漏了 "dedup_followup" 分支，落进单 text 的
    # 兜底分支，真机上直接 TypeError 崩掉（W2026-07-21 目标三真机验证发现）。
    seen = {}

    def fake_dedup_followup(text, remaining):
        seen["args"] = (text, remaining)
        return DedupFollowupReply(action="narrow", count=1)

    env = make_worker(tmp_path, classify_dedup_followup_fn=fake_dedup_followup)
    env.put_classify(ClassifyJob(generation=1, kind="dedup_followup", text="留一张吧",
                              context={"remaining": 3}))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, ClassifyDone)
    assert seen["args"] == ("留一张吧", 3)


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
    # Style 停在闸门、这一轮并不运行，不发 StageStarted（AG-05）。
    assert started == ["Ingest", "Dedup", "Curate"]
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "Style"
    # make_fixed_plan 的 Curate.count=2（真正筛选过），阶段一先给选片预览
    # payload（真机反馈：选片确认放在滤镜之前）。
    assert gate.payload == {"selected_count": 2, "preview_failed_count": 0, "export_error": None}
    assert env.store.load(run.run_id).status == RunStatus.AWAITING_GATE


def _started_stages(events):
    return [e.stage for e in events if isinstance(e, StageStarted)]


def test_full_gate_walk_style_then_apply_all_then_deliver(tmp_path):
    # 真机反馈：选片确认挪到 Style 闸门的阶段一（滤镜之前），Deliver 不
    # 再挂闸门二次预览全部选片，StyleApplyAll 批准后直接交付到底。
    env = make_worker(tmp_path)
    run = env.make_running_run()
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))
    env.step()
    events = env.drain_events()
    # 跑到 Style 闸门停下：只发真运行过的 stage，不发被闸门挡住的 Style（AG-05）。
    started = _started_stages(events)
    assert started == ["Ingest", "Dedup", "Curate"]
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "Style"
    # make_fixed_plan 的 Curate.count=2（真正筛选过），阶段一先给选片预览。
    assert gate.payload == {"selected_count": 2, "preview_failed_count": 0, "export_error": None}
    assert len(env.transport.sent_photos) == 2  # 选片预览
    # 选片预览逐张带"第 N 张"编号（AG-15）。
    assert env.transport.sent_photo_captions == ["第 1 张", "第 2 张"]

    # Style 闸门收到描述 -> rerun_style -> 停在 StyleApplyAll 预览闸门
    env.put_drive(DriveJob(generation=1, action="rerun_style", run_id=run.run_id,
                           args={"style_description": "复古暖色调"}))
    env.step()
    events = env.drain_events()
    # rerun_style 真正跑 Style，发 StageStarted(Style)；StyleApplyAll 停闸门不发。
    assert _started_stages(events) == ["Style"]
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "StyleApplyAll"
    assert gate.payload["chosen_recipe"] == "Havana 1959"
    assert gate.payload["preview_sent"] is True
    assert gate.payload["export_error"] is None
    assert len(env.transport.sent_photos) == 3  # +1 代表图预览

    # 确认风格 -> resolve_gate 跑 StyleApplyAll -> Deliver 不挂闸门，同一次
    # step() 里直接跑到底、真正交付。
    env.put_drive(DriveJob(generation=1, action="resolve_gate", run_id=run.run_id))
    env.step()
    events = env.drain_events()
    assert _started_stages(events) == ["StyleApplyAll", "Deliver"]
    finished = events[-1]
    assert isinstance(finished, RunFinished)
    assert finished.status == "done"
    assert len(env.transport.sent_files) == 2
    assert env.store.load(run.run_id).status == RunStatus.DONE


def test_preview_send_total_failure_emits_ordered_placeholder(tmp_path):
    # AG-15：某张图和文件都发不出去时，发"第 N 张预览发送失败"文本占位保序，
    # 并计入 failed。
    env = make_worker(tmp_path)
    run = env.make_running_run()

    def boom(*a, **k):
        raise RuntimeError("too big")
    env.worker.transport.send_photo = boom
    env.worker.transport.send_file = boom

    preview_dir = tmp_path / "preview" / run.run_id
    preview_dir.mkdir(parents=True)
    for name in ("a.jpg", "b.jpg"):
        (preview_dir / name).write_bytes(b"x")

    failed = env.worker._send_preview_media(run, ["a.jpg", "b.jpg"], numbered=True)

    assert failed == 2
    texts = env.transport.texts()
    assert "第 1 张预览发送失败" in texts
    assert "第 2 张预览发送失败" in texts


def test_deliver_export_failure_fails_run(tmp_path):
    # AG-06：交付 export-images 失败 = run FAILED（而非旧的 optional 吞成
    # SKIPPED -> DONE -> 误报"这批就处理完啦"）。Style/StyleApplyAll 的闸门
    # 预览也调 export-images，但预览失败只降级成 payload 里的 export_error
    # （不挡路，见 _prepare_gate_payload）；真正致命的是 Deliver stage 自
    # 己 run() 里的 export-images 调用。Deliver 不挂闸门，批准 StyleApplyAll
    # 后同一个 resolve_gate 就直接跑到 Deliver 失败。
    env = make_worker(tmp_path, client=FakeClient(raise_command_on=("export-images",)))
    run = env.make_running_run()
    for action, args in [
        ("start", {}),
        ("rerun_style", {"style_description": "复古暖色调"}),
        ("resolve_gate", {}),  # 放行 StyleApplyAll -> Deliver 直接跑, export 失败
    ]:
        env.put_drive(DriveJob(generation=1, action=action, run_id=run.run_id, args=args))
        env.step()
        events = env.drain_events()

    finished = events[-1]
    assert isinstance(finished, RunFinished)
    assert finished.status == "failed"
    assert "Deliver" in (finished.detail or "")
    saved = env.store.load(run.run_id)
    assert saved.status == RunStatus.FAILED
    assert saved.stage_states["Deliver"] == StageStatus.FAILED


def test_rerun_style_match_failure_reprompts_style_gate(tmp_path):
    # AG-01：描述匹配不上任何 preset（Style 软失败 match_failed）-> 退回 Style
    # 闸门重新问，不报废整批、不往下推进。
    env = make_worker(tmp_path)
    run = env.make_running_run()
    env.worker.driver.stages["Style"].http_post = _fake_style_http_post("Not A Real Preset")
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))
    env.step()
    env.drain_events()

    env.put_drive(DriveJob(generation=1, action="rerun_style", run_id=run.run_id,
                           args={"style_description": "匹配不上的乱描述"}))
    env.step()
    events = env.drain_events()

    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "Style"
    assert gate.payload.get("match_failed") is True
    saved = env.store.load(run.run_id)
    assert saved.status == RunStatus.AWAITING_GATE
    assert saved.gate_state.stage_name == "Style"


def test_rerun_style_skip_empty_description_runs_no_style(tmp_path):
    # AG-16.1：原图直出（空描述）-> Style 空跑 chosen_recipe None -> 越过 Style
    # 停在 StyleApplyAll 闸门，payload chosen_recipe None（consumer 会自动推进）。
    env = make_worker(tmp_path)
    run = env.make_running_run()
    env.put_drive(DriveJob(generation=1, action="start", run_id=run.run_id))
    env.step()
    env.drain_events()

    env.put_drive(DriveJob(generation=1, action="rerun_style", run_id=run.run_id,
                           args={"style_description": ""}))
    env.step()
    events = env.drain_events()

    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "StyleApplyAll"
    assert gate.payload.get("chosen_recipe") is None
    assert env.store.load(run.run_id).stage_states["Style"] == StageStatus.DONE


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
    assert env.client.armed_during["dedup"] is True      # Dedup 可杀
    assert env.client.armed_during["curate"] is True     # Curate 可杀(AI 开时耗时不再恒定)
    assert env.client.cancel_event is None               # 结束后摘除


def test_cancelled_error_mid_dedup_finishes_run_as_cancelled(tmp_path):
    env = make_worker(tmp_path, client=FakeClient(raise_cancelled_on=("dedup",)))
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


def test_prepare_gate_payload_curate_computes_remaining(tmp_path):
    # W2026-07-21 目标三决策四：remaining = Ingest.image_count - Dedup.tagged。
    env = make_worker(tmp_path)
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="Curate", gate="required")])
    run = RunState(
        run_id="r1", project_id="r1", plan=plan,
        stage_states={"Ingest": StageStatus.DONE, "Curate": StageStatus.PENDING},
        outputs={
            "Ingest": StageOutput(ok=True, data={"image_count": 10}),
            "Dedup": StageOutput(ok=True, data={"groups": 3, "tagged": 4, "skipped_no_capture_time": 0}),
        },
    )

    payload = env.worker._prepare_gate_payload(run, "Curate")

    assert payload == {"remaining": 6, "ai_enabled": False}


def test_prepare_gate_payload_curate_surfaces_ai_enabled(tmp_path):
    env = make_worker(tmp_path)
    plan = Plan(stages=[StageSpec(name="Ingest"),
                        StageSpec(name="Curate", gate="required", params={"ai_enabled": True})])
    run = RunState(
        run_id="r1", project_id="r1", plan=plan,
        stage_states={"Ingest": StageStatus.DONE, "Curate": StageStatus.PENDING},
        outputs={
            "Ingest": StageOutput(ok=True, data={"image_count": 10}),
            "Dedup": StageOutput(ok=True, data={"groups": 3, "tagged": 4, "skipped_no_capture_time": 0}),
        },
    )

    payload = env.worker._prepare_gate_payload(run, "Curate")

    assert payload == {"remaining": 6, "ai_enabled": True}


def test_prepare_gate_payload_style_empty_when_curate_passthrough(tmp_path):
    # 真机反馈：passthrough（count=None，没有真正筛选）时 Style 闸门不用
    # 展示选片结果，直接问风格——payload 应该是空的，不触发预览导出。
    env = make_worker(tmp_path)
    plan = Plan(stages=[StageSpec(name="Ingest"),
                        StageSpec(name="Curate", params={"count": None}),
                        StageSpec(name="Style", gate="required")])
    run = RunState(
        run_id="r1", project_id="r1", plan=plan,
        stage_states={"Ingest": StageStatus.DONE, "Curate": StageStatus.DONE,
                      "Style": StageStatus.PENDING},
        outputs={"Curate": StageOutput(ok=True, data={"selected": ["a.jpg", "b.jpg"]})},
    )

    payload = env.worker._prepare_gate_payload(run, "Style")

    assert payload == {}


def test_prepare_gate_payload_style_shows_selection_when_curate_narrowed(tmp_path):
    env = make_worker(tmp_path)
    plan = Plan(stages=[StageSpec(name="Ingest"),
                        StageSpec(name="Curate", params={"count": 2}),
                        StageSpec(name="Style", gate="required")])
    run = RunState(
        run_id="r1", project_id="r1", plan=plan,
        stage_states={"Ingest": StageStatus.DONE, "Curate": StageStatus.DONE,
                      "Style": StageStatus.PENDING},
        outputs={"Curate": StageOutput(ok=True, data={"selected": ["a.jpg", "b.jpg"]})},
    )

    payload = env.worker._prepare_gate_payload(run, "Style")

    assert payload["selected_count"] == 2
    assert payload["export_error"] is None


def _mark_ingest_dedup_done(run, image_count=2, tagged=0):
    run.stage_states["Ingest"] = StageStatus.DONE
    run.stage_states["Dedup"] = StageStatus.DONE
    run.outputs["Ingest"] = StageOutput(ok=True, data={"image_count": image_count})
    run.outputs["Dedup"] = StageOutput(ok=True, data={
        "groups": 0, "tagged": tagged, "skipped_no_capture_time": 0,
    })


def test_drive_rerun_curate_runs_passthrough_and_continues_to_next_gate(tmp_path):
    # W2026-07-21 目标三决策四：追问回复用 rerun_curate 直接跑 Curate，
    # 不重新触发它自己的闸门，continue 到下一个闸门（Style）。
    env = make_worker(tmp_path)
    run = env.make_running_run()
    _mark_ingest_dedup_done(run)
    env.store.save(run)
    env.put_drive(DriveJob(generation=1, action="rerun_curate", run_id=run.run_id,
                           args={"params": {"count": None}}))

    env.step()

    events = env.drain_events()
    assert _started_stages(events) == ["Curate"]
    # count=None -> passthrough：走 pzt images，不是 pzt curate（目标三决策三）。
    assert any(c[0] == "images" for c in env.client.calls)
    assert not any(c[0] == "curate" for c in env.client.calls)
    gate = events[-1]
    assert isinstance(gate, GateReached)
    assert gate.stage == "Style"


def test_drive_rerun_curate_with_count_calls_pzt_curate(tmp_path):
    env = make_worker(tmp_path)
    run = env.make_running_run()
    _mark_ingest_dedup_done(run)
    env.store.save(run)
    env.put_drive(DriveJob(generation=1, action="rerun_curate", run_id=run.run_id,
                           args={"params": {"count": 2, "apply_tag": "精选"}}))

    env.step()

    assert any(c[0] == "curate" for c in env.client.calls)
    assert not any(c[0] == "images" for c in env.client.calls)


def test_rerun_curate_cancelled_finishes_run_as_cancelled(tmp_path):
    env = make_worker(tmp_path, client=FakeClient(raise_cancelled_on=("images",)))
    run = env.make_running_run()
    _mark_ingest_dedup_done(run)
    env.store.save(run)
    env.put_drive(DriveJob(generation=1, action="rerun_curate", run_id=run.run_id,
                           args={"params": {"count": None}}))

    env.step()

    events = env.drain_events()
    finished = events[-1]
    assert isinstance(finished, RunFinished)
    assert finished.status == "cancelled"
    assert env.store.load(run.run_id).status == RunStatus.CANCELLED
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
    assert event.lane == "classify"
    assert "boom" in event.error


def test_drive_crash_emits_job_crashed_with_drive_lane(tmp_path):
    from session.protocol import DriveJob

    env = make_worker(tmp_path)
    run = env.make_running_run()
    # 未知 action 触发 _execute_drive 的 ValueError -> 未预期异常兜底。
    env.put_drive(DriveJob(generation=3, action="bogus", run_id=run.run_id))

    env.step()

    [event] = env.drain_events()
    assert isinstance(event, JobCrashed)
    assert event.generation == 3
    assert event.lane == "drive"


def test_export_previews_clears_stale_files_before_reexport(tmp_path):
    # 换滤镜后重新导出预览：必须先清掉上一次的 name.jpg，否则 export-images
    # 会消歧成 name_2.jpg，而发送端永远发 name.jpg -> 发的还是旧滤镜预览。
    env = make_worker(tmp_path)
    run = env.make_running_run()
    preview_dir = tmp_path / "preview" / run.run_id
    preview_dir.mkdir(parents=True)
    stale = preview_dir / "a.jpg"
    stale.write_bytes(b"old-recipe-preview")

    err = env.worker._export_previews(run, ["a.jpg"])

    assert err is None
    assert not stale.exists()  # 旧预览已清，不会把旧滤镜图又发一遍
