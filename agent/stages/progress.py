"""把子进程吐出来的进度接到 stage 上（T-8 A.5）。

dedup 和 curate 用同一套规则。抽出来而不是各写一份，是为了不让"该转发哪
个 phase"变成第三份实现 —— 这个仓库已经吃过 scope 解析和"排除废片"策略
在 core/cli/agent 各写一份的亏（提案 T-16 / T-25）。
"""
from __future__ import annotations

import contextlib

from orchestrator.stage import PROGRESS_COMPARISONS, PROGRESS_EVALUATIONS, PROGRESS_GROUPS

# phase -> 进度类别。开 AI 时转发的是那些分钟级的阶段：比较（每次一个受
# 60s 超时约束的视觉推理，实测本地模型约 40s/次）和票 05 起 curate 新增的
# 逐张评估。本地分簇（cluster）相对是一瞬，不转发；关 AI 时压根没有比较和
# 评估，分簇就是全部耗时，转发它。
#
# dedup 不发 evaluate，这张表对它而言就是原来那一条，行为不变。
_AI_PHASES = {"compare": PROGRESS_COMPARISONS, "evaluate": PROGRESS_EVALUATIONS}
_LOCAL_PHASES = {"cluster": PROGRESS_GROUPS}


@contextlib.contextmanager
def forwarding(client, ctx, ai_enabled: bool):
    """在作用域内把 client 的跨进程进度转成 ctx.on_progress，出去就摘掉。

    票 10 起同一个作用域里还挂开销（ctx.on_cost）：两条布防的时机、范围、
    摘除条件完全一致（都只覆盖那一次 dedup/curate 调用，不覆盖后面几次
    tag apply），拆成两个上下文管理器只会让每个调用点都写两层 with。

    布防/摘除跟 worker 挂 cancel_event 是同一个套路：挂在 client 实例上，
    stage 内部的 client.call(...) 零改动就吃得到。

    **转发多个 phase，但只转发分钟级的那些**（票 09）。这条规则此前是"一
    个 run 里只转发一个 phase"，理由是分母不同源会让进度看着倒退：用户会
    看到"已完成 17/17 张"紧接着变成"已完成 1/51 张"。票 05 之后开 AI 的
    curate 有了两个都是分钟级的阶段（比较、逐张评估），继续只转发一个等于
    让另一段整段静默，而那正是票 09 要消除的沉默。

    倒退这个顾虑没有消失，是**挪到了 consumer**：换 phase 时先把上一条进
    度收尾成终态，评估另起一条消息，两个数字不再挤在同一条里原地跳。代价
    是 Curate 期间占两条进度消息，明确偏离 AG-16.3"进度只占一条"，理由见
    docs/issues/intent-curation/09-agent-progress-rendering.md。

    仍然不转发本地分簇：开 AI 时它相对是一瞬，且 stderr 上三个 phase 都
    在，需要时查日志。
    """
    phases = _AI_PHASES if ai_enabled else _LOCAL_PHASES

    def sink(phase: str, done: int, total: int) -> None:
        kind = phases.get(phase)
        if kind is not None:
            ctx.on_progress(done, total, kind)

    def cost_sink(comparisons: int, evaluations: int) -> None:
        ctx.on_cost(comparisons, evaluations)

    client.progress_sink = sink
    # 关 AI 时压根不挂：core 那一侧的闸门本来就在 `ai_enabled` 里面、一行
    # 都不会发（`tournament::cluster_and_choose` / `curate_impl`），这里不
    # 挂是第二道保险 —— 真漏一行出来，用户收到的会是一句凭空的"这一步要
    # 花钱"，而这条路径上根本没有 AI 调用。
    client.cost_sink = cost_sink if ai_enabled else None
    try:
        yield
    finally:
        client.progress_sink = None
        client.cost_sink = None
