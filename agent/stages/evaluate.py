from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class EvaluateStage:
    client: PztClient
    name: str = "Evaluate"
    inputs: List[str] = field(default_factory=lambda: ["Ingest"])
    cost_class: str = "cloud"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        args = ["eval", ctx.project_id, "--scope", "*", "--provider", params.get("provider", "gemini")]
        if params.get("auto_reject", True):
            args.append("--auto-reject")
        try:
            result = self.client.call(*args)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")
        return StageOutput(
            ok=True,
            data={"submitted": result["submitted"], "evaluated": result["evaluated"]},
            skipped=result["failed"],
        )
