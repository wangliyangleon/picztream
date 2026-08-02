"""AI 开跑前的开销告知（票 10）。

PRD G5"用户可以在 AI 开跑前拒绝"此前只在 core 里可达：headless 那一侧
两个钩子都传 nullptr，真机上 `pzt curate --ai` 不问任何人就开始花钱。

拍板决策一把 headless 的闸门从"阻塞式确认"改成"告知 + 随时可撤"：core
报完精确开销就继续跑，consumer 收到之后立刻发一条独立消息并给可取消入
口。因此这里的每一条断言都在守同一件事 —— **这条消息要早、要准、要能
停**。
"""
from __future__ import annotations

from session.protocol import StageCost, StageProgress, StageStarted
from session.view import describe_ai_cost
from session_fakes import FakeClock, make_consumer, to_running


def _running_env(tmp_path, interval: float = 60.0):
    clock = FakeClock()
    env = make_consumer(tmp_path, clock=clock, progress_interval_seconds=interval)
    job = to_running(env)
    env.put_event(StageStarted(env.consumer.generation, job.run_id, "Dedup"))
    env.consumer.step()
    return env, job


# -- 措辞（纯函数，表驱动）--


def test_cost_wording_covers_comparisons_only():
    text = describe_ai_cost(comparisons=18, evaluations=0, provider="local", first=True)
    assert "18 次" in text


def test_cost_wording_covers_evaluations_only():
    text = describe_ai_cost(comparisons=0, evaluations=6, provider="local", first=True)
    assert "6 张" in text
    assert "次" not in text  # 一次比较都不跑时不该提比较


def test_cost_wording_covers_both_at_once():
    # curate 有簇要跑锦标赛时，比较和评估在同一次闸门里一起报（core 侧
    # gate_consulted 保证只问一次）。
    text = describe_ai_cost(comparisons=9, evaluations=6, provider="local", first=True)
    assert "9 次" in text and "6 张" in text


def test_local_provider_gets_a_duration_estimate():
    # 本地模型一次约 40 秒是拍板时记下的真机量级；18 次 = 12 分钟。
    text = describe_ai_cost(comparisons=18, evaluations=0, provider="local", first=True)
    assert "12 分钟" in text


def test_cloud_provider_reports_counts_but_invents_no_duration():
    """云端每次调用的耗时没有实测数据。在一条"要花多少"的消息里编一个数
    字，是让用户从此不再信这条消息的最快办法 —— 次数照报，时长省掉。"""
    text = describe_ai_cost(comparisons=18, evaluations=0, provider="gemini", first=True)
    assert "18 次" in text
    assert "分钟" not in text


def test_second_message_reads_as_a_continuation():
    """真机上 Dedup 先报一次、Curate 再报一次。第二条照抄第一条的措辞会
    读成"怎么又要跑一遍"，而它其实是同一笔账的后半段（决策五）。"""
    first = describe_ai_cost(comparisons=18, evaluations=0, provider="local", first=True)
    second = describe_ai_cost(comparisons=0, evaluations=6, provider="local", first=False)
    assert first != second
    assert "接着" in second or "还要" in second


def test_zero_cost_says_nothing():
    # core 在没有任何 AI 调用要发时不会报开销，真报了也不该翻译成一句
    # "接下来要跑 0 次"。
    assert describe_ai_cost(comparisons=0, evaluations=0, provider="local", first=True) is None


# -- consumer 接线 --


def test_stage_cost_is_announced_immediately(tmp_path):
    env, job = _running_env(tmp_path)

    env.put_event(StageCost(env.consumer.generation, job.run_id, "Dedup", 18, 0))
    env.consumer.step()

    assert "18 次" in env.transport.sent_buttons[-1][1]


def test_cost_message_carries_a_cancel_entry_point(tmp_path):
    # 决策一的另一半：没有可取消入口的话这条消息只是通知，G5 仍然不可达。
    env, job = _running_env(tmp_path)

    env.put_event(StageCost(env.consumer.generation, job.run_id, "Dedup", 18, 0))
    env.consumer.step()

    assert env.transport.button_tokens() == ["stop"]


def test_cancel_button_on_the_cost_message_really_stops_the_run(tmp_path):
    # Dedup/Curate 都在 worker.KILLABLE_STAGES 里，取消通路是现成的。
    env, job = _running_env(tmp_path)
    env.put_event(StageCost(env.consumer.generation, job.run_id, "Dedup", 18, 0))
    env.consumer.step()

    # 点"停下"先走既有的二次确认（取消会炸掉整批，误点代价太大），确认了
    # 才真的置位 cancel_event。
    env.push_callback(f"stop:{job.run_id}")
    env.consumer.step()
    assert env.transport.button_tokens() == ["confirm_cancel", "keep"]

    env.push_callback(f"confirm_cancel:{job.run_id}")
    env.consumer.step()

    assert job.cancel_event.is_set()


def test_two_stages_report_two_messages_and_the_second_continues(tmp_path):
    env, job = _running_env(tmp_path)
    gen = env.consumer.generation

    env.put_event(StageCost(gen, job.run_id, "Dedup", 18, 0))
    env.consumer.step()
    first = env.transport.sent_buttons[-1][1]

    env.put_event(StageStarted(gen, job.run_id, "Curate"))
    env.consumer.step()
    env.put_event(StageCost(gen, job.run_id, "Curate", 0, 6))
    env.consumer.step()
    second = env.transport.sent_buttons[-1][1]

    assert "18 次" in first
    assert "6 张" in second
    assert "接着" in second or "还要" in second


def test_stale_generation_cost_is_dropped(tmp_path):
    # 上一代的开销漏进来会让用户在新会话里收到一条凭空的账单。
    env, job = _running_env(tmp_path)
    before = len(env.transport.sent_buttons)

    env.put_event(StageCost(env.consumer.generation - 1, job.run_id, "Dedup", 18, 0))
    env.consumer.step()

    assert len(env.transport.sent_buttons) == before


def test_cost_message_does_not_clobber_the_progress_slot(tmp_path):
    """开销是独立一条新消息，不能占用进度那个原地编辑的槽 —— 占了的话，
    接下来第一条进度会把这条账单改写掉，用户翻回去看不到自己被告知过。"""
    env, job = _running_env(tmp_path, interval=600.0)
    gen = env.consumer.generation

    env.put_event(StageCost(gen, job.run_id, "Dedup", 18, 0))
    env.consumer.step()
    cost_text = env.transport.sent_buttons[-1][1]

    env.put_event(StageProgress(gen, job.run_id, "Dedup", 1, 18, "comparisons"))
    env.consumer.step()

    assert "18 次" in cost_text
    assert env.consumer._stage_progress is not None
    assert "1/18次" in env.transport.texts()[-1]


# -- 取消回执的诚实（票 10 决策二/四）--


def test_cancel_receipt_during_evaluation_says_the_records_stay(tmp_path):
    """票 05 定的语义：curate 的评估逐张写库，喊停时已评估完的那几张留在
    库里。这不是遗漏 —— 每条记录本身完整，留着正好被下次运行的缓存判据命
    中，回滚等于下次再花一次钱。用户话术必须如实反映，只说"已取消"等于让
    用户以为那几次调用白花了。"""
    from orchestrator.types import RunStatus
    from session.protocol import RunFinished

    env, job = _running_env(tmp_path)
    env.consumer._do_cancel()
    env.put_event(RunFinished(0, job.run_id, RunStatus.CANCELLED.value,
                               cancelled_partial=("Curate", 4, 12, "evaluations")))
    env.consumer.step()

    text = env.transport.texts()[-1]
    assert "4/12" in text
    assert "留" in text


def test_cancel_receipt_during_comparison_claims_nothing_was_written(tmp_path):
    """比较阶段（锦标赛）的写库统一在最后一步，取消确实是零写入。这里报
    "已经处理了 N 次"会是主动误导 —— 反方向的谎同样是谎。"""
    from orchestrator.types import RunStatus
    from session.protocol import RunFinished

    env, job = _running_env(tmp_path)
    env.consumer._do_cancel()
    env.put_event(RunFinished(0, job.run_id, RunStatus.CANCELLED.value, cancelled_partial=None))
    env.consumer.step()

    assert env.transport.texts()[-1] == "已取消"


def test_cancel_receipt_for_styling_is_unchanged(tmp_path):
    # 票 05/T-8 已有的那条一个字不动。
    from orchestrator.types import RunStatus
    from session.protocol import RunFinished

    env, job = _running_env(tmp_path)
    env.consumer._do_cancel()
    env.put_event(RunFinished(0, job.run_id, RunStatus.CANCELLED.value,
                               cancelled_partial=("StyleApplyAll", 3, 10, "photos")))
    env.consumer.step()

    assert env.transport.texts()[-1] == "已取消（已经给 3/10 张套上滤镜了，这部分保留）"
