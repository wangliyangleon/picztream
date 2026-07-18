"""SessionConsumer 消息流：入站分派、文本串行、关键词快路径、事件应用、
闸门文案渲染（docs/W2026-07-15_AgentRuntime_Eng_Design.md 第七、八节，
文案逐字对齐旧 router）。全部单步 step() 驱动，jobs/events 队列手工搬运，
不接真 worker。
"""
from __future__ import annotations

from compose.adjustment_parser import (
    CancelConfirmReply,
    CollectingReply,
    GateReply,
    PlanConfirmationReply,
    RunningReply,
    StyleDescribeReply,
    StyleGateReply,
)
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
    deliver_classify,
    make_consumer,
    to_planned,
    to_running,
    worker_saves_gate,
)


def test_first_photo_mints_collecting_run_with_single_ack(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")

    env.consumer.step()

    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING
    assert (tmp_path / "incoming" / run.run_id / "a.jpg").exists()
    # 一批开始回一句确认（真机反馈：初始那波图静默体验差）
    assert env.transport.texts() == ["收到～新任务开始了！照片尽管发，发完告诉我想怎么处理就行，比如\"选3张发朋友圈\""]

    # 后续照片仍逐张不回复、不刷屏
    env.push_photo("b.jpg")
    env.consumer.step()
    assert len(env.transport.texts()) == 1


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
    assert "理解你想：留 2 张，标签叫\"精选\"，自动剔除不合格和重复的照片，对吗？" in \
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


def test_planned_approve_via_refine_llm_starts_drive(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)

    env.push_text("好的就这样")
    env.consumer.step()
    [job] = env.drain_jobs()
    assert job.kind == "refine_plan"  # 不再关键词短路，走 LLM
    env.put_event(ClassifyDone(0, "refine_plan", PlanConfirmationReply(action="approve")))
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
    assert "只去重复、保留不合格的照片" in env.transport.texts()[-1]
    assert env.drain_jobs() == []


def test_cancel_without_active_run_replies_gently(tmp_path):
    env = make_consumer(tmp_path)
    env.push_text("取消")
    env.consumer.step()
    # 无 run：不 mint，交 collecting 分类；判成 cancel -> 无批次可取消
    [job] = env.drain_jobs()
    assert job.kind == "collecting"
    env.put_event(ClassifyDone(0, "collecting", CollectingReply(action="cancel")))
    env.consumer.step()

    assert env.transport.texts() == ["现在没有在处理的批次"]
    assert env.store.list_active() == []


def test_cancel_in_collecting_prompts_confirmation_then_cancels(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    [run] = env.store.list_active()

    env.push_text("别弄了")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="cancel"))

    # 二次确认：还没真取消
    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert env.transport.button_tokens() == ["confirm_cancel", "keep"]
    assert env.store.load(run.run_id).status == RunStatus.COLLECTING

    env.push_text("对，取消吧")
    env.consumer.step()
    deliver_classify(env, "cancel_confirm", CancelConfirmReply(action="confirm"))

    assert env.store.load(run.run_id).status == RunStatus.CANCELLED
    assert "已取消" in env.transport.texts()
    assert env.consumer.view.run_id is None
    assert env.consumer.generation == 1


def test_cancel_confirmation_declined_keeps_run(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    [run] = env.store.list_active()

    env.push_text("别弄了")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="cancel"))
    env.push_text("算了还是不取消")
    env.consumer.step()
    deliver_classify(env, "cancel_confirm", CancelConfirmReply(action="deny"))

    assert "好，继续" in env.transport.texts()
    assert env.store.load(run.run_id).status == RunStatus.COLLECTING
    assert env.consumer.run is not None


def test_cancel_confirmation_dismissed_by_unrelated_text(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()

    env.push_text("别弄了")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="cancel"))
    # cancel_confirm 判成 other：撤掉待确认，把这条重新当普通消息处理（不取消）
    env.push_text("筛一下留2张")
    env.consumer.step()
    deliver_classify(env, "cancel_confirm", CancelConfirmReply(action="other"))

    assert env.consumer._cancel_confirm_pending is False
    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING
    # 重新入队后被当作 collecting 意图分类
    [job] = env.drain_jobs()
    assert isinstance(job, ClassifyJob)
    assert job.kind == "collecting"


def test_photo_during_drive_queues_to_pending(tmp_path):
    env = make_consumer(tmp_path)
    to_running(env)

    env.push_photo("extra.jpg", b"z")
    env.consumer.step()

    assert (tmp_path / "incoming" / "_pending" / "extra.jpg").read_bytes() == b"z"
    assert "先帮你收着，这批处理完就接着看这些新照片" in env.transport.texts()


def test_text_during_drive_goes_to_running_classifier(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    env.put_event(StageStarted(0, job.run_id, "Evaluate"))
    env.consumer.step()
    assert "正在执行 AI 评估..." in env.transport.texts()

    env.push_text("到哪了")
    env.consumer.step()
    [j] = env.drain_jobs()
    assert j.kind == "running"  # 处理中也走 LLM（classify lane 并发）
    env.put_event(ClassifyDone(0, "running", RunningReply(action="query")))
    env.consumer.step()

    assert "正在执行 AI 评估" in env.transport.texts()[-1]  # query -> 回进度


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

    env.push_text("停一下别弄了")
    env.consumer.step()
    deliver_classify(env, "running", RunningReply(action="cancel"))
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
    env.push_text("停一下别弄了")
    env.consumer.step()
    deliver_classify(env, "running", RunningReply(action="cancel"))
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

    # 文本现在过 style_describe 分类器（AG-02）：describe -> rerun_style。
    env.push_text("复古暖色调")
    env.consumer.step()
    [classify_job] = env.drain_jobs()
    assert classify_job.kind == "style_describe"
    env.put_event(ClassifyDone(0, "style_describe", StyleDescribeReply(action="describe")))
    env.consumer.step()

    assert "正在选风格..." in env.transport.texts()
    [next_job] = env.drain_jobs()
    assert next_job.action == "rerun_style"
    assert next_job.args == {"style_description": "复古暖色调"}


def test_gate_style_reprompt_skipped_when_description_already_answered(tmp_path):
    # AG-04：Deliver 闸门调整选片会连带把 Style 重置回闸门，但风格早已答过
    # （style_description 幸存在 spec.params）：不再重问，按上次的自动重套。
    env = make_consumer(tmp_path)
    job = to_running(env)
    run = env.store.load(job.run_id)
    next(s for s in run.plan.stages if s.name == "Style").params["style_description"] = "复古暖色调"
    env.store.save(run)
    worker_saves_gate(env, job.run_id, "Style")

    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    texts = env.transport.texts()
    assert not any("想要什么风格" in t for t in texts)
    assert any("还按「复古暖色调」重新套用" in t for t in texts)
    [next_job] = env.drain_jobs()
    assert next_job.action == "rerun_style"
    assert next_job.args == {"style_description": "复古暖色调"}


def test_gate_style_reprompt_skipped_keeps_no_filter_when_previously_skipped(tmp_path):
    # AG-04 skip 变体：上次选了原图直出（style_description==""），调整选片后
    # 仍不重问、不套滤镜。
    env = make_consumer(tmp_path)
    job = to_running(env)
    run = env.store.load(job.run_id)
    next(s for s in run.plan.stages if s.name == "Style").params["style_description"] = ""
    env.store.save(run)
    worker_saves_gate(env, job.run_id, "Style")

    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    assert not any("想要什么风格" in t for t in env.transport.texts())
    assert any("不套滤镜" in t for t in env.transport.texts())
    [next_job] = env.drain_jobs()
    assert next_job.action == "rerun_style"
    assert next_job.args == {"style_description": ""}


def test_gate_style_describe_skip_runs_original_no_filter(tmp_path):
    # AG-16.1：说"原图就行" -> skip -> rerun_style 空描述（chosen_recipe None 空跑）。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    env.push_text("原图就行不用滤镜")
    env.consumer.step()
    deliver_classify(env, "style_describe", StyleDescribeReply(action="skip"))

    assert any("不套滤镜" in t for t in env.transport.texts())
    [next_job] = env.drain_jobs()
    assert next_job.action == "rerun_style"
    assert next_job.args == {"style_description": ""}


def test_gate_style_describe_cancel_prompts_confirmation(tmp_path):
    # AG-02：说"算了不弄了" -> cancel -> 二次确认，而不是被当风格描述。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    env.push_text("算了不弄了")
    env.consumer.step()
    deliver_classify(env, "style_describe", StyleDescribeReply(action="cancel"))

    assert env.consumer._cancel_confirm_pending is True
    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert not env.drain_jobs()  # 没有 rerun_style


def test_gate_style_describe_query_lists_presets(tmp_path):
    # AG-16.4 基础版：说"有哪些风格" -> query -> 列出 9 个 preset。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    env.push_text("有哪些风格可以选")
    env.consumer.step()
    deliver_classify(env, "style_describe", StyleDescribeReply(action="query"))

    texts = " ".join(env.transport.texts())
    assert "Havana 1959" in texts and "Berlin 1989" in texts
    assert not env.drain_jobs()


def test_gate_style_match_failed_reprompts_without_failing(tmp_path):
    # AG-01：GateReached(Style, match_failed) -> 原地重问，不报"处理失败"。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")

    env.put_event(GateReached(0, job.run_id, "Style", {"match_failed": True}))
    env.consumer.step()

    assert any("没能选出对应的风格" in t for t in env.transport.texts())
    assert env.consumer.view.status == RunStatus.AWAITING_GATE  # 没报废


def test_gate_style_describe_classify_failure_falls_back_to_description(tmp_path):
    # 分类失败降级为当描述，不卡用户。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    env.push_text("暖一点的")
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ClassifyFailed(0, "style_describe", retryable=False))
    env.consumer.step()

    [next_job] = env.drain_jobs()
    assert next_job.action == "rerun_style"
    assert next_job.args == {"style_description": "暖一点的"}


def test_style_describe_infra_failure_says_service_down(tmp_path):
    # AG-10：Ollama 挂掉(retryable)时回"AI 连不上", 不误当描述去 rerun_style。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    env.push_text("暖一点的")
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ClassifyFailed(0, "style_describe", retryable=True))
    env.consumer.step()

    assert any("连不上" in t for t in env.transport.texts())
    assert env.drain_jobs() == []  # 没有 rerun_style


def test_gate_style_apply_all_renders_preview_and_approve_resolves(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "StyleApplyAll")

    env.put_event(GateReached(0, job.run_id, "StyleApplyAll", {
        "chosen_recipe": "Havana 1959", "preview_sent": True, "export_error": None}))
    env.consumer.step()

    assert ("这是用「Havana 1959」套用的效果，满意点\"满意\"，"
            "想换风格点\"重选\"或直接打字描述") in env.transport.texts()
    assert env.transport.button_tokens() == ["approve", "restyle"]

    env.push_text("不错就这个")
    env.consumer.step()
    [j] = env.drain_jobs()
    assert j.kind == "style_gate"  # 预览确认走 LLM，不再关键词
    env.put_event(ClassifyDone(0, "style_gate", StyleGateReply(action="approve")))
    env.consumer.step()

    [next_job] = env.drain_jobs()
    assert next_job.action == "resolve_gate"


def test_gate_style_apply_all_without_chosen_recipe_auto_proceeds(tmp_path):
    # AG-16.1：原图直出（chosen_recipe None）在 StyleApplyAll 无预览可确认，
    # 自动推进到交付闸门，不再问"没能选出风格"。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "StyleApplyAll")

    env.put_event(GateReached(0, job.run_id, "StyleApplyAll", {"chosen_recipe": None}))
    env.consumer.step()

    assert any("不套滤镜" in t for t in env.transport.texts())
    [next_job] = env.drain_jobs()
    assert next_job.action == "resolve_gate"


def test_gate_deliver_summary_then_llm_adjustment(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Deliver")

    env.put_event(GateReached(0, job.run_id, "Deliver", {
        "selected_count": 2, "applied_recipe": "Havana 1959",
        "preview_failed_count": 0, "export_error": None}))
    env.consumer.step()

    assert ("选好了 2 张，已套用风格「Havana 1959」，"
            "满意点\"满意\"，想调整点\"重选\"或直接打字说") in env.transport.texts()
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

    assert ("没听懂这句话，能再说清楚点吗？满意就点\"满意\"，"
            "想调整直接说想怎么调") in env.transport.texts()


def test_gate_reply_infra_failure_says_service_down(tmp_path):
    # AG-10：闸门回复分类因 Ollama 连不上(retryable)失败 -> "AI 连不上"而非"没听懂"。
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Deliver")
    env.put_event(GateReached(0, job.run_id, "Deliver", {
        "selected_count": 2, "applied_recipe": None,
        "preview_failed_count": 0, "export_error": None}))
    env.consumer.step()

    env.push_text("留三张吧")
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ClassifyFailed(0, "gate_reply", retryable=True))
    env.consumer.step()

    texts = env.transport.texts()
    assert any("连不上" in t for t in texts)
    assert not any("没听懂" in t for t in texts)


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

    env.put_event(JobCrashed(0, "drive", "RuntimeError('boom')"))
    env.consumer.step()
    assert env.consumer.view.drive_active is False
    assert env.consumer.view.status == RunStatus.RUNNING  # run 停在检查点
    assert "处理过程中出了点问题，这批先停在这儿了，回句话我接着试" in env.transport.texts()

    env.push_text("继续吧")
    env.consumer.step()

    jobs = env.drain_jobs()
    drive_jobs = [j for j in jobs if isinstance(j, DriveJob)]
    assert [j.action for j in drive_jobs] == ["resume"]  # 下一条消息触发续跑
    assert drive_jobs[0].run_id == job.run_id


def test_classify_crash_does_not_touch_active_drive(tmp_path):
    # AG-03：drive 正常跑时，classify lane 崩溃只清在途分类，不碰 drive 状态，
    # 否则会误触 resume 与真 DriveJob 排两个（预览重发、闸门重复提问）。
    env = make_consumer(tmp_path)
    job = to_running(env)
    assert env.consumer.view.drive_active is True
    env.consumer.inflight = {"type": "running", "text": "到哪了"}  # classify lane 在途

    env.put_event(JobCrashed(0, "classify", "RuntimeError('boom')"))
    env.consumer.step()

    assert env.consumer.inflight is None  # 只清了在途分类
    assert env.consumer.view.drive_active is True  # drive 不受牵连
    assert env.consumer.active_drive_job is job
    assert "刚才那条没能处理，能再说一次吗？" in env.transport.texts()


def test_drive_crash_does_not_touch_inflight_classify(tmp_path):
    # AG-03 对称情形：drive 崩溃时保留 classify lane 的在途分类，否则那条文
    # 本会没有任何回复。
    env = make_consumer(tmp_path)
    to_running(env)
    env.consumer.inflight = {"type": "running", "text": "到哪了"}

    env.put_event(JobCrashed(0, "drive", "RuntimeError('boom')"))
    env.consumer.step()

    assert env.consumer.inflight == {"type": "running", "text": "到哪了"}  # 保留
    assert env.consumer.view.drive_active is False
    assert env.consumer.active_drive_job is None


def test_collecting_cancel_intent_prompts_confirmation(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.push_text("这批先别弄了吧")  # 非关键词，走 LLM 分类
    env.consumer.step()
    env.drain_jobs()

    env.put_event(ClassifyDone(0, "collecting", CollectingReply(action="cancel")))
    env.consumer.step()

    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert env.transport.button_tokens() == ["confirm_cancel", "keep"]
    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING  # 还没真取消


def test_collecting_non_intent_message_gets_help_not_default_plan(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.push_text("你好呀你是谁")
    env.consumer.step()
    env.drain_jobs()

    env.put_event(ClassifyDone(0, "collecting", CollectingReply(action="other")))
    env.consumer.step()

    assert any("我是帮你选照片的小助手" in t for t in env.transport.texts())
    assert env.drain_jobs() == []  # 不投 compose，不硬编默认方案
    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING


def test_run_finished_done_sends_batch_wrapup(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)

    env.put_event(RunFinished(0, job.run_id, "done", None))
    env.consumer.step()

    assert "这批就处理完啦～想开新的一批，随时把照片发给我就行 📷" in env.transport.texts()
    assert env.consumer.view.run_id is None


def test_gate_reject_via_llm_prompts_confirmation_not_immediate_cancel(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Deliver")
    env.put_event(GateReached(0, job.run_id, "Deliver", {
        "selected_count": 2, "applied_recipe": None,
        "preview_failed_count": 0, "export_error": None}))
    env.consumer.step()

    env.push_text("这几张都不满意，不想要了")  # 非关键词，走 LLM 分类
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ClassifyDone(0, "gate_reply", GateReply(action="reject")))
    env.consumer.step()

    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert env.store.load(job.run_id).status == RunStatus.AWAITING_GATE  # 二次确认前不取消


def test_intent_before_photos_is_kept_as_draft(tmp_path):
    env = make_consumer(tmp_path)
    env.push_text("选三张发朋友圈")
    env.consumer.step()
    [job] = env.drain_jobs()
    assert job.kind == "collecting"  # 无 run，先分类
    deliver_classify(env, "collecting", CollectingReply(action="intent"))

    # 记成草稿：建了 run、intent_raw 存下，不丢
    [run] = env.store.list_active()
    assert run.status == RunStatus.COLLECTING
    assert run.intent_raw == "选三张发朋友圈"
    assert any("记下了" in t for t in env.transport.texts())
    assert env.drain_jobs() == []  # 还没照片，不 compose


def test_second_intent_while_draft_no_photos_merges_into_draft(tmp_path):
    # AG-08：草稿态（0 照片）再来一句意图 -> 并入草稿、不组空方案、不整句覆盖。
    env = make_consumer(tmp_path)
    env.push_text("选三张发朋友圈")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))
    env.drain_jobs()

    env.push_text("标签叫ins吧")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))

    [run] = env.store.list_active()
    assert run.intent_raw == "选三张发朋友圈；标签叫ins吧"
    assert any("记下了" in t for t in env.transport.texts())
    assert env.drain_jobs() == []  # 还没照片，不 compose


def test_intent_with_photos_and_draft_merges_before_compose(tmp_path):
    # AG-08：有草稿 + 有照片时再补一句意图 -> 拼接后再 compose，旧约束不丢。
    env = make_consumer(tmp_path)
    env.push_text("选三张发朋友圈")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))
    env.push_photo("a.jpg", b"a")
    env.consumer.step()
    env.drain_jobs()

    env.push_text("标签叫ins吧")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))

    [compose_job] = env.drain_jobs()
    assert isinstance(compose_job, ComposeJob)
    assert compose_job.intent_text == "选三张发朋友圈；标签叫ins吧"


def test_start_with_draft_but_no_photos_asks_for_photos(tmp_path):
    # AG-08：草稿态（0 照片）说"开始" -> 提示等照片，不用 0 张组方案。
    env = make_consumer(tmp_path)
    env.push_text("选三张发朋友圈")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))
    env.drain_jobs()

    env.push_text("开始吧")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="start"))

    assert any("还没收到照片" in t for t in env.transport.texts())
    assert env.drain_jobs() == []


def test_draft_then_photos_then_start_composes_from_draft(tmp_path):
    env = make_consumer(tmp_path)
    env.push_text("选三张发朋友圈")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))
    env.push_photo("a.jpg", b"a")
    env.consumer.step()

    env.push_text("好了开始吧")
    env.consumer.step()
    [job] = env.drain_jobs()
    assert job.kind == "collecting"
    env.put_event(ClassifyDone(env.consumer.generation, "collecting",
                                CollectingReply(action="start")))
    env.consumer.step()

    [compose_job] = env.drain_jobs()
    assert isinstance(compose_job, ComposeJob)
    assert compose_job.intent_text == "选三张发朋友圈"  # 用的是草稿


def test_start_without_draft_asks_for_intent(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg", b"a")
    env.consumer.step()

    env.push_text("开始吧")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="start"))

    assert any("还没告诉我想怎么处理" in t for t in env.transport.texts())
    assert env.drain_jobs() == []


def test_idle_with_draft_and_photos_auto_composes(tmp_path):
    from session_fakes import FakeClock
    clock = FakeClock()
    env = make_consumer(tmp_path, clock=clock)
    env.push_text("选三张发朋友圈")
    env.consumer.step()
    deliver_classify(env, "collecting", CollectingReply(action="intent"))
    env.push_photo("a.jpg", b"a")
    env.consumer.step()
    env.drain_jobs()

    clock.advance(300)
    env.consumer.step()

    [compose_job] = env.drain_jobs()
    assert isinstance(compose_job, ComposeJob)
    assert compose_job.intent_text == "选三张发朋友圈"
