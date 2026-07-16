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
    inputs: List[str] = field(default_factory=lambda: ["Style"])
    cost_class: str = "local"
    criticality: str = "optional"  # 降级不死：交付失败不该抹掉前面已经算完的选片结果

    def _marker_path(self, run_id: str, selected: List[str], applied_styles: Dict[str, str]) -> Path:
        # marker 必须跟"这批具体交付的是哪几张、什么顺序、套了什么风
        # 格"绑定，不能只认 run_id：子增量 E 的调整(换掉第N张/改张数)
        # 会让同一个 run_id 的 Curate 输出换成不同的 selected 列表，
        # Deliver 子图重跑要能把新结果真的送出去；只用 run_id 当 marker
        # 会把"这次 Curate 换了新结果"误判成"上次已经交付过了"，新选
        # 片永远发不出去。目标三加了 Style 之后同理：selected 路径列表
        # 不变、只是单独调整了风格，也必须触发重新交付，否则新风格永
        # 远到不了交付文件，见
        # docs/W2026-07-15_AgentStyle_Eng_Design.md 第七节。哈希只用来
        # 让文件名短且确定，不要求防碰撞级别的强度。
        style_sig = "|".join(f"{p}={applied_styles.get(p, '')}" for p in selected)
        digest = hashlib.sha256((("|".join(selected)) + "||" + style_sig).encode("utf-8")).hexdigest()[:16]
        return Path(self.marker_dir) / f"{run_id}-{digest}.json"

    def _load_sent(self, marker_path: Path) -> List[str]:
        if not marker_path.exists():
            return []
        return json.loads(marker_path.read_text())["sent"]

    def _persist_sent(self, marker_path: Path, sent: List[str]) -> None:
        # 用跟 store/run_store.py 一致的原子写(先写临时文件再 os.replace)。
        marker_path.parent.mkdir(parents=True, exist_ok=True)
        tmp_path = marker_path.with_suffix(".json.tmp")
        tmp_path.write_text(json.dumps({"sent": sent}))
        os.replace(tmp_path, marker_path)

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
        style_output = ctx.outputs.get("Style")
        applied_styles: Dict[str, str] = style_output.data.get("applied", {}) if style_output else {}
        marker_path = self._marker_path(ctx.run_id, selected, applied_styles)
        run_staging_dir = Path(self.staging_dir) / ctx.run_id

        sent = self._load_sent(marker_path)
        if set(sent) >= set(selected):
            delivered_paths = [str(run_staging_dir / Path(p).name) for p in selected]
            return StageOutput(ok=True, data={"already_delivered": True, "delivered": delivered_paths})

        try:
            export_result = self.client.call("export-images", ctx.project_id, *selected, str(run_staging_dir))
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        sent_set = set(sent)
        for path in selected:
            if path in sent_set:
                continue
            staged_path = str(run_staging_dir / Path(path).name)
            self.transport.send_file(self.chat_id, staged_path)
            # 逐张标记：每成功发完一张就立刻落盘，崩溃最多重发这一张
            # 还没标记完的，不会把已经发出去的其它张也重发一遍。
            sent.append(path)
            sent_set.add(path)
            self._persist_sent(marker_path, sent)

        self.transport.send_text(self.chat_id, f"选好了 {len(selected)} 张")

        delivered_paths = [str(run_staging_dir / Path(p).name) for p in selected]
        return StageOutput(ok=True, data={"delivered": delivered_paths, "exported": export_result["exported"]})
