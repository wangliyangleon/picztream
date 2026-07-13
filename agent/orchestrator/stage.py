from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Literal, Protocol

from .types import StageOutput


@dataclass
class StageContext:
    run_id: str
    project_id: str
    outputs: dict[str, StageOutput] = field(default_factory=dict)


class Stage(Protocol):
    name: str
    inputs: list[str]
    cost_class: Literal["local", "cloud"]
    criticality: Literal["critical", "optional"]

    def run(self, ctx: StageContext, params: dict[str, Any]) -> StageOutput: ...
