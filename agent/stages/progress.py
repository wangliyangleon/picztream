"""把子进程吐出来的进度接到 stage 上（T-8 A.5）。

dedup 和 curate 用同一套规则。抽出来而不是各写一份，是为了不让"该转发哪
个 phase"变成第三份实现 —— 这个仓库已经吃过 scope 解析和"排除废片"策略
在 core/cli/agent 各写一份的亏（提案 T-16 / T-25）。
"""
from __future__ import annotations

import contextlib

from orchestrator.stage import PROGRESS_COMPARISONS, PROGRESS_GROUPS


@contextlib.contextmanager
def forwarding(client, ctx, ai_enabled: bool):
    """在作用域内把 client 的跨进程进度转成 ctx.on_progress，出去就摘掉。

    布防/摘除跟 worker 挂 cancel_event 是同一个套路：挂在 client 实例上，
    stage 内部的 client.call(...) 零改动就吃得到。

    **一个 run 里只转发一个 phase。** 两个都往上送的话，用户会看到"已完
    成 17/17 张"紧接着变成"已完成 1/51 张" —— 分子分母同时跳，读起来像
    进度条倒退（PRD 风险二）。根因是两个阶段的分母不同源：本地分簇数的是
    候选簇，AI 阶段数的是比较次数。

    取哪个按"哪个阶段是分钟级的"：开 AI 时耗时几乎全在比较上（每次一个受
    60s 超时约束的视觉推理，实测本地模型约 40s/次），分簇相对是一瞬；不开
    AI 时压根没有比较阶段。代价是开 AI 且批量很大时分簇那一段仍然静默 ——
    接受，它比改造前的整段静默短得多，且 stderr 上两个 phase 都在。
    """
    wanted, kind = (("compare", PROGRESS_COMPARISONS) if ai_enabled
                    else ("cluster", PROGRESS_GROUPS))

    def sink(phase: str, done: int, total: int) -> None:
        if phase == wanted:
            ctx.on_progress(done, total, kind)

    client.progress_sink = sink
    try:
        yield
    finally:
        client.progress_sink = None
