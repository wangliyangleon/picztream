from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Literal

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput


@dataclass
class FakeStage:
    name: str
    inputs: list[str] = field(default_factory=list)
    cost_class: Literal["local", "cloud"] = "local"
    criticality: Literal["critical", "optional"] = "critical"
    result: StageOutput = field(default_factory=lambda: StageOutput(ok=True))
    calls: list[dict[str, Any]] = field(default_factory=list)

    def run(self, ctx: StageContext, params: dict[str, Any]) -> StageOutput:
        self.calls.append({"params": dict(params), "outputs_seen": set(ctx.outputs.keys())})
        return self.result
