"""dedup/curate 把跨进程开销接到 ctx.on_cost 上（票 10）。

跟进度共用 stages/progress.forwarding 那一个上下文管理器：两条布防在同
一个地方挂、同一个地方摘，不再各写一份（模块 docstring 里那条"不让某件
事变成第三份实现"的规矩）。

这里只验接线，解析在 tests/test_pzt_client_cost.py 验过。
"""
from __future__ import annotations

from typing import Any, Dict, List, Tuple

from orchestrator.stage import StageContext
from stages.curate import CurateStage
from stages.dedup import DedupStage

_RESPONSES: Dict[str, Any] = {
    "dedup": {"groups": 2, "tagged": 2, "skipped_no_capture_time": 0},
    "curate": {"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"]},
    "tag": {},
    "images": {"images": [{"path": "a.jpg", "tags": [], "system_tags": []}]},
}


class _CostingClient:
    """在 call() 里回放一行开销，模拟 core 在第一次视觉调用之前报的那次。"""

    def __init__(self, emit: List[Tuple[int, int]]) -> None:
        self.emit = emit
        self.progress_sink = None
        self.cost_sink = None
        self.sink_during: Dict[str, bool] = {}

    def call(self, *args: str) -> dict:
        self.sink_during[args[0]] = self.cost_sink is not None
        if args[0] in ("dedup", "curate") and self.cost_sink is not None:
            for row in self.emit:
                self.cost_sink(*row)
        return dict(_RESPONSES[args[0]])


def _ctx(seen):
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})
    ctx.on_cost = lambda comparisons, evaluations: seen.append((comparisons, evaluations))
    return ctx


def test_dedup_with_ai_forwards_the_cost():
    seen = []
    client = _CostingClient([(18, 0)])

    DedupStage(client=client).run(_ctx(seen), {"ai_enabled": True, "provider": "local"})

    assert seen == [(18, 0)]


def test_curate_with_ai_forwards_the_cost():
    seen = []
    client = _CostingClient([(9, 6)])

    CurateStage(client=client).run(_ctx(seen), {"count": 2, "apply_tag": "精选",
                                                 "ai_enabled": True, "provider": "local"})

    assert seen == [(9, 6)]


def test_no_cost_sink_is_armed_when_ai_is_off():
    """关 AI 时 core 压根不会报开销（`cluster_and_choose` 的闸门在
    `ai_enabled` 里面）。这里连 sink 都不挂，是第二道保险：真有一行漏出
    来也不会变成一句"这一步要花钱"的假话。"""
    seen = []
    client = _CostingClient([(18, 0)])

    DedupStage(client=client).run(_ctx(seen), {"ai_enabled": False})

    assert seen == []
    assert client.sink_during["dedup"] is False


def test_curate_with_ai_off_arms_no_cost_sink_either():
    seen = []
    client = _CostingClient([(9, 6)])

    CurateStage(client=client).run(_ctx(seen), {"count": 2, "apply_tag": "精选",
                                                 "ai_enabled": False})

    assert seen == []
    assert client.sink_during["curate"] is False


def test_cost_sink_is_detached_after_the_call():
    # 同 progress_sink：留着的话 curate 之后那几次 tag apply 也顶着 sink
    # 跑，而 ctx 是这一次 run 的，跨 stage 泄漏就是错的上报。
    client = _CostingClient([(9, 6)])

    CurateStage(client=client).run(_ctx([]), {"count": 2, "apply_tag": "精选",
                                               "ai_enabled": True, "provider": "local"})

    assert client.cost_sink is None
    assert client.sink_during["tag"] is False


def test_stage_can_run_with_no_cost_sink_attached():
    # run_watchfolder/run_intent 和既有单测都不挂 sink，默认必须是能调的
    # no-op（同 ctx.on_progress）。
    client = _CostingClient([(18, 0)])
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    out = DedupStage(client=client).run(ctx, {"ai_enabled": True, "provider": "local"})

    assert out.ok
