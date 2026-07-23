"""inline 按钮（callback_query）路径：所有 yes/no 确认点都挂按钮，点击
经 kind="callback" 的 InboundMessage 回到 consumer，映射到跟打字关键词
一致的处理（docs/W2026-07-15_AgentRuntime_Eng_Design.md 真机反馈调整）。
校验点：按钮附在哪些确认上、点击语义、run_id 防误触、降级到纯文本。
"""
from __future__ import annotations

from compose.adjustment_parser import PlanConfirmationReply, StyleGateReply
from orchestrator.types import RunStatus
from session.protocol import ClassifyDone, DriveJob, GateReached

from session_fakes import deliver_classify, make_consumer, to_planned, to_running, worker_saves_gate


def _reach_style_apply_all_gate(env, run_id):
    worker_saves_gate(env, run_id, "StyleApplyAll")
    env.put_event(GateReached(0, run_id, "StyleApplyAll", {
        "chosen_recipe": "Havana 1959", "preview_sent": True, "export_error": None}))
    env.consumer.step()


def test_plan_confirmation_attaches_approve_and_ai_buttons(tmp_path):
    env = make_consumer(tmp_path)
    to_planned(env)

    # 取消不再是按钮（危险操作）；ai_enabled 默认 False，所以"好的"之外还
    # 带一个 AI 快捷按钮（W2026-07-21 目标三决策五）。
    assert env.transport.button_tokens() == ["approve", "ai_curate"]
    chat, text, options = env.transport.sent_buttons[-1]
    run_id = env.consumer.view.run_id
    assert options[0] == ("好的 ✅", f"approve:{run_id}")


def test_approve_button_at_planned_starts_drive(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)

    env.push_callback(f"approve:{run.run_id}")
    env.consumer.step()

    assert "开始处理了，共 1 张" in env.transport.texts()
    [job] = env.drain_jobs()
    assert isinstance(job, DriveJob)
    assert job.action == "start"
    assert env.store.load(run.run_id).status == RunStatus.RUNNING


def test_approve_button_ignored_while_classify_inflight(tmp_path):
    # AG-07：打字调整在途(inflight 非空)时点"好的"不该用旧参数抢跑；落地后再点才生效。
    env = make_consumer(tmp_path)
    run = to_planned(env)

    env.push_text("改成6张")  # refine classify 在途
    env.consumer.step()
    env.drain_jobs()  # 排掉 refine ClassifyJob

    env.push_callback(f"approve:{run.run_id}")
    env.consumer.step()

    assert "上一条还在处理，稍等一下再点～" in env.transport.texts()
    assert env.drain_jobs() == []  # 没有 start DriveJob
    assert env.store.load(run.run_id).status == RunStatus.PLANNED

    # 分类落地清 inflight 后再点 -> 正常开跑（只是延后，不是永久拦）。
    env.put_event(ClassifyDone(env.consumer.generation, "refine_plan",
                                PlanConfirmationReply(action="confirmed", count=6)))
    env.consumer.step()
    env.drain_jobs()
    env.push_callback(f"approve:{run.run_id}")
    env.consumer.step()
    [job] = env.drain_jobs()
    assert isinstance(job, DriveJob) and job.action == "start"


def _prompt_cancel_at_planned(env):
    # PLANNED 态打字取消：走 refine 分类判 reject -> 二次确认。
    env.push_text("不想弄了")
    env.consumer.step()
    deliver_classify(env, "refine_plan", PlanConfirmationReply(action="reject"))


def test_typed_cancel_prompts_confirmation_buttons(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)

    _prompt_cancel_at_planned(env)

    assert any("确定要取消整批吗" in t for t in env.transport.texts())
    assert env.transport.button_tokens() == ["confirm_cancel", "keep"]
    assert env.store.load(run.run_id).status == RunStatus.PLANNED  # 还没真取消


def test_confirm_cancel_button_cancels(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)
    _prompt_cancel_at_planned(env)

    env.push_callback(f"confirm_cancel:{run.run_id}")
    env.consumer.step()

    assert env.store.load(run.run_id).status == RunStatus.CANCELLED
    assert "已取消" in env.transport.texts()
    assert env.consumer.view.run_id is None


def test_keep_button_dismisses_cancel(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)
    _prompt_cancel_at_planned(env)

    env.push_callback(f"keep:{run.run_id}")
    env.consumer.step()

    assert "好，继续" in env.transport.texts()
    assert env.store.load(run.run_id).status == RunStatus.PLANNED
    assert env.consumer._cancel_confirm_pending is False


def test_confirm_cancel_button_without_pending_is_stale(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)  # 没先打"取消"，没有待确认

    env.push_callback(f"confirm_cancel:{run.run_id}")
    env.consumer.step()

    assert "这个选项已经过期了，看我最新的消息哈" in env.transport.texts()
    assert env.store.load(run.run_id).status == RunStatus.PLANNED


def test_approve_button_at_style_apply_all_resolves_gate(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    _reach_style_apply_all_gate(env, job.run_id)
    assert env.transport.button_tokens() == ["approve", "restyle"]

    env.push_callback(f"approve:{job.run_id}")
    env.consumer.step()

    [drive_job] = env.drain_jobs()
    assert drive_job.action == "resolve_gate"


def test_restyle_button_prompts_for_new_description(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    _reach_style_apply_all_gate(env, job.run_id)

    env.push_callback(f"restyle:{job.run_id}")
    env.consumer.step()

    assert "想要什么风格？直接打字告诉我，比如\"复古暖色调\"" in env.transport.texts()
    assert env.drain_jobs() == []  # 只是提示，不触发任何 drive
    assert env.consumer.view.status == RunStatus.AWAITING_GATE  # 还停在闸门

    # 用户接着打字给新描述 -> style_gate 分类判 redescribe -> rerun_style
    env.push_text("再冷一点")
    env.consumer.step()
    [cj] = env.drain_jobs()
    assert cj.kind == "style_gate"
    env.put_event(ClassifyDone(0, "style_gate", StyleGateReply(action="redescribe")))
    env.consumer.step()
    [job2] = env.drain_jobs()
    assert job2.action == "rerun_style"
    assert job2.args == {"style_description": "再冷一点"}


def test_stale_callback_with_wrong_run_id_is_rejected(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)

    env.push_callback("approve:tg-someoldrun")
    env.consumer.step()

    assert "这个选项已经过期了，看我最新的消息哈" in env.transport.texts()
    assert env.drain_jobs() == []
    assert env.store.load(run.run_id).status == RunStatus.PLANNED  # 没被误触发


def test_callback_during_drive_is_treated_as_stale(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)  # drive_active True, run 交给 worker

    env.push_callback(f"approve:{job.run_id}")
    env.consumer.step()

    assert "这个选项已经过期了，看我最新的消息哈" in env.transport.texts()
    assert env.drain_jobs() == []


def test_style_ask_gate_has_no_buttons(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    before = len(env.transport.sent_buttons)
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    # 问描述是开放式，纯文本，不挂任何按钮
    assert len(env.transport.sent_buttons) == before
    assert "想要什么风格？用一句话描述就行，比如\"复古暖色调\"" in env.transport.texts()


def test_degrades_to_plain_text_without_send_buttons_capability(tmp_path):
    env = make_consumer(tmp_path)
    # 抹掉 transport 的按钮能力，模拟非 Telegram / 旧版 transport（consumer
    # 用 getattr 探测，取到 None 就降级纯文本）
    env.transport.send_buttons = None
    to_planned(env)

    # 纯文本仍然发出（走 _send 降级），关键词/打字路径照常可用
    assert any("理解你想" in t for t in env.transport.texts())
    assert env.transport.sent_buttons == []
