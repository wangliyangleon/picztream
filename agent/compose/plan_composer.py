"""意图到 Plan 组装的那"一次性"LLM 步骤，见 docs/M4_Agent_Workflow_Design.md
五"意图到 Plan 组装"。LLM 只决定 Curate/Style 用到的参数（W2026-07-21：
Evaluate stage 已删除，agent 不再整批跑评估，见
docs/W2026-07-21_PRD.md 已拍板决策 4）：Ingest/Deliver 的文件夹路径由调
用方(run_intent.py)在 compose_plan 返回之后另行填入，模型不该、也没有
信息去编文件路径。
profile/last_config 是 docs/M4_Eng_Design.md 四已锁定的签名，子增量 E
不实现 Profile/上次配置(见本模块对应的实现计划 Global Constraints)，
接收但不用。
"""
from __future__ import annotations

from typing import Optional

from compose.llm_client import HttpPostFn, request_json
from orchestrator.types import Plan, StageSpec

_SCHEMA_INSTRUCTION = (
    "You are translating a user's free-text photo-culling request into structured "
    "pipeline parameters. Respond with a single JSON object with exactly these fields: "
    '"provider" (string, one of "local", "gemini", or "claude", pick "local" unless the '
    'user explicitly asks for a cloud provider by name), "ai_enabled" (boolean, whether '
    "to use AI visual comparison to pick the best photo among similar/duplicate ones "
    "instead of just picking by capture time or diversity -- true only if the user "
    'explicitly asks for AI-assisted picking, e.g. "AI帮我选"/"挑最好的"/"用AI选", default '
    'false), "dedup_requested" (boolean, whether the user explicitly asked to remove '
    'duplicate/near-duplicate photos, e.g. "去重"/"去除重复的"/"删掉重复的照片", default '
    'false), "count" (integer or null, how many final photos the user wants -- only put '
    'a number here if the user actually named a target count; if the user only asked to '
    'dedup without naming a count, this MUST be null (do not default it to 9 -- a null '
    'here is what defers the "how many" decision until after dedup runs); if the user '
    "named neither a count nor dedup, null is also fine (a default applies downstream)), "
    '"apply_tag" (string, the tag name to apply to the '
    "selected photos. Derive it from the destination or audience the user mentions, "
    "using that as the tag name itself: "
    '"发朋友圈"/"发朋友圈的" -> "朋友圈", "发到ins"/"发instagram" -> "ins", '
    '"给我妈看" -> "家人", "选几张精修" -> "精修". Only fall back to the default "精选" when '
    "the user names no destination, audience, album, or tag at all)."
)


def compose_plan(intent: str, profile: Optional[str], last_config: Optional[Plan],
                  http_post: Optional[HttpPostFn] = None, meta_provider: str = "local") -> Plan:
    del profile, last_config  # 子增量 E 未实现，签名锁定见本文件顶部说明
    decision = request_json(
        user_prompt=intent,
        schema_instruction=_SCHEMA_INSTRUCTION,
        provider=meta_provider,
        http_post=http_post,
    )
    count = decision.get("count")
    dedup_requested = decision.get("dedup_requested", False)
    ai_enabled = decision.get("ai_enabled", False)
    provider = decision.get("provider", "local")
    apply_tag = decision.get("apply_tag", "精选")

    stages = [StageSpec(name="Ingest")]
    if count is None and dedup_requested:
        # W2026-07-21 目标三案例二：只说去重没给数量，Curate 的决定推迟到
        # Dedup 跑完之后用一个闸门问（agent/session 侧接线见 Commit 8）。
        stages.append(StageSpec(name="Dedup", params={
            "ai_enabled": ai_enabled,
            "provider": provider,
        }))
        stages.append(StageSpec(name="Curate", params={
            "count": None,
            "apply_tag": apply_tag,
            "ai_enabled": ai_enabled,
            "provider": provider,
        }, gate="required"))
    else:
        # count 给了（案例三）或什么都没给（案例一/默认 9）：core curate 的
        # 粗聚类已经隐含去重效果，单独跑一次 Dedup 是多余的（见
        # docs/W2026-07-21_Tournament_Eng_Design.md 目标三补充设计决策一）。
        stages.append(StageSpec(name="Curate", params={
            "count": count if count is not None else 9,
            "apply_tag": apply_tag,
            "ai_enabled": ai_enabled,
            "provider": provider,
        }))
    stages += [
        StageSpec(name="Style", params={"provider": provider}, gate="required"),
        StageSpec(name="StyleApplyAll", gate="required"),
        StageSpec(name="Deliver"),
    ]
    return Plan(stages=stages)
