"""运行期进度播报（T-8 G3）。

在这条改动之前 RUNNING 期间一条周期性消息都没有：`_check_idle_reminder`
与 `_check_collecting_progress` 都显式跳过 RUNNING，`view.stage_progress`
只被赋过 None，`view.describe()` 里依赖它的那个分支是死代码。

播报文案直接复用 `view.describe()`，不另起一套措辞 - view 已经是"当前
状态怎么说给用户听"的唯一真相源。
"""
from __future__ import annotations

from orchestrator.types import RunStatus
from session.protocol import GateReached, RunFinished, StageProgress, StageStarted
from session_fakes import FakeClock, make_consumer, to_running, worker_saves_gate


def _running_env(tmp_path, interval: float = 60.0):
    clock = FakeClock()
    env = make_consumer(tmp_path, clock=clock, progress_interval_seconds=interval)
    job = to_running(env)
    env.put_event(StageStarted(env.consumer.generation, job.run_id, "Curate"))
    env.consumer.step()
    return env, job


def test_stage_progress_fills_the_view_and_gets_broadcast(tmp_path):
    env, job = _running_env(tmp_path)

    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Curate", 3, 10, "groups"))
    env.consumer.step()

    assert env.consumer.view.stage_progress == (3, 10, "groups")
    # 单位跟着数字走：这里数的是候选簇，不是照片张数（真机验收踩到的原话
    # 是"已完成 1/1 张"，用户明明要选 3 张）。
    assert "3/10组" in env.transport.texts()[-1]


def test_first_progress_of_a_stage_is_sent_immediately(tmp_path):
    # 刚进一个 stage 就等满一个 interval 才吭声，等于 U-10 抱怨的沉默又
    # 回来了一遍（只是短一点）。第一条必须立刻出去。
    env, job = _running_env(tmp_path, interval=600.0)

    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Curate", 1, 20, "comparisons"))
    env.consumer.step()

    assert "1/20次" in env.transport.texts()[-1]


def test_progress_between_intervals_updates_the_view_but_does_not_send(tmp_path):
    # 决策二：core/cli 每次都写，节流在这里做。一个 20 张的簇是 19 次比
    # 较，每次发一条会被 Telegram 限流。
    env, job = _running_env(tmp_path, interval=60.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 20, "comparisons"))
    env.consumer.step()
    before = len(env.transport.sent_texts) + len(env.transport.sent_edits)

    env.clock.advance(5)
    env.put_event(StageProgress(gen, job.run_id, "Curate", 2, 20, "comparisons"))
    env.consumer.step()

    assert env.consumer.view.stage_progress == (2, 20, "comparisons")  # view 始终是最新的
    assert len(env.transport.sent_texts) + len(env.transport.sent_edits) == before


def test_progress_sends_again_after_the_interval_elapses(tmp_path):
    env, job = _running_env(tmp_path, interval=60.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 20, "comparisons"))
    env.consumer.step()

    env.clock.advance(61)
    env.put_event(StageProgress(gen, job.run_id, "Curate", 9, 20, "comparisons"))
    env.consumer.step()

    assert "9/20" in (env.transport.texts() + [t for _, t in env.transport.sent_edits])[-1]


def test_each_stage_starts_a_fresh_progress_message(tmp_path):
    # 进度是原地编辑的（AG-16.3 的 _send_progress）。不在 StageStarted 时
    # 换槽的话，Curate 的进度会去改写 Dedup 那条消息，用户翻回去看到的历
    # 史是错的。
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 5, "groups"))
    env.consumer.step()
    edits_before = len(env.transport.sent_edits)

    env.put_event(StageStarted(gen, job.run_id, "Deliver"))
    env.consumer.step()
    env.put_event(StageProgress(gen, job.run_id, "Deliver", 1, 5, "photos"))
    env.consumer.step()

    # 换 stage 时只该多一次编辑：把上一条进度收尾（"照片组都处理完了"）。
    # 新 stage 的进度必须是一条新消息，不是继续改写上一条。
    assert len(env.transport.sent_edits) == edits_before + 1
    assert "处理完了" in env.transport.sent_edits[-1][1]
    assert "1/5张" in env.transport.texts()[-1]


def test_stale_generation_progress_is_dropped(tmp_path):
    env, job = _running_env(tmp_path)
    sent_before = len(env.transport.sent_texts)

    env.put_event(StageProgress(env.consumer.generation - 1, job.run_id, "Curate", 3, 10, "groups"))
    env.consumer.step()

    assert env.consumer.view.stage_progress is None
    assert len(env.transport.sent_texts) == sent_before


# -- 取消回执带上部分成果（T-8 决策五）--


def test_cancel_receipt_says_how_many_were_already_styled(tmp_path):
    # StyleApplyAll 的写入是逐张的，取消一定留下部分成果。只说"已取消"
    # 等于假装什么都没发生 —— 用户回头看到一半照片带滤镜会更困惑。
    env, job = _running_env(tmp_path)
    env.consumer._do_cancel()
    env.put_event(RunFinished(0, job.run_id, RunStatus.CANCELLED.value,
                               cancelled_partial=("StyleApplyAll", 3, 10)))
    env.consumer.step()

    text = env.transport.texts()[-1]
    assert "已取消" in text and "3/10" in text


def test_cancel_receipt_stays_bare_when_nothing_was_written(tmp_path):
    # dedup/curate 的取消按 core 契约是零写入，回执一个数字都不该有。
    env, job = _running_env(tmp_path)
    env.consumer._do_cancel()
    env.put_event(RunFinished(0, job.run_id, RunStatus.CANCELLED.value))
    env.consumer.step()

    assert env.transport.texts()[-1] == "已取消"


def test_reset_session_clears_the_stage_progress_slot(tmp_path):
    # 进度消息是原地编辑的（AG-16.3）。槽不随会话重置的话，下一批的第一
    # 条进度会去编辑上一批那条已经作废的消息。_collecting_progress 一直
    # 是这么做的，运行期这两个是 B.1a 漏掉的。
    env, job = _running_env(tmp_path)
    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Curate", 1, 5, "groups"))
    env.consumer.step()
    assert env.consumer._stage_progress is not None

    env.consumer._reset_session()

    assert env.consumer._stage_progress is None
    assert env.consumer._stage_progress_notified_at is None


# -- 进度要有个终态（真机反馈）--


def test_the_final_tick_is_never_throttled_away(tmp_path):
    # 最后一跳往往紧跟着前一跳（比如 2/3 -> 3/3 中间只隔几十毫秒），正好
    # 落在节流窗口里被吃掉，于是可见的最后一条停在 2/3。
    env, job = _running_env(tmp_path, interval=600.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "StyleApplyAll", 2, 3, "photos"))
    env.consumer.step()

    env.clock.advance(1)  # 远不到 600 秒
    env.put_event(StageProgress(gen, job.run_id, "StyleApplyAll", 3, 3, "photos"))
    env.consumer.step()

    assert "3/3" in env.transport.sent_edits[-1][1]


def test_progress_message_is_closed_out_when_the_next_stage_starts(tmp_path):
    # 光有 3/3 还不够：那句话仍然是"正在…"，读起来像还在跑。下一个 stage
    # 开跑说明这个已经完了，把同一条消息改成终态。
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "StyleApplyAll", 3, 3, "photos"))
    env.consumer.step()

    env.put_event(StageStarted(gen, job.run_id, "Deliver"))
    env.consumer.step()

    closed = env.transport.sent_edits[-1][1]
    assert "正在" not in closed
    assert "3" in closed


def test_progress_message_is_closed_out_at_a_gate(tmp_path):
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 4, 4, "groups"))
    env.consumer.step()

    worker_saves_gate(env, job.run_id, "Style")
    env.put_event(GateReached(gen, job.run_id, "Style", {
        "selected_count": 2, "preview_failed_count": 0, "export_error": None}))
    env.consumer.step()

    assert "正在" not in env.transport.sent_edits[-1][1]


# -- 同一个 stage 里换 phase（票 09）--
#
# 票 05 之后，开 AI 的 curate 会在一个 Curate stage 里先比较、后逐张评估。
# 两段数的东西不同（次 / 张），挤在同一条消息里原地编辑的话，用户会看到
# "已完成 160/160 次"直接变成"已完成 1/6 张" —— 分子分母同时跳，读起来像
# 进度条倒退；比较那条的终态句也就永远发不出去了。


def test_switching_phase_closes_out_the_previous_progress_message(tmp_path):
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 9, 9, "comparisons"))
    env.consumer.step()

    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 6, "evaluations"))
    env.consumer.step()

    closed = env.transport.sent_edits[-1][1]
    assert "正在" not in closed
    assert "9" in closed and "次" in closed


def test_switching_phase_starts_a_new_message_instead_of_editing(tmp_path):
    # 不换槽的话评估进度会去改写比较那条，用户往回翻看到的历史是错的。
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 9, 9, "comparisons"))
    env.consumer.step()
    texts_before = len(env.transport.sent_texts)

    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 6, "evaluations"))
    env.consumer.step()

    assert len(env.transport.sent_texts) == texts_before + 1
    assert "1/6张" in env.transport.texts()[-1]


def test_first_frame_after_a_phase_switch_is_not_throttled(tmp_path):
    # 换槽必须把计时器一起归零，否则评估的第一帧落在比较那一帧的节流窗口
    # 里被吃掉，用户在分钟级的评估阶段面对的是一条已经收尾的消息。
    env, job = _running_env(tmp_path, interval=600.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 9, 9, "comparisons"))
    env.consumer.step()

    env.clock.advance(1)  # 远不到 600 秒
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 6, "evaluations"))
    env.consumer.step()

    assert "1/6张" in env.transport.texts()[-1]


def test_same_phase_keeps_editing_one_message(tmp_path):
    # 反向保护：同一个 phase 内不该被这套换槽逻辑带得每帧发新消息。
    env, job = _running_env(tmp_path, interval=0.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 9, "comparisons"))
    env.consumer.step()
    texts_before = len(env.transport.sent_texts)

    env.put_event(StageProgress(gen, job.run_id, "Curate", 2, 9, "comparisons"))
    env.consumer.step()

    assert len(env.transport.sent_texts) == texts_before
    assert "2/9次" in env.transport.sent_edits[-1][1]


def test_cancelled_progress_is_not_closed_out_as_finished(tmp_path):
    # 取消不是完成。把半截进度改成"套完了"是在撒谎。
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "StyleApplyAll", 1, 10, "photos"))
    env.consumer.step()
    edits_before = len(env.transport.sent_edits)

    env.consumer._do_cancel()
    env.put_event(RunFinished(0, job.run_id, RunStatus.CANCELLED.value,
                               cancelled_partial=("StyleApplyAll", 1, 10)))
    env.consumer.step()

    assert len(env.transport.sent_edits) == edits_before
