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
用户回显确认(PLANNED)，不直接开跑。用户的确认回复可能是一句直接同意，
也可能是一句自然语言修正("改成6张")或者
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
    "The adjustable fields are count (how many photos to keep), apply_tag (the "
    "tag/album/audience name), ai_enabled (boolean, whether to use AI visual comparison "
    "to pick the best photo among similar ones instead of just picking by capture time), "
    "and provider (string, one of \"local\", \"gemini\", or \"claude\", which AI model to "
    "use when ai_enabled is true). Respond with a single JSON object in one of five shapes: "
    '{"action": "approve"} if the user is satisfied and wants to proceed, even if '
    'phrased casually or indirectly (for example "好的，处理吧", "可以", "没问题"); '
    '{"action": "reject"} if the user wants to abandon this plan entirely (for example '
    '"算了", "不要了", "cancel"); '
    '{"action": "confirmed", "count": <integer>, "apply_tag": <string>, '
    '"ai_enabled": <boolean>, "provider": <string>} if the reply asks to CHANGE any field. '
    "Requests to change the number of photos, the tag, or whether AI picks are used are "
    "ALWAYS 'confirmed', never 'query'. Copy every field from the proposed plan and "
    'overwrite only what the user asked to change. Examples (proposed count=3): "选六张吧"/'
    '"改成6张"/"要6张" -> count 6; "留9张" -> count 9; "标签叫ins"/"发到ins" -> apply_tag '
    '"ins"; "6张，标签叫朋友圈" -> count 6 and apply_tag "朋友圈"; "AI帮我选"/"用AI挑"/"挑最好'
    '的" -> ai_enabled true; "不用AI了"/"按时间选就行"/"别用AI" -> ai_enabled false; "换成'
    'gemini"/"用gemini" -> provider "gemini"; '
    '{"action": "query"} ONLY if the message is purely a question that asks for '
    'information and requests NO change (for example "你收到几张图片了？", "现在留几张？"); '
    'a message that states a new number or tag is NOT a query; '
    '{"action": "clarify", "question": <string>} if the reply seems to want a change but '
    'is too vague to safely act on (for example just "不对"/"no" with no specifics) - '
    "write a short, specific follow-up question to ask the user in that case."
)


@dataclass
class PlanConfirmationReply:
    action: Literal["confirmed", "clarify", "query", "approve", "reject"]
    count: Optional[int] = None
    apply_tag: Optional[str] = None
    ai_enabled: Optional[bool] = None
    provider: Optional[str] = None
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
            count=decision.get("count", current_params.get("count")),
            apply_tag=decision.get("apply_tag", current_params.get("apply_tag")),
            ai_enabled=decision.get("ai_enabled", current_params.get("ai_enabled")),
            provider=decision.get("provider", current_params.get("provider")),
        )

    raise AdjustmentError("unknown_action", f"unrecognized confirmation action {action!r}")


_COLLECTING_SCHEMA_INSTRUCTION = (
    "The user is sending photos to a photo-culling bot and giving instructions. You are "
    "given how many photos have been received so far and the user's message. Respond "
    "with a single JSON object in one of five shapes: "
    '{"action": "intent"} if the message describes HOW to process/cull/select the photos, '
    'including messages that only ask to remove near-duplicate photos without naming a '
    'final count (for example "帮我选几张发朋友圈", "筛一下", "挑5张精修", "把糊的去掉", '
    '"帮我去个重复的照片"); '
    '{"action": "start"} if the message just says to begin now WITHOUT describing what to '
    'select or remove - a plain go-ahead, typically after they have already said what they '
    'want or finished sending photos (for example "开始吧", "好了", "就这些", "可以了", '
    '"发完了", "弄吧"); '
    '{"action": "query"} if the message is just a question about the current status '
    '(for example "收到几张了？", "how many photos so far?"), not an instruction; '
    '{"action": "cancel"} if the message expresses wanting to abort/stop/cancel this '
    'batch (for example "取消", "算了不弄了", "别处理了", "停一下", "不想要了"); '
    '{"action": "other"} if the message is NOT related to photo culling at all - a '
    "greeting, small talk, an unrelated question, or gibberish (for example \"你好\", "
    '"今天天气不错", "你是谁", "asdfgh"). Do NOT guess an "intent" for messages that '
    "are not clearly photo-processing instructions; use \"other\" instead. The \"action\" "
    "value must always be exactly one of these five English words: intent, start, query, "
    "cancel, other -- never a Chinese word or phrase, even though the user's message and "
    "the examples above are in Chinese."
)


@dataclass
class CollectingReply:
    action: Literal["query", "intent", "start", "cancel", "other"]


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
    if action not in ("query", "intent", "start", "cancel", "other"):
        raise AdjustmentError("unknown_action", f"unrecognized collecting message action {action!r}")
    return CollectingReply(action=action)


# 以下三个分类器是"彻底去掉文本精确匹配"引入的（真机反馈）：风格预览闸
# 门、处理中、取消二次确认这三处以前是关键词精确匹配，现在都交 LLM。都
# 是小而紧的 schema（贴合各自上下文的有界动作集），比通用分类更不容易
# 幻觉。按钮回调仍是确定性即时路径，不经过这些。

_STYLE_DESCRIBE_SCHEMA_INSTRUCTION = (
    "The user is being asked to describe, in free text, the photo color style/filter they "
    "want applied to their selected photos. They have NOT been shown any preview yet. "
    "You are given the user's reply. Respond with a single JSON object in one of four shapes: "
    '{"action": "describe"} if the user is describing a style they want (for example '
    '"复古暖色调", "黑白胶片", "冷一点的电影感", "日系清新", "再暖一点"); '
    '{"action": "skip"} if the user does NOT want any filter and wants the original photos '
    'as-is (for example "不要滤镜", "原图就行", "不用调色", "保持原样", "别加滤镜"); '
    '{"action": "cancel"} if the user wants to abort/cancel the whole batch (for example '
    '"取消", "算了不弄了", "不弄了", "不想要了"); '
    '{"action": "query"} if the user is just asking a question about the available styles '
    'or what to do, not making a choice (for example "有哪些风格？", "都能选什么", "这是啥"). '
    "When unsure, prefer describe."
)


@dataclass
class StyleDescribeReply:
    action: Literal["describe", "skip", "cancel", "query"]


def classify_style_describe(text: str, http_post: Optional[HttpPostFn] = None,
                            meta_provider: str = "local") -> StyleDescribeReply:
    decision = request_json(
        user_prompt=text,
        schema_instruction=_STYLE_DESCRIBE_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action not in ("describe", "skip", "cancel", "query"):
        raise AdjustmentError("unknown_action", f"unrecognized style describe action {action!r}")
    return StyleDescribeReply(action=action)


_STYLE_GATE_SCHEMA_INSTRUCTION = (
    "The user was shown one representative photo with a color style/filter applied, and "
    "asked whether this style is OK. You are given the user's reply. Respond with a "
    "single JSON object in one of four shapes: "
    '{"action": "approve"} if the user is satisfied with this style and wants to apply '
    'it to the whole batch (for example "满意", "不错", "就这个", "可以了", "挺好看的"); '
    '{"action": "redescribe"} if the user is NOT satisfied and is describing a different '
    'style they want instead (for example "再暖一点", "太冷了", "换个复古的", "黑白的"); '
    '{"action": "cancel"} if the user wants to abort/cancel the whole batch (for example '
    '"取消", "算了不弄了", "不要了"); '
    '{"action": "query"} if the message is just a question about the current status, not '
    "a decision. When unsure between approve and redescribe, prefer redescribe."
)


@dataclass
class StyleGateReply:
    action: Literal["approve", "redescribe", "cancel", "query"]


def classify_style_gate_reply(text: str, http_post: Optional[HttpPostFn] = None,
                               meta_provider: str = "local") -> StyleGateReply:
    decision = request_json(
        user_prompt=text,
        schema_instruction=_STYLE_GATE_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action not in ("approve", "redescribe", "cancel", "query"):
        raise AdjustmentError("unknown_action", f"unrecognized style gate action {action!r}")
    return StyleGateReply(action=action)


_DEDUP_FOLLOWUP_SCHEMA_INSTRUCTION = (
    "The user just finished deduplicating a batch of photos and is being asked whether "
    "they want to further narrow down to a specific number, or keep everything that "
    "survived deduplication. You are given how many photos remain after dedup and the "
    "user's reply. Respond with a single JSON object in one of four shapes: "
    '{"action": "narrow", "count": <integer>} if the user gives a target number of '
    'photos to keep (for example "留5张", "5张就行", "选3张吧"); '
    '{"action": "skip"} if the user does NOT want any further narrowing and wants to '
    'keep everything that survived dedup (for example "不用了", "够了", "都要", "不筛了", '
    '"就这些吧"); '
    '{"action": "query"} if the message is just a question about status, not a decision '
    '(for example "现在还剩几张？", "去重完了吗"); '
    '{"action": "cancel"} if the user wants to abort/cancel the whole batch (for example '
    '"算了", "不要了", "取消").'
)


@dataclass
class DedupFollowupReply:
    action: Literal["narrow", "skip", "query", "cancel"]
    count: Optional[int] = None


def classify_dedup_followup(text: str, remaining: int, http_post: Optional[HttpPostFn] = None,
                             meta_provider: str = "local") -> DedupFollowupReply:
    user_prompt = f"去重后还剩 {remaining} 张。用户回复：{text}"
    decision = request_json(
        user_prompt=user_prompt,
        schema_instruction=_DEDUP_FOLLOWUP_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action == "narrow":
        return DedupFollowupReply(action="narrow", count=decision.get("count"))
    if action in ("skip", "query", "cancel"):
        return DedupFollowupReply(action=action)
    raise AdjustmentError("unknown_action", f"unrecognized dedup followup action {action!r}")


_RUNNING_SCHEMA_INSTRUCTION = (
    "A photo-culling batch is currently being processed (evaluating/deduping/selecting). "
    "The user sent a message while it runs. Respond with a single JSON object in one of "
    "three shapes: "
    '{"action": "cancel"} if the user wants to abort/stop/cancel the running batch (for '
    'example "取消", "停一下", "别弄了", "算了", "不想要了"); '
    '{"action": "query"} if the user is asking about progress/status (for example '
    '"到哪了？", "还要多久", "怎么样了"); '
    '{"action": "other"} for anything else (small talk, unrelated, gibberish).'
)


@dataclass
class RunningReply:
    action: Literal["cancel", "query", "other"]


def classify_running_message(text: str, http_post: Optional[HttpPostFn] = None,
                             meta_provider: str = "local") -> RunningReply:
    decision = request_json(
        user_prompt=text,
        schema_instruction=_RUNNING_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action not in ("cancel", "query", "other"):
        raise AdjustmentError("unknown_action", f"unrecognized running message action {action!r}")
    return RunningReply(action=action)


_CANCEL_CONFIRM_SCHEMA_INSTRUCTION = (
    "The user asked to cancel the whole batch, and was asked to confirm (this is "
    "destructive - all photos and results in this batch will be discarded). You are "
    "given the user's reply. Respond with a single JSON object in one of three shapes: "
    '{"action": "confirm"} if the user confirms they really want to cancel (for example '
    '"确认取消", "对就取消", "嗯取消吧", "是的"); '
    '{"action": "deny"} if the user does NOT want to cancel after all / wants to continue '
    '(for example "不取消", "继续", "算了还是不取消了", "点错了"); '
    '{"action": "other"} if the reply is unrelated to this yes/no confirmation. When '
    "unsure, prefer other (never destructively cancel on an ambiguous reply)."
)


@dataclass
class CancelConfirmReply:
    action: Literal["confirm", "deny", "other"]


def classify_cancel_confirmation(text: str, http_post: Optional[HttpPostFn] = None,
                                 meta_provider: str = "local") -> CancelConfirmReply:
    decision = request_json(
        user_prompt=text,
        schema_instruction=_CANCEL_CONFIRM_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    action = decision.get("action")
    if action not in ("confirm", "deny", "other"):
        raise AdjustmentError("unknown_action", f"unrecognized cancel confirmation action {action!r}")
    return CancelConfirmReply(action=action)
