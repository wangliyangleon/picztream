"""对话调整解析，见 docs/M4_Agent_Workflow_Design.md 六"调整模型"：一次
调整由 LLM 解析成结构化配置增量。子增量 E 范围只认 Curate 一个 Stage
的三种调整("留 N 张"/"换标签"/"换掉第 N 张")。

LLM 只负责识别"用户想要哪种调整、给了什么值"这一步(有界词汇，action
只有三种)；"第 N 张"到实际 image_path 的下标解析、跟历史 exclude 列
表的合并，是纯 Python 确定性逻辑，不交给模型，模型不擅长可靠数数，这
一步错了就是发错张，必须是能单测锁死的代码路径。
"""
from __future__ import annotations

from typing import List, Optional

from compose.llm_client import HttpPostFn, request_json
from orchestrator.types import PlanDelta, RunState

_SCHEMA_INSTRUCTION = (
    "You are classifying a user's follow-up adjustment message about a batch of "
    "selected photos. Respond with a single JSON object describing exactly one "
    "adjustment, using one of these three shapes: "
    '{"action": "set_count", "count": <integer>} if the user wants a different total '
    'number of photos; {"action": "set_apply_tag", "apply_tag": <string>} if the user '
    'wants the selected photos tagged with a different tag, album, or audience name; '
    '{"action": "swap_out", "index": <integer>} if the user wants to replace one '
    "specific photo, identified by its 1-based position in the previously shown list "
    '(for example "换掉第3张" or "swap out photo #3" means index=3).'
)


class AdjustmentError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def parse_adjustment(msg: str, run: RunState, http_post: Optional[HttpPostFn] = None,
                      meta_provider: str = "gemini") -> PlanDelta:
    decision = request_json(
        user_prompt=msg,
        schema_instruction=_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")

    if action == "set_count":
        return PlanDelta(stage_name="Curate", params={"count": decision["count"]})

    if action == "set_apply_tag":
        return PlanDelta(stage_name="Curate", params={"apply_tag": decision["apply_tag"]})

    if action == "swap_out":
        return _resolve_swap_out(decision["index"], run)

    raise AdjustmentError("unknown_action", f"unrecognized adjustment action {action!r}")


def _resolve_swap_out(index: int, run: RunState) -> PlanDelta:
    curate_output = run.outputs.get("Curate")
    selected: List[str] = curate_output.data.get("selected", []) if curate_output else []
    if not isinstance(index, int) or index < 1 or index > len(selected):
        raise AdjustmentError(
            "index_out_of_range",
            f"'第{index}张' is out of range: Curate only selected {len(selected)} photos",
        )
    target_path = selected[index - 1]

    curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
    current_exclude = curate_spec.params.get("exclude", [])
    merged_exclude = list(current_exclude)
    if target_path not in merged_exclude:
        merged_exclude.append(target_path)

    return PlanDelta(stage_name="Curate", params={"exclude": merged_exclude})
