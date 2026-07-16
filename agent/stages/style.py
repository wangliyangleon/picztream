from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class StyleStage:
    client: PztClient
    name: str = "Style"
    inputs: List[str] = field(default_factory=lambda: ["Curate"])
    cost_class: str = "cloud"
    criticality: str = "optional"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        curate_output = ctx.outputs.get("Curate")
        selected: List[str] = curate_output.data.get("selected", []) if curate_output else []
        provider = params.get("provider", "gemini")

        applied: Dict[str, str] = {}
        skipped: List[Dict[str, str]] = []
        for path in selected:
            try:
                suggestion = self.client.call(
                    "recipe", "suggest", ctx.project_id, path, "--provider", provider)
                self.client.call("recipe", "apply", ctx.project_id, path, suggestion["recipe_name"])
                applied[path] = suggestion["recipe_name"]
            except PztCommandError as e:
                skipped.append({"path": path, "error": f"{e.code}: {e.message}"})

        if selected and not applied:
            return StageOutput(
                ok=False, error="style suggestion/apply failed for every selected photo",
                data={"applied": applied}, skipped=skipped)
        return StageOutput(ok=True, data={"applied": applied}, skipped=skipped)
