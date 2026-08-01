"""dedup/curate 把跨进程进度接到 ctx.on_progress 上（T-8 A.5）。

这是 A.1/A.3（cli 往 stderr 写）和 B.1a（agent 内的进度事件链路）之间
最后一段接线。转发规则见 stages/progress.py 的 docstring。
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
    "images": {"images": [{"path": "a.jpg", "tags": []}]},
}


class _ProgressingClient:
    """在 call() 里回放几行进度，模拟子进程边跑边吐。真正的解析在
    tests/test_pzt_client_progress.py 验过，这里只验接线。"""

    def __init__(self, emit: List[Tuple[str, int, int]]) -> None:
        self.emit = emit
        self.progress_sink = None
        self.sink_during: Dict[str, bool] = {}

    def call(self, *args: str) -> dict:
        self.sink_during[args[0]] = self.progress_sink is not None
        if args[0] in ("dedup", "curate") and self.progress_sink is not None:
            for row in self.emit:
                self.progress_sink(*row)
        return dict(_RESPONSES[args[0]])


def _ctx(seen):
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})
    ctx.on_progress = lambda done, total, kind: seen.append((done, total, kind))
    return ctx


_BOTH_PHASES = [("cluster", 1, 4), ("cluster", 4, 4), ("compare", 1, 9), ("compare", 5, 9)]


def test_dedup_without_ai_forwards_the_clustering_phase():
    seen = []
    client = _ProgressingClient(_BOTH_PHASES)

    DedupStage(client=client).run(_ctx(seen), {"ai_enabled": False})

    assert seen == [(1, 4, "groups"), (4, 4, "groups")]


def test_dedup_with_ai_forwards_the_comparison_phase():
    # 开 AI 时耗时几乎全在比较上，分簇那一段相对是一瞬。
    seen = []
    client = _ProgressingClient(_BOTH_PHASES)

    DedupStage(client=client).run(_ctx(seen), {"ai_enabled": True, "provider": "local"})

    assert seen == [(1, 9, "comparisons"), (5, 9, "comparisons")]


def test_curate_forwards_progress_the_same_way():
    seen = []
    client = _ProgressingClient(_BOTH_PHASES)

    CurateStage(client=client).run(_ctx(seen), {"count": 2, "apply_tag": "精选",
                                                 "ai_enabled": True, "provider": "local"})

    assert seen == [(1, 9, "comparisons"), (5, 9, "comparisons")]


# -- 票 05 之后 curate 多了第三个 phase：逐张评估预选集（票 09）--

_CURATE_AI_PHASES = [("cluster", 4, 4), ("compare", 9, 9), ("evaluate", 1, 6), ("evaluate", 6, 6)]


def test_curate_with_ai_forwards_both_the_comparison_and_the_evaluation_phase():
    # 只转发一个的话评估那段（分钟级、逐张一次视觉调用）整段静默，而它正
    # 是票 09 要解决的那段沉默。
    seen = []
    client = _ProgressingClient(_CURATE_AI_PHASES)

    CurateStage(client=client).run(_ctx(seen), {"count": 2, "apply_tag": "精选",
                                                 "ai_enabled": True, "provider": "local"})

    assert seen == [(9, 9, "comparisons"), (1, 6, "evaluations"), (6, 6, "evaluations")]


def test_evaluation_phase_is_not_forwarded_when_ai_is_off():
    # 关 AI 时压根不评估，真出现这个 phase 也是上游的错，不该当进度播。
    seen = []
    client = _ProgressingClient(_CURATE_AI_PHASES)

    CurateStage(client=client).run(_ctx(seen), {"count": 2, "apply_tag": "精选",
                                                 "ai_enabled": False})

    assert seen == [(4, 4, "groups")]


def test_dedup_is_unaffected_by_the_new_evaluation_phase():
    # 转发规则按 phase 分派、不按 stage 分（模块 docstring：不让"该转发哪
    # 个 phase"变成第三份实现）。dedup 从不发 evaluate，所以它实际看到的
    # 东西一字未变 —— 这里喂的就是 dedup 真会发的那两个 phase。
    seen = []
    client = _ProgressingClient(_BOTH_PHASES)

    DedupStage(client=client).run(_ctx(seen), {"ai_enabled": True, "provider": "local"})

    assert seen == [(1, 9, "comparisons"), (5, 9, "comparisons")]


def test_sink_is_detached_after_the_call():
    # 留着的话，后面那几次 tag apply 的 stderr 也会被当进度解析。更要紧
    # 的是 ctx 是这一次 run 的，跨 stage 泄漏出去就是错的上报。
    client = _ProgressingClient(_BOTH_PHASES)

    DedupStage(client=client).run(_ctx([]), {"ai_enabled": False})

    assert client.progress_sink is None


def test_curate_does_not_arm_the_sink_for_tag_calls():
    # curate 跑完还要 tag clear + N 次 tag apply，那几次是毫秒级、没有进
    # 度可言，不该顶着 sink 跑。
    client = _ProgressingClient(_BOTH_PHASES)

    CurateStage(client=client).run(_ctx([]), {"count": 2, "apply_tag": "精选"})

    assert client.sink_during["curate"] is True
    assert client.sink_during["tag"] is False


def test_curate_passthrough_needs_no_sink():
    # count=None 不调 pzt curate，只有一次 images 查询。
    client = _ProgressingClient(_BOTH_PHASES)

    CurateStage(client=client).run(_ctx([]), {"count": None, "apply_tag": "精选"})

    assert client.sink_during["images"] is False
