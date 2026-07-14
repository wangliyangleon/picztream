"""Data types for the deterministic orchestrator (Stage/Plan/Run)."""
from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Literal, Optional


class StageStatus(str, Enum):
    PENDING = "pending"
    RUNNING = "running"
    DONE = "done"
    FAILED = "failed"
    SKIPPED = "skipped"


class RunStatus(str, Enum):
    COLLECTING = "collecting"
    PLANNED = "planned"
    RUNNING = "running"
    AWAITING_GATE = "awaiting_gate"
    AWAITING_REVIEW = "awaiting_review"
    DONE = "done"
    FAILED = "failed"
    CANCELLED = "cancelled"


GateSetting = Literal["off", "courtesy", "required"]
GateDecision = Literal["proceed", "hold"]


@dataclass
class StageSpec:
    name: str
    params: dict[str, Any] = field(default_factory=dict)
    gate: GateSetting = "off"
    gate_on_timeout: GateDecision = "proceed"


@dataclass
class Plan:
    stages: list[StageSpec]


@dataclass
class StageOutput:
    ok: bool
    data: dict[str, Any] = field(default_factory=dict)
    skipped: list[Any] = field(default_factory=list)
    error: Optional[str] = None


@dataclass
class GateState:
    stage_name: str
    setting: GateSetting
    decision: Optional[GateDecision] = None


@dataclass
class RunState:
    run_id: str
    project_id: str
    plan: Plan
    stage_states: dict[str, StageStatus]
    outputs: dict[str, StageOutput] = field(default_factory=dict)
    gate_state: Optional[GateState] = None
    intent_raw: str = ""
    status: RunStatus = RunStatus.PLANNED
    last_activity_at: Optional[float] = None
    reminder_sent: bool = False


@dataclass
class PlanDelta:
    stage_name: str
    params: dict[str, Any] = field(default_factory=dict)


def run_state_from_dict(data: dict[str, Any]) -> RunState:
    plan = Plan(stages=[StageSpec(**s) for s in data["plan"]["stages"]])
    stage_states = {k: StageStatus(v) for k, v in data["stage_states"].items()}
    outputs = {k: StageOutput(**v) for k, v in data.get("outputs", {}).items()}
    gate_state = GateState(**data["gate_state"]) if data.get("gate_state") else None
    return RunState(
        run_id=data["run_id"],
        project_id=data["project_id"],
        plan=plan,
        stage_states=stage_states,
        outputs=outputs,
        gate_state=gate_state,
        intent_raw=data.get("intent_raw", ""),
        status=RunStatus(data["status"]),
        last_activity_at=data.get("last_activity_at"),
        reminder_sent=data.get("reminder_sent", False),
    )
