"""SessionConsumer 的三个 timer（idle 提醒/收图播报沿用旧语义，Evaluate
进度轮询是 2.0 新增）与启动恢复三分支（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第七节第 6、7 条）。时间全部走注入的 FakeClock。
"""
from __future__ import annotations

from orchestrator.types import RunState, RunStatus, StageStatus
from session.protocol import DriveJob, StageStarted
from store.run_store import RunStore

from session_fakes import (
    FakeClock,
    make_consumer,
    make_fixed_plan,
    to_running,
)


def test_idle_reminder_fires_once_and_resets_on_activity(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()

    env.clock.advance(300)
    env.consumer.step()
    assert env.transport.texts().count("看到你发了 1 张，想怎么处理？") == 1

    env.consumer.step()  # 一次性：不重复提醒
    assert env.transport.texts().count("看到你发了 1 张，想怎么处理？") == 1

    env.push_photo("b.jpg")  # 活动重置提醒标记
    env.consumer.step()
    env.clock.advance(300)
    env.consumer.step()
    assert env.transport.texts().count("看到你发了 2 张，想怎么处理？") == 1


def test_collecting_progress_broadcast_waits_full_interval(tmp_path):
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.push_photo("b.jpg")
    env.consumer.step()
    assert "已收到 2 张图片" not in env.transport.texts()  # 首播也要等满间隔

    env.clock.advance(60)
    env.consumer.step()

    assert "已收到 2 张图片" in env.transport.texts()


def test_eval_progress_poll_counts_evaluated_images(tmp_path):
    env = make_consumer(tmp_path)
    job = to_running(env)
    env.put_event(StageStarted(0, job.run_id, "Evaluate"))
    env.consumer.step()
    assert not any(args[0] == "images" for args in env.client.calls)

    env.clock.advance(60)
    env.consumer.step()

    assert ("images", job.run_id) in env.client.calls  # 只读查询，project_id == run_id
    assert env.consumer.view.stage_progress == (1, 2)  # fake images: 2 张中 1 张已评估
    assert "评估进行中，已完成 1/2 张" in env.transport.texts()

    env.clock.advance(30)
    env.consumer.step()  # 没到下一个间隔，不再轮询
    assert sum(1 for args in env.client.calls if args[0] == "images") == 1


def test_collecting_progress_edits_in_place_on_later_ticks(tmp_path):
    # AG-16.3：同一批的收图进度只占一条消息，后续 tick 走 editMessageText。
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.clock.advance(60)
    env.consumer.step()  # 首播 -> 发新
    assert "已收到 1 张图片" in env.transport.texts()
    n_sends = sum(1 for t in env.transport.texts() if "已收到" in t)

    env.push_photo("b.jpg")
    env.consumer.step()
    env.clock.advance(60)
    env.consumer.step()  # 第二次 -> 编辑同一条

    assert any(t == "已收到 2 张图片" for _, t in env.transport.sent_edits)
    assert sum(1 for t in env.transport.texts() if "已收到" in t) == n_sends  # 没新发


def test_progress_unchanged_text_is_not_resent_or_edited(tmp_path):
    # AG-16.3：内容没变（照片数没变）就既不发也不编辑，不刷屏。
    env = make_consumer(tmp_path)
    env.push_photo("a.jpg")
    env.consumer.step()
    env.clock.advance(60)
    env.consumer.step()  # 首播 "已收到 1 张图片"
    sends = sum(1 for t in env.transport.texts() if "已收到" in t)
    edits = len(env.transport.sent_edits)

    env.clock.advance(60)
    env.consumer.step()  # 还是 1 张，内容不变

    assert sum(1 for t in env.transport.texts() if "已收到" in t) == sends
    assert len(env.transport.sent_edits) == edits


def test_eval_progress_edits_in_place_on_later_ticks(tmp_path):
    # AG-16.3：Evaluate 进度后续 tick 也原地编辑。造两次不同的 evaluated 计数。
    env = make_consumer(tmp_path)
    job = to_running(env)
    env.put_event(StageStarted(0, job.run_id, "Evaluate"))
    env.consumer.step()

    counts = iter([  # 第一次 1/2，第二次 2/2
        {"images": [{"evaluated": True}, {"evaluated": False}]},
        {"images": [{"evaluated": True}, {"evaluated": True}]},
    ])
    env.client.call = lambda *a: next(counts)

    env.clock.advance(60)
    env.consumer.step()  # 首播 1/2 -> 发新
    assert "评估进行中，已完成 1/2 张" in env.transport.texts()

    env.clock.advance(60)
    env.consumer.step()  # 2/2 -> 编辑

    assert any(t == "评估进行中，已完成 2/2 张" for _, t in env.transport.sent_edits)


def test_eval_poll_failure_is_tolerated_silently(tmp_path):
    env = make_consumer(tmp_path)

    def exploding_call(*args):
        raise RuntimeError("db locked")

    env.client.call = exploding_call
    job = to_running(env)
    env.put_event(StageStarted(0, job.run_id, "Evaluate"))
    env.consumer.step()
    before = len(env.transport.texts())

    env.clock.advance(60)
    env.consumer.step()  # 不炸、不发进度

    assert len(env.transport.texts()) == before
    assert env.consumer.view.stage_progress is None


def _prefill_run(tmp_path, run_id: str, status: RunStatus) -> RunState:
    store = RunStore(tmp_path / "runs")
    plan = make_fixed_plan(str(tmp_path / "incoming" / run_id), str(tmp_path / "deliver-out"))
    run = RunState(
        run_id=run_id, project_id=run_id, plan=plan,
        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
        status=status, intent_raw="筛一下留2张",
    )
    store.save(run)
    return run


def test_bootstrap_resumes_running_run(tmp_path):
    _prefill_run(tmp_path, "tg-boot1", RunStatus.RUNNING)
    env = make_consumer(tmp_path)

    env.consumer.bootstrap()

    assert "上次处理被中断，正在接着跑…" in env.transport.texts()
    [job] = env.drain_jobs()
    assert isinstance(job, DriveJob)
    assert job.action == "resume"
    assert job.run_id == "tg-boot1"
    assert env.consumer.view.drive_active is True


def test_bootstrap_adopts_planned_run(tmp_path):
    _prefill_run(tmp_path, "tg-boot2", RunStatus.PLANNED)
    env = make_consumer(tmp_path)

    env.consumer.bootstrap()

    assert env.transport.texts() == []  # 不重发闸门/确认提示，靠 idle 提醒兜底
    assert env.consumer.view.status == RunStatus.PLANNED
    assert env.consumer.run is not None

    env.push_text("好的")
    env.consumer.step()
    env.drain_jobs()  # refine_plan ClassifyJob
    from compose.adjustment_parser import PlanConfirmationReply
    from session.protocol import ClassifyDone
    env.put_event(ClassifyDone(0, "refine_plan", PlanConfirmationReply(action="approve")))
    env.consumer.step()
    [job] = env.drain_jobs()
    assert job.action == "start"


def test_bootstrap_sweeps_stale_terminal_runs(tmp_path):
    # AG-14：启动清扫终态超保留窗口的 run —— pzt delete 项目 + 删 JSON/文件；
    # 未超龄的终态 run 保留。
    import os as _os
    from session_fakes import FakeClock
    clock = FakeClock()
    _prefill_run(tmp_path, "tg-stale", RunStatus.DONE)
    _prefill_run(tmp_path, "tg-fresh", RunStatus.DONE)
    runs = tmp_path / "runs"
    _os.utime(runs / "tg-stale.json", (clock() - 10 * 86400, clock() - 10 * 86400))
    _os.utime(runs / "tg-fresh.json", (clock() - 1 * 86400, clock() - 1 * 86400))
    (tmp_path / "incoming" / "tg-stale").mkdir(parents=True, exist_ok=True)
    env = make_consumer(tmp_path, clock=clock, terminal_retention_seconds=7 * 86400)

    env.consumer.bootstrap()

    assert ("delete", "tg-stale", "--force") in env.client.calls  # 项目回收
    assert not (runs / "tg-stale.json").exists()                  # JSON 清掉
    assert not (tmp_path / "incoming" / "tg-stale").exists()      # 文件清掉
    assert (runs / "tg-fresh.json").exists()                      # 未超龄保留
    assert all(c[:2] != ("delete", "tg-fresh") for c in env.client.calls)


def test_bootstrap_self_heals_multiple_active_runs(tmp_path):
    # AG-12：取消/崩溃竞态留下多个非终态 run，bootstrap 不再 assert 拒绝启动，
    # 保留 last_activity_at 最新的、其余 cancel 落盘。
    older = _prefill_run(tmp_path, "tg-old", RunStatus.RUNNING)
    newer = _prefill_run(tmp_path, "tg-new", RunStatus.RUNNING)
    store = RunStore(tmp_path / "runs")
    older.last_activity_at = 100.0
    newer.last_activity_at = 200.0
    store.save(older)
    store.save(newer)
    env = make_consumer(tmp_path)

    env.consumer.bootstrap()  # 不抛 AssertionError

    assert store.load("tg-old").status == RunStatus.CANCELLED  # 较旧的被取消
    [job] = env.drain_jobs()
    assert job.action == "resume" and job.run_id == "tg-new"  # 最新的续跑


def test_bootstrap_does_not_revive_a_cancelling_run(tmp_path):
    # AG-12：用户明确取消过、worker 崩在收尾前的 RUNNING run，bootstrap 补
    # cancel、不复活。
    _prefill_run(tmp_path, "tg-cxl", RunStatus.RUNNING)
    store = RunStore(tmp_path / "runs")
    store.mark_cancelling("tg-cxl")
    env = make_consumer(tmp_path)

    env.consumer.bootstrap()

    assert store.load("tg-cxl").status == RunStatus.CANCELLED
    assert "上次处理被中断，正在接着跑…" not in env.transport.texts()
    assert env.drain_jobs() == []  # 不 resume
    assert store.is_cancelling("tg-cxl") is False  # 标记清掉


def test_bootstrap_approves_leftover_awaiting_review(tmp_path):
    _prefill_run(tmp_path, "tg-boot3", RunStatus.AWAITING_REVIEW)
    env = make_consumer(tmp_path)

    env.consumer.bootstrap()

    assert env.store.load("tg-boot3").status == RunStatus.DONE
    assert env.store.list_active() == []
    assert env.consumer.run is None
