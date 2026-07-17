"""SessionConsumer 消息流：入站分派、文本串行、关键词快路径、事件应用、
闸门文案渲染（docs/W2026-07-15_AgentRuntime_Eng_Design.md 第七、八节，
文案逐字对齐旧 router）。全部单步 step() 驱动，jobs/events 队列手工搬运，
不接真 worker。
"""
from __future__ import annotations

from compose.adjustment_parser import CollectingReply, GateReply, PlanConfirmationReply
from orchestrator.types import PlanDelta, RunStatus
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

from session_fakes import (
    bare_compose_plan,
    make_consumer,
    to_planned,
    to_running,
    worker_saves_gate,
)


def test_first_photo_mints_collecting_run_without_reply(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")

    env.consumer.step()

    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING
    assert (tmp_path / "incoming" / run.run_id / "a.jpg").exists()
    assert env.transport.texts() == []  # 逐张不回复


def test_intent_text_chains_classify_fallback_then_compose_to_planned(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.push_text("筛一下留2张")
    env.consumer.step()

    [job] = env.drain_jobs()
    assert isinstance(job, ClassifyJob)
    assert job.kind == "collecting"
    assert job.context == {"photo_count": 1}

    # 分类基础设施失败 -> 旧降级路径：直接当意图去 compose
    env.put_event(ClassifyFailed(0, "collecting", retryable=True))
    env.consumer.step()
    [job] = env.drain_jobs()
    assert isinstance(job, ComposeJob)
    assert job.intent_text == "筛一下留2张"

    env.put_event(ComposeDone(0, bare_compose_plan()))
    env.consumer.step()

    run = env.consumer.run
    assert run.status == RunStatus.PLANNED
    assert run.intent_raw == "筛一下留2张"
    ingest = next(s for s in run.plan.stages if s.name == "Ingest")
    deliver = next(s for s in run.plan.stages if s.name == "Deliver")
    assert ingest.params["folder"] == str(tmp_path / "incoming" / run.run_id)
    assert deliver.params["out_folder"] == str(tmp_path / "deliver-out")
    assert deliver.gate == "required"
    assert "理解你想：留 2 张，标签叫\"精选\"，自动剔除不合格照片，对吗？" in \
        env.transport.texts()[-1]


def test_second_text_waits_until_inflight_classify_resolves(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.push_text("收到几张了")
    env.consumer.step()
    assert len(env.drain_jobs()) == 1

    env.push_text("筛一下")
    env.consumer.step()
    assert env.drain_jobs() == []  # 串行：上一条没回来不处理下一条

    env.put_event(ClassifyDone(0, "collecting", CollectingReply(action="query")))
    env.consumer.step()

    assert "目前收到 1 张照片，还没告诉我想怎么处理" in env.transport.texts()
    [job] = env.drain_jobs()  # 第二条这才被处理
    assert isinstance(job, ClassifyJob)
    assert job.text == "筛一下"


def test_planned_approve_keyword_starts_drive(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)

    env.push_text("好的")
    env.consumer.step()

    assert "开始处理了，共 1 张" in env.transport.texts()
    [job] = env.drain_jobs()
    assert isinstance(job, DriveJob)
    assert job.action == "start"
    assert env.store.load(run.run_id).status == RunStatus.RUNNING
    assert env.consumer.view.drive_active is True
    assert env.consumer.run is None  # 所有权已交给 worker


def test_planned_refine_confirmed_updates_params_and_reconfirms(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)
    env.push_text("改成6张，标签叫ins")
    env.consumer.step()
    [job] = env.drain_jobs()
    assert job.kind == "refine_plan"
    assert job.context["intent_raw"] == "筛一下留2张"
    assert job.context["current_params"]["count"] == 2

    env.put_event(ClassifyDone(0, "refine_plan", PlanConfirmationReply(
        action="confirmed", provider="gemini", auto_reject=False, count=6, apply_tag="ins")))
    env.consumer.step()

    saved = env.store.load(run.run_id)
    assert saved.status == RunStatus.PLANNED  # 改完必须再确认，不自动开跑
    curate = next(s for s in saved.plan.stages if s.name == "Curate")
    evaluate = next(s for s in saved.plan.stages if s.name == "Evaluate")
    assert curate.params["count"] == 6
    assert curate.params["apply_tag"] == "ins"
    assert evaluate.params["auto_reject"] is False
    assert "留 6 张" in env.transport.texts()[-1]
    assert "不自动剔除照片" in env.transport.texts()[-1]
    assert env.drain_jobs() == []


def test_cancel_without_active_run_replies_gently(tmp_path):
    env = make_consumer(tmp_path)
    env.push_text("取消")

    env.consumer.step()

    assert env.transport.texts() == ["现在没有在处理的批次"]
    assert env.store.list_active() == []


def test_cancel_in_collecting_prompts_confirmation_then_cancels(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    [run] = env.store.list_active()

    env.push_text("取消")
    env.consumer.step()

    # 二次确认：还没真取消
    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert env.transport.button_tokens() == ["confirm_cancel", "keep"]
    assert env.store.load(run.run_id).status == RunStatus.COLLECTING

    env.push_text("确认取消")
    env.consumer.step()

    assert env.store.load(run.run_id).status == RunStatus.CANCELLED
    assert "已取消" in env.transport.texts()
    assert env.consumer.view.run_id is None
    assert env.consumer.generation == 1


def test_cancel_confirmation_declined_keeps_run(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    [run] = env.store.list_active()

    env.push_text("取消")
    env.consumer.step()
    env.push_text("不取消")
    env.consumer.step()

    assert "好，继续" in env.transport.texts()
    assert env.store.load(run.run_id).status == RunStatus.COLLECTING
    assert env.consumer.run is not None


def test_cancel_confirmation_dismissed_by_unrelated_text(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()

    env.push_text("取消")
    env.consumer.step()
    # 没明确确认，改发了别的意图：撤掉待确认，当普通意图处理（不取消）
    env.push_text("筛一下留2张")
    env.consumer.step()

    assert env.consumer._cancel_confirm_pending is False
    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING
    [job] = env.drain_jobs()
    assert isinstance(job, ClassifyJob)  # 当成了新意图


def test_photo_during_drive_queues_to_pending(tmp_path):
    env = make_consumer(tmp_path)
    to_running(env)

    env.push_photo("extra.jpg", b"z")
    env.consumer.step()

    assert (tmp_path / "incoming" / "_pending" / "extra.jpg").read_bytes() == b"z"
    assert "先帮你收着，这批处理完就接着看这些新照片" in env.transport.texts()


def test_text_during_drive_gets_template_reply_without_llm(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    env.put_event(StageStarted(0, job.run_id, "Evaluate"))
    env.consumer.step()
    assert "正在执行 AI 评估..." in env.transport.texts()

    env.push_text("到哪了")
    env.consumer.step()

    assert env.drain_jobs() == []  # 不投任何 LLM job
    assert "说\"取消\"可以停" in env.transport.texts()[-1]


def test_stage_started_renders_text_only_for_message_stages(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    before = len(env.transport.texts())

    env.put_event(StageStarted(0, job.run_id, "Style"))
    env.consumer.step()

    assert len(env.transport.texts()) == before  # Style 故意不发进度文案
    assert env.consumer.view.current_stage == "Style"


def test_cancel_during_drive_confirms_then_sets_event(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)

    env.push_text("取消")
    env.consumer.step()
    # drive 中也先弹二次确认，不立即掐
    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert not job.cancel_event.is_set()

    env.push_callback(f"confirm_cancel:{job.run_id}")
    env.consumer.step()

    assert "正在停下来..." in env.transport.texts()
    assert job.cancel_event.is_set()
    assert env.consumer.generation == 1
    assert env.consumer.view.run_id is None

    env.put_event(RunFinished(0, job.run_id, "cancelled", None))  # 旧代事件
    env.consumer.step()

    assert env.transport.texts()[-1] == "已取消"


def test_stale_generation_events_are_dropped_after_cancel(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    env.push_text("取消")
    env.consumer.step()
    env.push_callback(f"confirm_cancel:{job.run_id}")
    env.consumer.step()
    before = len(env.transport.texts())

    env.put_event(StageStarted(0, job.run_id, "Evaluate"))
    env.consumer.step()

    assert len(env.transport.texts()) == before
    assert env.consumer.view.current_stage is None


def test_gate_style_prompts_then_rerun_with_description(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")

    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    assert "想要什么风格？用一句话描述就行，比如\"复古暖色调\"" in env.transport.texts()
    assert env.consumer.view.status == RunStatus.AWAITING_GATE
    assert env.consumer.run is not None  # 所有权交回

    env.push_text("复古暖色调")
    env.consumer.step()

    assert "正在选风格..." in env.transport.texts()
    [next_job] = env.drain_jobs()
    assert next_job.action == "rerun_style"
    assert next_job.args == {"style_description": "复古暖色调"}


def test_gate_style_apply_all_renders_preview_and_approve_resolves(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "StyleApplyAll")

    env.put_event(GateReached(0, job.run_id, "StyleApplyAll", {
        "chosen_recipe": "Havana 1959", "preview_sent": True, "export_error": None}))
    env.consumer.step()

    assert ("这是用「Havana 1959」套用的效果，满意点\"满意\"，"
            "想换风格点\"重选\"或直接打字描述，想取消打字说\"取消\"") in env.transport.texts()
    assert env.transport.button_tokens() == ["approve", "restyle"]

    env.push_text("好的")
    env.consumer.step()

    [next_job] = env.drain_jobs()
    assert next_job.action == "resolve_gate"


def test_gate_style_apply_all_without_chosen_recipe_asks_for_description(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "StyleApplyAll")

    env.put_event(GateReached(0, job.run_id, "StyleApplyAll", {"chosen_recipe": None}))
    env.consumer.step()

    assert "没能选出风格，直接说说想要什么风格吧" in env.transport.texts()


def test_gate_deliver_summary_then_llm_adjustment(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Deliver")

    env.put_event(GateReached(0, job.run_id, "Deliver", {
        "selected_count": 2, "applied_recipe": "Havana 1959",
        "preview_failed_count": 0, "export_error": None}))
    env.consumer.step()

    assert ("选好了 2 张，已套用风格「Havana 1959」，"
            "满意点\"满意\"，想调整点\"重选\"或直接打字说，想取消打字说\"取消\"") in env.transport.texts()
    assert env.transport.button_tokens() == ["approve", "restyle"]

    env.push_text("换掉第1张")
    env.consumer.step()
    [classify_job] = env.drain_jobs()
    assert classify_job.kind == "gate_reply"
    assert classify_job.context == {"run_id": job.run_id}

    delta = PlanDelta(stage_name="Curate", params={"exclude": ["a.jpg"]})
    env.put_event(ClassifyDone(0, "gate_reply", GateReply(action="adjust", delta=delta)))
    env.consumer.step()

    [drive_job] = env.drain_jobs()
    assert drive_job.action == "adjustment"
    assert drive_job.args["delta"].params == {"exclude": ["a.jpg"]}


def test_gate_classify_not_understood_replies_guidance(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Deliver")
    env.put_event(GateReached(0, job.run_id, "Deliver", {
        "selected_count": 2, "applied_recipe": None,
        "preview_failed_count": 0, "export_error": None}))
    env.consumer.step()

    env.push_text("呃呃呃")
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ClassifyFailed(0, "gate_reply", retryable=False))
    env.consumer.step()

    assert ("没听懂这句话，能再说清楚点吗？满意就说\"好的\"，"
            "不满意说说想怎么调，不要了就说\"取消\"") in env.transport.texts()


def test_compose_failed_keeps_collecting_and_allows_retry(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.push_text("火星话")
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ClassifyFailed(0, "collecting", retryable=False))
    env.consumer.step()
    env.drain_jobs()

    env.put_event(ComposeFailed(0, "unparseable"))
    env.consumer.step()

    assert "没看懂这句意图，能换个说法再说一次吗？（unparseable）" in env.transport.texts()
    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING

    env.push_text("筛一下")
    env.consumer.step()
    [job] = env.drain_jobs()
    assert isinstance(job, ClassifyJob)  # 队列没被卡死，能重试


def test_run_finished_failed_reports_detail_and_clears(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)

    env.put_event(RunFinished(0, job.run_id, "failed", "Evaluate：eval_failed: boom"))
    env.consumer.step()

    assert "处理失败：Evaluate：eval_failed: boom" in env.transport.texts()
    assert env.consumer.view.run_id is None


def test_job_crash_marks_idle_and_next_message_resumes(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)

    env.put_event(JobCrashed(0, "RuntimeError('boom')"))
    env.consumer.step()
    assert env.consumer.view.drive_active is False
    assert env.consumer.view.status == RunStatus.RUNNING  # run 停在检查点
    assert "处理过程中出了点问题，这批先停在这儿了，回句话我接着试" in env.transport.texts()

    env.push_text("继续吧")
    env.consumer.step()

    jobs = env.drain_jobs()
    assert [j.action for j in jobs if isinstance(j, DriveJob)] == ["resume"]
    assert jobs[0].run_id == job.run_id
