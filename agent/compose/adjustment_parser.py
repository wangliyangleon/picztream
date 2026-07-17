"""对话调整解析，见 docs/M4_Agent_Workflow_Design.md 六"调整模型"：一次
调整由 LLM 解析成结构化配置增量。子增量 E 范围只认 Curate 一个 Stage
的三种调整("留 N 张"/"换标签"/"换掉第 N 张")。

LLM 只负责识别"用户想要哪种调整、给了什么值"这一步(有界词汇，action
只有三种)；"第 N 张"到实际 image_path 的下标解析、跟历史 exclude 列
表的合并，是纯 Python 确定性逻辑，不交给模型，模型不擅长可靠数数，这
一步错了就是发错张，必须是能单测锁死的代码路径。

`classify_gate_reply` 是子增量 F1 真机验证后加的：Gate 阶段"这句话算
同意/拒绝/调整"原本用固定关键词精确匹配，"挺好的，就这三张吧"这种自
然口语因为不精确等于词表里任何一项，会被误当成一句解析不出来的调整。
按 docs/M4_Agent_Workflow_Design.md 六原本的设计思路("明显的走廉价规
则短路，'是不是改动、改什么'交 LLM，跟调整解析同一个调用")，把同意/
拒绝也并进这一次 LLM 调用里一起判断，不额外多花一次调用。`parse_adjustment`
本身的行为/签名保持不变，两者共用下面的 `_decision_to_delta`。

`refine_plan_confirmation` 是子增量 F3 加的：意图解析成 Plan 之后先给
用户回显确认(PLANNED)，不直接开跑。用户的确认回复可能是精确关键词(交
给 SessionRouter 自己判断)，也可能是一句自然语言修正("改成6张")或者
太含糊的否定("不对")。这一个 LLM 调用把"原始意图 + 已提议参数 + 用户
回复"一起丢给模型，让模型自己判断：能不能安全地合并出一份新参数(这时
候不吭声再问一遍，直接照着改)，还是含糊到没法安全动手、得反问一句更
具体的("具体想改哪一项？")。跟 classify_gate_reply 是完全一样的设计
思路(廉价关键词短路留给调用方，LLM 只处理"不确定"的情况)，复用同一个
AdjustmentError。
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import List, Literal, Optional

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

_GATE_SCHEMA_INSTRUCTION = (
    "You are classifying a user's reply about a batch of photos they were just shown "
    "for review, right before final delivery. Respond with a single JSON object "
    "describing exactly one of these six shapes: "
    '{"action": "approve"} if the user is satisfied and wants to proceed with delivery, '
    'even if phrased casually or indirectly (for example "挺好的，就这三张吧", "可以了", '
    '"没问题", "just send it"); '
    '{"action": "reject"} if the user wants to cancel/abandon this batch entirely '
    '(for example "算了", "不要了", "cancel this"); '
    '{"action": "set_count", "count": <integer>} if the user wants a different total '
    'number of photos; {"action": "set_apply_tag", "apply_tag": <string>} if the user '
    'wants the selected photos tagged with a different tag, album, or audience name; '
    '{"action": "swap_out", "index": <integer>} if the user wants to replace one '
    "specific photo, identified by its 1-based position in the previously shown list "
    '(for example "换掉第3张" or "swap out photo #3" means index=3); '
    '{"action": "query"} if the message is just a question about the current status '
    '(for example "选了几张呀？", "how many did you pick?"), not an instruction to '
    "change anything or proceed."
)

_ADJUST_ACTIONS = ("set_count", "set_apply_tag", "swap_out")


class AdjustmentError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


@dataclass
class GateReply:
    action: Literal["approve", "reject", "adjust", "query"]
    delta: Optional[PlanDelta] = None


def parse_adjustment(msg: str, run: RunState, http_post: Optional[HttpPostFn] = None,
                      meta_provider: str = "local") -> PlanDelta:
    decision = request_json(
        user_prompt=msg,
        schema_instruction=_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action not in _ADJUST_ACTIONS:
        raise AdjustmentError("unknown_action", f"unrecognized adjustment action {action!r}")
    return _decision_to_delta(action, decision, run)


def classify_gate_reply(msg: str, run: RunState, http_post: Optional[HttpPostFn] = None,
                         meta_provider: str = "local") -> GateReply:
    decision = request_json(
        user_prompt=msg,
        schema_instruction=_GATE_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")

    if action == "approve":
        return GateReply(action="approve")

    if action == "reject":
        return GateReply(action="reject")

    if action == "query":
        return GateReply(action="query")

    if action in _ADJUST_ACTIONS:
        return GateReply(action="adjust", delta=_decision_to_delta(action, decision, run))

    raise AdjustmentError("unknown_action", f"unrecognized gate reply action {action!r}")


def _decision_to_delta(action: str, decision: dict, run: RunState) -> PlanDelta:
    if action == "set_count":
        return PlanDelta(stage_name="Curate", params={"count": decision["count"]})

    if action == "set_apply_tag":
        return PlanDelta(stage_name="Curate", params={"apply_tag": decision["apply_tag"]})

    return _resolve_swap_out(decision["index"], run)


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


_CONFIRMATION_SCHEMA_INSTRUCTION = (
    "You proposed a photo-culling plan and asked the user to confirm it. You are given "
    "the user's original intent, the plan you proposed (as JSON), and the user's reply. "
    "Respond with a single JSON object in one of five shapes: "
    '{"action": "approve"} if the user is satisfied and wants to proceed, even if '
    'phrased casually or indirectly (for example "好的，处理吧", "可以", "没问题"); '
    '{"action": "reject"} if the user wants to abandon this plan entirely (for example '
    '"算了", "不要了", "cancel"); '
    '{"action": "query"} if the message is just a question about the current status '
    '(for example "你收到几张图片了？", "现在的方案是什么？"), not an instruction to '
    "change or approve anything; "
    '{"action": "confirmed", "provider": <string>, "auto_reject": <boolean>, '
    '"count": <integer>, "apply_tag": <string>} if the reply lets you determine a '
    "complete updated plan (for any field the user didn't ask to change, keep the same "
    "value as the plan you proposed); "
    '{"action": "clarify", "question": <string>} if the reply seems to want a change but '
    'is too vague to safely act on (for example just "不对"/"no" with no specifics) - '
    "write a short, specific follow-up question to ask the user in that case."
)


@dataclass
class PlanConfirmationReply:
    action: Literal["confirmed", "clarify", "query", "approve", "reject"]
    provider: Optional[str] = None
    auto_reject: Optional[bool] = None
    count: Optional[int] = None
    apply_tag: Optional[str] = None
    question: Optional[str] = None


def refine_plan_confirmation(original_intent: str, current_params: dict, followup_message: str,
                              http_post: Optional[HttpPostFn] = None,
                              meta_provider: str = "local") -> PlanConfirmationReply:
    user_prompt = (
        f"原始意图：{original_intent}\n"
        f"你之前提出的方案：{json.dumps(current_params, ensure_ascii=False)}\n"
        f"用户的回复：{followup_message}"
    )
    decision = request_json(
        user_prompt=user_prompt,
        schema_instruction=_CONFIRMATION_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")

    if action == "approve":
        return PlanConfirmationReply(action="approve")

    if action == "reject":
        return PlanConfirmationReply(action="reject")

    if action == "query":
        return PlanConfirmationReply(action="query")

    if action == "clarify":
        return PlanConfirmationReply(action="clarify", question=decision.get("question", "能再说清楚一点吗？"))

    if action == "confirmed":
        return PlanConfirmationReply(
            action="confirmed",
            provider=decision.get("provider", current_params.get("provider")),
            auto_reject=decision.get("auto_reject", current_params.get("auto_reject")),
            count=decision.get("count", current_params.get("count")),
            apply_tag=decision.get("apply_tag", current_params.get("apply_tag")),
        )

    raise AdjustmentError("unknown_action", f"unrecognized confirmation action {action!r}")


_COLLECTING_SCHEMA_INSTRUCTION = (
    "The user has been sending photos to a photo-culling bot and hasn't yet given a "
    "processing instruction. You are given how many photos have been received so far "
    "and the user's message. Respond with a single JSON object in one of four shapes: "
    '{"action": "intent"} if the message is an actual instruction describing how to '
    'process/cull/select the photos (for example "帮我选几张发朋友圈", "筛一下", '
    '"挑5张精修", "把糊的去掉"); '
    '{"action": "query"} if the message is just a question about the current status '
    '(for example "收到几张了？", "how many photos so far?"), not an instruction; '
    '{"action": "cancel"} if the message expresses wanting to abort/stop/cancel this '
    'batch (for example "取消", "算了不弄了", "别处理了", "停一下", "不想要了"); '
    '{"action": "other"} if the message is NOT related to photo culling at all - a '
    "greeting, small talk, an unrelated question, or gibberish (for example \"你好\", "
    '"今天天气不错", "你是谁", "asdfgh"). Do NOT guess an "intent" for messages that '
    "are not clearly photo-processing instructions; use \"other\" instead."
)


@dataclass
class CollectingReply:
    action: Literal["query", "intent", "cancel", "other"]


def classify_collecting_message(text: str, photo_count: int, http_post: Optional[HttpPostFn] = None,
                                 meta_provider: str = "local") -> CollectingReply:
    user_prompt = f"目前已收到 {photo_count} 张照片。用户消息：{text}"
    decision = request_json(
        user_prompt=user_prompt,
        schema_instruction=_COLLECTING_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action not in ("query", "intent", "cancel", "other"):
        raise AdjustmentError("unknown_action", f"unrecognized collecting message action {action!r}")
    return CollectingReply(action=action)
