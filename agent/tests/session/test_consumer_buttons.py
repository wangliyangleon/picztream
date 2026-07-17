"""inline 按钮（callback_query）路径：所有 yes/no 确认点都挂按钮，点击
经 kind="callback" 的 InboundMessage 回到 consumer，映射到跟打字关键词
一致的处理（docs/W2026-07-15_AgentRuntime_Eng_Design.md 真机反馈调整）。
校验点：按钮附在哪些确认上、点击语义、run_id 防误触、降级到纯文本。
"""
from __future__ import annotations

from orchestrator.types import RunStatus
from session.protocol import DriveJob, GateReached

from session_fakes import make_consumer, to_planned, to_running, worker_saves_gate


def _reach_style_apply_all_gate(env, run_id):
    worker_saves_gate(env, run_id, "StyleApplyAll")
    env.put_event(GateReached(0, run_id, "StyleApplyAll", {
        "chosen_recipe": "Havana 1959", "preview_sent": True, "export_error": None}))
    env.consumer.step()


def test_plan_confirmation_attaches_approve_reject_buttons(tmp_path):
    env = make_consumer(tmp_path)
    to_planned(env)

    assert env.transport.button_tokens() == ["approve", "reject"]
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


def test_reject_button_at_planned_cancels(tmp_path):
    env = make_consumer(tmp_path)
    run = to_planned(env)

    env.push_callback(f"reject:{run.run_id}")
    env.consumer.step()

    assert env.store.load(run.run_id).status == RunStatus.CANCELLED
    assert "已取消" in env.transport.texts()
    assert env.consumer.view.run_id is None


def test_approve_button_at_style_apply_all_resolves_gate(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    _reach_style_apply_all_gate(env, job.run_id)
    assert env.transport.button_tokens() == ["approve", "restyle", "reject"]

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

    # 用户接着打字给新描述 -> 走既有的 rerun_style 路径
    env.push_text("再冷一点")
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


def test_style_ask_gate_only_offers_cancel_button(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(0, job.run_id, "Style", {}))
    env.consumer.step()

    assert env.transport.button_tokens() == ["reject"]  # 问描述是开放式，无批准按钮


def test_degrades_to_plain_text_without_send_buttons_capability(tmp_path):
    env = make_consumer(tmp_path)
    # 抹掉 transport 的按钮能力，模拟非 Telegram / 旧版 transport（consumer
    # 用 getattr 探测，取到 None 就降级纯文本）
    env.transport.send_buttons = None
    to_planned(env)

    # 纯文本仍然发出（走 _send 降级），关键词/打字路径照常可用
    assert any("理解你想" in t for t in env.transport.texts())
    assert env.transport.sent_buttons == []
