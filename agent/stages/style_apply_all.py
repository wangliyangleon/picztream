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
        # T-8：这个 stage 是 N 次子进程调用的循环（30 张精选 = 30 次进程启
        # 动），i/N 在循环里天然就有，不需要跨进程通道。分母用 len(selected)
        # 而不是 len(remaining)：代表图已经在 Style 阶段套好了，报到 "2/2"
        # 而用户明明选了 3 张，会让人以为漏了一张。
        total = len(selected)
        done = len(applied)
        for path in remaining:
            try:
                self.client.call("recipe", "apply", ctx.project_id, path, chosen_recipe)
                applied[path] = chosen_recipe
            except PztCommandError as e:
                skipped.append({"path": path, "error": f"{e.code}: {e.message}"})
            # 失败的也推进：单张失败是软失败（criticality="optional"），计
            # 数卡住不动的话用户看到的是"卡死"而不是"跳过了一张"。
            done += 1
            ctx.on_progress(done, total)

        if remaining and len(skipped) == len(remaining):
            return StageOutput(
                ok=False, error="failed to apply the confirmed style to every remaining photo",
                data={"applied": applied}, skipped=skipped)
        return StageOutput(ok=True, data={"applied": applied}, skipped=skipped)
