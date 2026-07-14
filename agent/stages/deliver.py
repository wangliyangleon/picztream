from __future__ import annotations

import hashlib
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
    criticality: str = "optional"  # 降级不死：交付失败不该抹掉前面已经算完的选片结果

    def _marker_path(self, run_id: str, selected: List[str]) -> Path:
        # marker 必须跟"这批具体交付的是哪几张、什么顺序"绑定，不能只
        # 认 run_id：子增量 E 的调整(换掉第N张/改张数)会让同一个 run_id
        # 的 Curate 输出换成不同的 selected 列表，Deliver 子图重跑要能
        # 把新结果真的送出去；只用 run_id 当 marker 会把"这次 Curate
        # 换了新结果"误判成"上次已经交付过了"，新选片永远发不出去。
        # 哈希只用来让文件名短且确定，不要求防碰撞级别的强度。
        digest = hashlib.sha256("|".join(selected).encode("utf-8")).hexdigest()[:16]
        return Path(self.marker_dir) / f"{run_id}-{digest}.json"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        curate_output = ctx.outputs.get("Curate")
        # export-images 只负责"把选中的图导出成字节"，导出目的地是这个
        # Stage 私有的暂存目录(按 run_id 隔离，不是 params["out_folder"]
        # ：那是 transport 最终交付的目的地，两者不能是同一个文件夹，
        # 否则 pzt export-images 已经把文件放进 out_folder 之后，
        # transport.send_file 再拷贝同一个路径进同一个 out_folder，
        # src/dst 会撞成同一个文件，真机跑的时候 shutil.copy2 直接报
        # SameFileError。
        selected: List[str] = curate_output.data.get("selected", []) if curate_output else []
        marker_path = self._marker_path(ctx.run_id, selected)
        if marker_path.exists():
            delivered = json.loads(marker_path.read_text())["paths"]
            return StageOutput(ok=True, data={"already_delivered": True, "delivered": delivered})

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

        # 标记必须在真正发送之后才写，用跟 store/run_store.py 一致的原
        # 子写(先写临时文件再 os.replace)。
        marker_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = marker_path.with_suffix(".json.tmp")
        tmp_path.write_text(json.dumps({"paths": delivered_paths}))
        os.replace(tmp_path, marker_path)

        return StageOutput(ok=True, data={"delivered": delivered_paths, "exported": export_result["exported"]})
