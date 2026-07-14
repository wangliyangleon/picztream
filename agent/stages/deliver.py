from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError


@dataclass
class DeliverStage:
    client: PztClient
    transport: Any
    marker_dir: Path
    chat_id: str = "watchfolder"
    name: str = "Deliver"
    inputs: List[str] = field(default_factory=lambda: ["Curate"])
    cost_class: str = "local"
    criticality: str = "optional"  # 降级不死:交付失败不该抹掉前面已经算完的选片结果

    def _marker_path(self, run_id: str) -> Path:
        return Path(self.marker_dir) / f"{run_id}.json"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        marker_path = self._marker_path(ctx.run_id)
        if marker_path.exists():
            delivered = json.loads(marker_path.read_text())["paths"]
            return StageOutput(ok=True, data={"already_delivered": True, "delivered": delivered})

        curate_output = ctx.outputs.get("Curate")
        selected: List[str] = curate_output.data.get("selected", []) if curate_output else []
        out_folder = params["out_folder"]

        try:
            export_result = self.client.call("export-images", ctx.project_id, *selected, out_folder)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        delivered_paths = []
        for path in selected:
            exported_path = str(Path(out_folder) / Path(path).name)
            self.transport.send_file(self.chat_id, exported_path)
            delivered_paths.append(exported_path)
        self.transport.send_text(self.chat_id, f"选好了 {len(delivered_paths)} 张")

        # 标记必须在真正发送之后才写(见本计划 Global Constraints 的幂
        # 等说明)，用跟 store/run_store.py 一致的原子写。
        marker_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = marker_path.with_suffix(".json.tmp")
        tmp_path.write_text(json.dumps({"paths": delivered_paths}))
        os.replace(tmp_path, marker_path)

        return StageOutput(ok=True, data={"delivered": delivered_paths, "exported": export_result["exported"]})
