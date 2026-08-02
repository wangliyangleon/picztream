"""「停下」= 叫停这一次 AI 尝试，不是作废整批（真机反馈 2026-08-02）。

票 10 第一版把开销消息上的按钮直接接到了整批取消上。真机验证：停是停下
了，但整批照片跟着一起没了 —— 而用户点它的动机从来不是"这批不要了"，是
"别用 AI 跑这一步"。两件事被折叠成了一个动作。

现在它退回**被停下的那一步**，上游成果留着，然后重新问一次那个本来就问
过的问题：要不要用 AI。打字说"取消"仍然是整批作废 + 二次确认，两条路分
开。
"""
from __future__ import annotations

from orchestrator.types import RunStatus, StageStatus
from session.protocol import RunRewound, StageCost, StageProgress, StageStarted
from session_fakes import FakeClock, make_consumer, to_running


def _running_env(tmp_path, stage: str = "Dedup"):
    clock = FakeClock()
    env = make_consumer(tmp_path, clock=clock)
    job = to_running(env)
    env.put_event(StageStarted(env.consumer.generation, job.run_id, stage))
    env.consumer.step()
    env.put_event(StageCost(env.consumer.generation, job.run_id, stage, 18, 0))
    env.consumer.step()
    return env, job


def test_stop_button_takes_one_click_and_asks_no_confirmation(tmp_path):
    """改成可恢复之后，二次确认原本的理由（误点会丢掉整批照片）不成立了。
    想停的时候要点两下，纯粹是摩擦。"""
    env, job = _running_env(tmp_path)

    env.push_callback(f"stop:{job.run_id}")
    env.consumer.step()

    assert job.cancel_event.is_set()
    assert job.on_cancel == "rewind"
    assert env.transport.button_tokens() == ["stop"]  # 没有再弹确认按钮


def test_stop_does_not_bump_the_generation_or_drop_the_batch(tmp_path):
    """整批取消会 _reset_session（generation +1、run 丢掉）。停下不能 ——
    那样紧接着回来的 RunRewound 会被当过期事件丢弃，用户按了个没反应的
    按钮。"""
    env, job = _running_env(tmp_path)
    gen_before = env.consumer.generation

    env.push_callback(f"stop:{job.run_id}")
    env.consumer.step()

    assert env.consumer.generation == gen_before
    assert env.consumer.cancelling_run_id is None  # 不是取消，不落 cancelling 标记


def test_typed_cancel_still_wipes_the_whole_batch_with_a_confirmation(tmp_path):
    # 两条路分开：打字"取消"的语义没变。
    env, job = _running_env(tmp_path)

    env.consumer._prompt_cancel_confirmation()

    assert env.transport.button_tokens() == ["confirm_cancel", "keep"]


# -- 回退之后回到哪一步 --


def _rewind(env, job, stage: str, partial=None):
    env.put_event(RunRewound(env.consumer.generation, job.run_id, stage, partial))
    env.consumer.step()


def test_stopping_dedup_returns_to_the_plan_confirmation_with_the_ai_button(tmp_path):
    """回到方案确认那一步，AI 关掉 - 刚被停下的就是它，默认再开一次等于
    没听见。

    按钮是"AI筛选"还是"AI去重"由 Plan 形状决定（张数定没定），不由被停的
    是哪一步决定；默认 fixture 的张数是定好的，走前者。用户日志里那条
    "先帮你去重" + [AI去重 🤖] 是张数待定的形状，下一条用例覆盖。"""
    env, job = _running_env(tmp_path, stage="Dedup")

    _rewind(env, job, "Dedup")

    text = env.transport.sent_buttons[-1][1]
    assert "可以吗？" in text
    assert env.transport.button_tokens() == ["approve", "ai_curate"]
    run = env.store.load(job.run_id)
    assert run.status == RunStatus.PLANNED
    assert run.stage_states["Dedup"] == StageStatus.PENDING


def test_stopping_dedup_on_a_deferred_plan_reoffers_ai_dedup(tmp_path):
    # 用户真机日志里那条原话的形状：张数待定，问的是"先帮你去重…可以吗？"
    env, job = _running_env(tmp_path, stage="Dedup")
    run = env.store.load(job.run_id)
    next(s for s in run.plan.stages if s.name == "Curate").params["count"] = None
    env.store.save(run)

    _rewind(env, job, "Dedup")

    assert "先帮你去重" in env.transport.sent_buttons[-1][1]
    assert env.transport.button_tokens() == ["approve", "ai_dedup"]


def test_stopping_curate_returns_to_the_dedup_followup_gate(tmp_path):
    """count 待定那条 Plan 上，'要不要用 AI 选片'这个问题是在去重后的追问
    闸门上问的，不在最初的方案确认上。回错地方等于把已经跑完的去重也一起
    重问一遍。"""
    env, job = _running_env(tmp_path, stage="Curate")
    run = env.store.load(job.run_id)
    curate = next(s for s in run.plan.stages if s.name == "Curate")
    curate.gate = "required"           # 追问闸门那条形状
    run.stage_states["Dedup"] = StageStatus.DONE
    env.store.save(run)

    _rewind(env, job, "Curate")

    text = env.transport.sent_buttons[-1][1]
    assert "要不要再筛选" in text
    assert "ai_narrow" in env.transport.button_tokens()
    assert env.store.load(job.run_id).status == RunStatus.AWAITING_GATE


def test_stopping_curate_without_a_followup_gate_returns_to_the_plan_confirmation(tmp_path):
    # 张数一开始就说定了那条形状：AI 的选择在方案确认上问过，回那儿。
    env, job = _running_env(tmp_path, stage="Curate")

    _rewind(env, job, "Curate")

    assert env.transport.button_tokens() == ["approve", "ai_curate"]


def test_stopping_curate_does_not_rerun_the_finished_dedup(tmp_path):
    env, job = _running_env(tmp_path, stage="Curate")
    run = env.store.load(job.run_id)
    run.stage_states["Dedup"] = StageStatus.DONE
    env.store.save(run)

    _rewind(env, job, "Curate")

    assert env.store.load(job.run_id).stage_states["Dedup"] == StageStatus.DONE


def test_rewind_turns_the_ai_switch_back_off(tmp_path):
    # 全局开关（SPEC §3.3）：Dedup/Curate 一起关。不关的话方案确认那条消
    # 息只会给一个"好的"，用户唯一能点的按钮就是再跑一次 AI。
    env, job = _running_env(tmp_path, stage="Dedup")

    _rewind(env, job, "Dedup")

    run = env.store.load(job.run_id)
    for name in ("Dedup", "Curate"):
        spec = next(s for s in run.plan.stages if s.name == name)
        assert spec.params.get("ai_enabled") is False


def test_rewind_receipt_says_what_was_kept(tmp_path):
    # 评估段停下时那几张的评价留在库里，跟取消回执同一套诚实。
    env, job = _running_env(tmp_path, stage="Curate")

    _rewind(env, job, "Curate", partial=("Curate", 4, 12, "evaluations"))

    texts = env.transport.texts()
    assert any("4/12" in t and "留在库里" in t for t in texts)


def test_rewind_does_not_close_the_progress_message_out_as_finished(tmp_path):
    """停下不是跑完。把半截进度改写成"两两比较跑完了，共 18 次"是撒谎 ——
    跟取消路径同一条规矩。"""
    env, job = _running_env(tmp_path, stage="Dedup")
    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Dedup", 3, 18, "comparisons"))
    env.consumer.step()
    edits_before = len(env.transport.sent_edits)

    _rewind(env, job, "Dedup")

    assert len(env.transport.sent_edits) == edits_before


def test_user_can_proceed_without_ai_after_stopping(tmp_path):
    # 整条路要真的能走完：停 -> 回到选择 -> 点"好的" -> 不带 AI 接着跑。
    env, job = _running_env(tmp_path, stage="Dedup")
    _rewind(env, job, "Dedup")

    env.push_callback(f"approve:{env.consumer.view.run_id}")
    env.consumer.step()

    drive = [j for j in env.drain_jobs() if getattr(j, "action", None) == "start"]
    assert len(drive) == 1
    run = env.store.load(job.run_id)
    dedup = next(s for s in run.plan.stages if s.name == "Dedup")
    assert dedup.params.get("ai_enabled") is False


def test_stale_stop_button_from_an_older_message_is_rejected(tmp_path):
    env, job = _running_env(tmp_path)

    env.push_callback("stop:tg-someone-else")
    env.consumer.step()

    assert not job.cancel_event.is_set()
