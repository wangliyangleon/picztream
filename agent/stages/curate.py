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
        count = params["count"]
        apply_tag = params.get("apply_tag", "精选")
        exclude = params.get("exclude", [])

        args = [
            "curate", ctx.project_id,
            "--count", str(count + len(exclude)),
            "--apply-tag", apply_tag,
        ]
        if params.get("ai_enabled", False):
            args += ["--ai", "--provider", params.get("provider", "local")]

        try:
            result = self.client.call(*args)
            # pzt curate --apply-tag 无条件给拿到的每一张候选打标(包括
            # 多要的 len(exclude) 张、以及要被换掉的那几张)，过滤裁剪
            # 之后必须重新收口标签状态：不能指望 --apply-tag 自己做对。
            final_selection = [p for p in result["selected"] if p not in exclude][:count]
            self.client.call("tag", "clear", ctx.project_id, apply_tag)
            for path in final_selection:
                self.client.call("tag", "apply", ctx.project_id, path, apply_tag)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        return StageOutput(ok=True, data={
            "requested": count,
            "returned": len(final_selection),
            "selected": final_selection,
        })
