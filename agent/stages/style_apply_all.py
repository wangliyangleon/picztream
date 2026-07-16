from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class StyleApplyAllStage:
    client: PztClient
    name: str = "StyleApplyAll"
    inputs: List[str] = field(default_factory=lambda: ["Style"])
    cost_class: str = "local"
    criticality: str = "optional"  # 个别照片套用失败不该拖垮整批交付

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        style_output = ctx.outputs.get("Style")
        chosen_recipe = style_output.data.get("chosen_recipe") if style_output else None
        preview_photo = style_output.data.get("preview_photo") if style_output else None
        curate_output = ctx.outputs.get("Curate")
        selected: List[str] = curate_output.data.get("selected", []) if curate_output else []

        if not chosen_recipe:
            return StageOutput(ok=True, data={"applied": {}})

        # preview_photo 已经在 Style 阶段套用成功了，这里不重复调用。
        applied: Dict[str, str] = {preview_photo: chosen_recipe} if preview_photo else {}
        remaining = [p for p in selected if p != preview_photo]
        skipped: List[Dict[str, str]] = []
        for path in remaining:
            try:
                self.client.call("recipe", "apply", ctx.project_id, path, chosen_recipe)
                applied[path] = chosen_recipe
            except PztCommandError as e:
                skipped.append({"path": path, "error": f"{e.code}: {e.message}"})

        if remaining and len(skipped) == len(remaining):
            return StageOutput(
                ok=False, error="failed to apply the confirmed style to every remaining photo",
                data={"applied": applied}, skipped=skipped)
        return StageOutput(ok=True, data={"applied": applied}, skipped=skipped)
