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
    staging_dir: Path
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
        # export-images 只负责"把选中的图导出成字节"，导出目的地是这个
        # Stage 私有的暂存目录(按 run_id 隔离，不是 params["out_folder"]
        # ——那是 transport 最终交付的目的地，两者不能是同一个文件夹：
        # pzt export-images 已经把文件放进 out_folder 了，如果 Deliver 再
        # 管 transport.send_file 去"发"同一个路径进同一个 out_folder，
        # src/dst 会撞成同一个文件，真机跑的时候 shutil.copy2 直接报
        # SameFileError。导出(核心的事) 和 交付(transport 的事) 必须是
        # 两步、两个目的地——真实 Telegram 传输下这两步本来就分明:先导
        # 出到本地暂存，再把字节上传出去，WatchFolderTransport 只是把
        # "上传"实现成"拷进 out_dir"，不该跟导出目的地共用同一个文件夹。
        run_staging_dir = Path(self.staging_dir) / ctx.run_id

        try:
            export_result = self.client.call("export-images", ctx.project_id, *selected, str(run_staging_dir))
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        delivered_paths = []
        for path in selected:
            staged_path = str(run_staging_dir / Path(path).name)
            self.transport.send_file(self.chat_id, staged_path)
            delivered_paths.append(staged_path)
        self.transport.send_text(self.chat_id, f"选好了 {len(delivered_paths)} 张")

        # 标记必须在真正发送之后才写(见本计划 Global Constraints 的幂
        # 等说明)，用跟 store/run_store.py 一致的原子写。
        marker_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = marker_path.with_suffix(".json.tmp")
        tmp_path.write_text(json.dumps({"paths": delivered_paths}))
        os.replace(tmp_path, marker_path)

        return StageOutput(ok=True, data={"delivered": delivered_paths, "exported": export_result["exported"]})
