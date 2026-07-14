from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class CurateStage:
    client: PztClient
    name: str = "Curate"
    inputs: List[str] = field(default_factory=lambda: ["Dedup"])
    cost_class: str = "local"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        args = ["curate", ctx.project_id, "--count", str(params["count"]), "--apply-tag", params.get("apply_tag", "精选")]
        try:
            result = self.client.call(*args)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")
        return StageOutput(ok=True, data=result)
