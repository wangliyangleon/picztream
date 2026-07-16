"""意图到 Plan 组装的那"一次性"LLM 步骤，见 docs/M4_Agent_Workflow_Design.md
五"意图到 Plan 组装"。子增量 E 范围内 LLM 只决定 Evaluate/Curate 两个
Stage 的参数：Ingest/Deliver 的文件夹路径由调用方(run_intent.py)在
compose_plan 返回之后另行填入，模型不该、也没有信息去编文件路径。
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
    'user explicitly asks for a cloud provider by name), "auto_reject" (boolean, whether '
    "photos that fail quality evaluation should be automatically discarded, default true "
    'unless the user says to keep everything), "count" (integer, how many final photos '
    'the user wants, a reasonable default is 9 if unspecified), "apply_tag" (string, the '
    'tag name to apply to the selected photos, default "精选" unless the user names a '
    "specific tag, album, or audience)."
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
    return Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Evaluate", params={
            "provider": decision.get("provider", "local"),
            "auto_reject": decision.get("auto_reject", True),
        }),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={
            "count": decision.get("count", 9),
            "apply_tag": decision.get("apply_tag", "精选"),
        }),
        StageSpec(name="Style", params={
            "provider": decision.get("provider", "local"),
        }),
        StageSpec(name="Deliver"),
    ])
