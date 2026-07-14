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
    inputs: List[str] = field(default_factory=lambda: ["Evaluate"])
    cost_class: str = "local"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        try:
            result = self.client.call("dedup", ctx.project_id, "--scope", "*")
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")
        return StageOutput(ok=True, data=result)
