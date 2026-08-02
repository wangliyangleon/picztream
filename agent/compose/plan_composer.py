"""意图到 Plan 组装的那"一次性"LLM 步骤，见 docs/history/M4_Agent_Workflow_Design.md
五"意图到 Plan 组装"。LLM 只决定 Curate/Style 用到的参数（W2026-07-21：
Evaluate stage 已删除，agent 不再整批跑评估，见
docs/history/W2026-07-21_PRD.md 已拍板决策 4）：Ingest/Deliver 的文件夹路径由调
用方(run_intent.py)在 compose_plan 返回之后另行填入，模型不该、也没有
信息去编文件路径。
profile/last_config 是 docs/history/M4_Eng_Design.md 四已锁定的签名，子增量 E
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
    'a number here if the user actually named a target count, no matter which verb they '
    'used to describe narrowing down: "选5张"/"留5张"/"挑5张"/"筛5张"/"筛选5张"/"筛出5张" '
    'all mean count=5, and Chinese numerals count too: "选三张"/"留两张" mean count=3 and '
    "count=2; even when combined with a dedup request in the same sentence (e.g. "
    '"去重筛两张"/"去重然后筛选两张"/"去重留两张" all mean dedup_requested=true AND count=2 '
    "-- do not let the dedup mention make you drop the number); if the user only asked to "
    'dedup without naming any count at all, count MUST be null (do not default it to 9 -- '
    'a null here is what defers the "how many" decision until after dedup runs); if the '
    "user named neither a count nor dedup, null is also fine (a default applies downstream)), "
    '"apply_tag" (string, the tag name to apply to the '
    "selected photos. Derive it from the destination or audience the user mentions, "
    "using that as the tag name itself: "
    '"发朋友圈"/"发朋友圈的" -> "朋友圈", "发到ins"/"发instagram" -> "ins", '
    '"给我妈看" -> "家人", "选几张精修" -> "精修". Only fall back to the default "精选" when '
    'the user names no destination, audience, album, or tag at all), "selection_brief" '
    "(string, a short Chinese brief telling whoever picks the photos WHAT KIND of photos "
    "this user wants and HOW they should be ordered. Include only what affects which "
    "photos get picked and in what order: subject-matter preferences "
    '("有景有人"/"表情活泼"/"别都是风景照"/"要有小孩的"), narrative or ordering requirements '
    '("按时间顺序"/"开头结尾要呼应"), and the destination or audience if the user named one '
    '("发朋友圈"/"给我妈看"). Leave OUT everything that does not bear on that choice: '
    "deduplication requests, which AI provider to use, how many photos they want (that is "
    'already "count"), greetings and small talk. Do NOT copy the user\'s sentence verbatim '
    "- write the brief yourself, and never invent a preference the user did not state. "
    'Examples: "挑8张有小孩的、别都是背影，发到ins" -> "发 ins 用，要有小孩的，别都是背影"; '
    '"去重，然后用 gemini 留5张按时间顺序排" -> "按拍摄时间顺序排"; "选3张发朋友圈" -> '
    '"发朋友圈用" (the user named a destination but no subject preference, so the brief '
    'says only that); "帮我筛一下，留9张" -> "" (nothing was said about what kind of photos '
    "or where they go)."
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
    # `.get(k, 默认)` 只在 key 缺席时给默认值，模型显式回 null 时给的是
    # None，会被 validate_plan 拒掉、整次方案组装失败。真机上确实会发生
    # （"先去重，然后挑5张有小孩的" 这种没提目的地的说法，同一句话跑两次
    # 一次 null 一次正常），是票 08 之前就有的缺陷，随手修掉。
    apply_tag = decision.get("apply_tag") or "精选"
    # 票 08：null 与字段缺席都归一成空串（= 这次没有题材要求），不留给
    # validate_plan 去拒。模型在"用户什么偏好都没说"时回 null 是常态，为一
    # 个纯增量的字段把整次方案组装打成失败，代价与收益完全不成比例。别的
    # 形状（dict/数字）仍然原样交给 validate_plan 拦，那才是输出污染。
    selection_brief = decision.get("selection_brief") or ""

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
            "selection_brief": selection_brief,
        }, gate="required"))
    else:
        # count 给了（案例三）或什么都没给（案例一/默认 9）：core curate 的
        # 粗聚类已经隐含去重效果，单独跑一次 Dedup 是多余的（见
        # docs/history/W2026-07-21_Tournament_Eng_Design.md 目标三补充设计决策一）。
        stages.append(StageSpec(name="Curate", params={
            "count": count if count is not None else 9,
            "apply_tag": apply_tag,
            "ai_enabled": ai_enabled,
            "provider": provider,
            "selection_brief": selection_brief,
        }))
    stages += [
        StageSpec(name="Style", params={"provider": provider}, gate="required"),
        StageSpec(name="StyleApplyAll", gate="required"),
        StageSpec(name="Deliver"),
    ]
    return Plan(stages=stages)
