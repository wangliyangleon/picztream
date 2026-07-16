from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional

from compose.llm_client import HttpPostFn, LlmRequestError
from compose.style_matcher import StyleMatchError, match_style_description
from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class StyleStage:
    client: PztClient
    name: str = "Style"
    inputs: List[str] = field(default_factory=lambda: ["Curate"])
    cost_class: str = "cloud"
    # 现在只有一次风格决策(挑 preset + 套到代表图)，失败了下游 StyleApplyAll/
    # Deliver 都没有意义，不再是旧版"部分照片失败可以跳过"的语义。
    criticality: str = "critical"
    # 测试用的 fake 注入点：match_style_description 直接调
    # compose/llm_client.py::request_json(..., http_post=...)，不经过
    # self.client 那条已经被 PztClient fake runner 接管的子进程边界，需要
    # 单独给一个注入点，否则测试会真的打网络请求到本地 Ollama。
    http_post: Optional[HttpPostFn] = None

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        description = (params.get("style_description") or "").strip()
        if not description:
            return StageOutput(ok=False, error="missing style_description")

        curate_output = ctx.outputs.get("Curate")
        selected: List[str] = curate_output.data.get("selected", []) if curate_output else []
        if not selected:
            return StageOutput(ok=True, data={"chosen_recipe": None, "preview_photo": None})

        provider = params.get("provider", "local")
        try:
            recipe_name = match_style_description(description, http_post=self.http_post, meta_provider=provider)
        except (StyleMatchError, LlmRequestError) as e:
            return StageOutput(ok=False, error=str(e))

        preview_photo = selected[0]
        try:
            self.client.call("recipe", "apply", ctx.project_id, preview_photo, recipe_name)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        return StageOutput(ok=True, data={"chosen_recipe": recipe_name, "preview_photo": preview_photo})
