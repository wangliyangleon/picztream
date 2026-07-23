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
    # "Ingest" 而非 "Dedup"：run() 从不读 ctx.outputs["Dedup"]，这里只是顺
    # 序声明，Dedup 存在时 Plan 的 list 顺序已经保证先后。W2026-07-21 目标
    # 三起 Dedup 可能不在 Plan 里，声明成 "Dedup" 会让 Driver 的拓扑检查把
    # 它当成永远解不开的依赖。
    inputs: List[str] = field(default_factory=lambda: ["Ingest"])
    cost_class: str = "local"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        count = params["count"]
        apply_tag = params.get("apply_tag", "精选")
        exclude = params.get("exclude", [])

        try:
            if count is None:
                # passthrough：Curate 被跳过聚类，直接把去重后的候选原样
                # 交付，不调 pzt curate（W2026-07-21 目标三决策三：pzt
                # curate 的 count 语义是"每簇最多一个 winner"，不是
                # "top N"，冒充会把用户明确拒绝的"再筛一次"悄悄做了）。
                images = self.client.call("images", ctx.project_id)["images"]
                survivors = [img["path"] for img in images
                             if "重复" not in img["tags"] and "废片" not in img["tags"]]
                final_selection = [p for p in survivors if p not in exclude]
                requested = None
            else:
                args = [
                    "curate", ctx.project_id,
                    "--count", str(count + len(exclude)),
                    "--apply-tag", apply_tag,
                ]
                if params.get("ai_enabled", False):
                    args += ["--ai", "--provider", params.get("provider", "local")]
                result = self.client.call(*args)
                # pzt curate --apply-tag 无条件给拿到的每一张候选打标(包括
                # 多要的 len(exclude) 张、以及要被换掉的那几张)，过滤裁剪
                # 之后必须重新收口标签状态：不能指望 --apply-tag 自己做对。
                final_selection = [p for p in result["selected"] if p not in exclude][:count]
                requested = count

            self.client.call("tag", "clear", ctx.project_id, apply_tag)
            for path in final_selection:
                self.client.call("tag", "apply", ctx.project_id, path, apply_tag)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        return StageOutput(ok=True, data={
            "requested": requested,
            "returned": len(final_selection),
            "selected": final_selection,
        })
