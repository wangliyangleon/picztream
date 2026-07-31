"""运行期进度播报（T-8 G3）。

在这条改动之前 RUNNING 期间一条周期性消息都没有：`_check_idle_reminder`
与 `_check_collecting_progress` 都显式跳过 RUNNING，`view.stage_progress`
只被赋过 None，`view.describe()` 里依赖它的那个分支是死代码。

播报文案直接复用 `view.describe()`，不另起一套措辞 - view 已经是"当前
状态怎么说给用户听"的唯一真相源。
"""
from __future__ import annotations

from orchestrator.types import RunStatus
from session.protocol import RunFinished, StageProgress, StageStarted
from session_fakes import FakeClock, make_consumer, to_running


def _running_env(tmp_path, interval: float = 60.0):
    clock = FakeClock()
    env = make_consumer(tmp_path, clock=clock, progress_interval_seconds=interval)
    job = to_running(env)
    env.put_event(StageStarted(env.consumer.generation, job.run_id, "Curate"))
    env.consumer.step()
    return env, job


def test_stage_progress_fills_the_view_and_gets_broadcast(tmp_path):
    env, job = _running_env(tmp_path)

    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Curate", 3, 10))
    env.consumer.step()

    assert env.consumer.view.stage_progress == (3, 10)
    assert "3/10" in env.transport.texts()[-1]


def test_first_progress_of_a_stage_is_sent_immediately(tmp_path):
    # 刚进一个 stage 就等满一个 interval 才吭声，等于 U-10 抱怨的沉默又
    # 回来了一遍（只是短一点）。第一条必须立刻出去。
    env, job = _running_env(tmp_path, interval=600.0)

    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Curate", 1, 20))
    env.consumer.step()

    assert "1/20" in env.transport.texts()[-1]


def test_progress_between_intervals_updates_the_view_but_does_not_send(tmp_path):
    # 决策二：core/cli 每次都写，节流在这里做。一个 20 张的簇是 19 次比
    # 较，每次发一条会被 Telegram 限流。
    env, job = _running_env(tmp_path, interval=60.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 20))
    env.consumer.step()
    before = len(env.transport.sent_texts) + len(env.transport.sent_edits)

    env.clock.advance(5)
    env.put_event(StageProgress(gen, job.run_id, "Curate", 2, 20))
    env.consumer.step()

    assert env.consumer.view.stage_progress == (2, 20)  # view 始终是最新的
    assert len(env.transport.sent_texts) + len(env.transport.sent_edits) == before


def test_progress_sends_again_after_the_interval_elapses(tmp_path):
    env, job = _running_env(tmp_path, interval=60.0)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 20))
    env.consumer.step()

    env.clock.advance(61)
    env.put_event(StageProgress(gen, job.run_id, "Curate", 9, 20))
    env.consumer.step()

    assert "9/20" in (env.transport.texts() + [t for _, t in env.transport.sent_edits])[-1]


def test_each_stage_starts_a_fresh_progress_message(tmp_path):
    # 进度是原地编辑的（AG-16.3 的 _send_progress）。不在 StageStarted 时
    # 换槽的话，Curate 的进度会去改写 Dedup 那条消息，用户翻回去看到的历
    # 史是错的。
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation
    env.put_event(StageProgress(gen, job.run_id, "Curate", 1, 5))
    env.consumer.step()
    edits_before = len(env.transport.sent_edits)

    env.put_event(StageStarted(gen, job.run_id, "Deliver"))
    env.consumer.step()
    env.put_event(StageProgress(gen, job.run_id, "Deliver", 1, 5))
    env.consumer.step()

    assert len(env.transport.sent_edits) == edits_before  # 新消息，不是编辑旧的


def test_stale_generation_progress_is_dropped(tmp_path):
    env, job = _running_env(tmp_path)
    sent_before = len(env.transport.sent_texts)

    env.put_event(StageProgress(env.consumer.generation - 1, job.run_id, "Curate", 3, 10))
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
    env.put_event(StageProgress(env.consumer.generation, job.run_id, "Curate", 1, 5))
    env.consumer.step()
    assert env.consumer._stage_progress is not None

    env.consumer._reset_session()

    assert env.consumer._stage_progress is None
    assert env.consumer._stage_progress_notified_at is None
