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
    # criticality 仍是 critical，但只对"套 recipe 到代表图真失败"这种硬故障
    # 生效：风格没匹配上任何 preset、或用户不要滤镜（空描述），都不是失败，
    # 而是软化为 chosen_recipe=None（AG-01/AG-16.1）——匹配失败由 worker 退回
    # Style 闸门重新问，空描述则原图直出、管线空跑。
    criticality: str = "critical"
    # 测试用的 fake 注入点：match_style_description 直接调
    # compose/llm_client.py::request_json(..., http_post=...)，不经过
    # self.client 那条已经被 PztClient fake runner 接管的子进程边界，需要
    # 单独给一个注入点，否则测试会真的打网络请求到本地 Ollama。
    http_post: Optional[HttpPostFn] = None

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        description = (params.get("style_description") or "").strip()

        curate_output = ctx.outputs.get("Curate")
        selected: List[str] = curate_output.data.get("selected", []) if curate_output else []
        if not selected:
            return StageOutput(ok=True, data={"chosen_recipe": None, "preview_photo": None})

        if not description:
            # 空描述 = 不套滤镜/原图直出（skip 路径传的就是 ""）。不是失败，
            # 管线按 chosen_recipe=None 空跑（AG-16.1）。
            return StageOutput(ok=True, data={"chosen_recipe": None, "preview_photo": None})

        provider = params.get("provider", "local")
        try:
            recipe_name = match_style_description(description, http_post=self.http_post, meta_provider=provider)
        except (StyleMatchError, LlmRequestError):
            # 描述没映射到任何 preset（本地小模型常见）：软失败，不报废整批。
            # worker 见到 match_failed 会退回 Style 闸门重新问（AG-01）。
            return StageOutput(ok=True, data={"chosen_recipe": None, "preview_photo": None,
                                              "match_failed": True})

        preview_photo = selected[0]
        try:
            self.client.call("recipe", "apply", ctx.project_id, preview_photo, recipe_name)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        return StageOutput(ok=True, data={"chosen_recipe": recipe_name, "preview_photo": preview_photo})
