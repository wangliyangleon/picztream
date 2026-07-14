from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class IngestStage:
    client: PztClient
    name: str = "Ingest"
    inputs: List[str] = field(default_factory=list)
    cost_class: str = "local"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        try:
            result = self.client.call("new", ctx.project_id, params["folder"])
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")
        return StageOutput(ok=True, data={"image_count": result["image_count"]})
