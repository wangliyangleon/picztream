from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class DedupStage:
    client: PztClient
    name: str = "Dedup"
    inputs: List[str] = field(default_factory=lambda: ["Ingest"])
    cost_class: str = "local"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        args = ["dedup", ctx.project_id, "--scope", "*"]
        if params.get("ai_enabled", False):
            args += ["--ai", "--provider", params.get("provider", "local")]
        try:
            result = self.client.call(*args)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")
        return StageOutput(ok=True, data=result)
