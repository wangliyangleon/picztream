"""SessionView：consumer 私有的会话内存视图（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第四节）。它回答"现在到哪一步/进度多少"，让 consumer 在
worker 独占 RunState 期间（DriveJob 活跃）也能秒级应答——**不是**持久
真相，重启从 RunStore 重建，平时被 worker 事件更新。

photo_count 故意不做成字段：照片数的真相是 incoming 目录本身，惰性现算
（旧 router 同款），缓存进字段只会跟目录漂移。用户设想的
ai_eval_in_progress 这类步骤枚举也不单独建：(status, current_stage,
stage_progress) 组合已完整表达，平行枚举只会漂移。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Tuple

from orchestrator.types import RunState, RunStatus
from router.collecting import incoming_dir_for

# stage 运行前发给用户的"正在…"进度文案。Style/StyleApplyAll 故意缺席：它们是
# required 闸门，advance() 会在运行前停下问人，"正在自动套用风格..."紧
# 贴着闸门自己的提问会自相矛盾，闸门消息本身就是恰当的进度提示。
STAGE_PROGRESS_MESSAGES = {
    "Ingest": "正在导入照片...",
    "Dedup": "正在执行去重...",
    "Curate": "正在筛选...",
    "Deliver": "正在交付...",
}


@dataclass
class SessionView:
    incoming_root: Path
    run_id: Optional[str] = None
    project_id: Optional[str] = None
    status: Optional[RunStatus] = None
    current_stage: Optional[str] = None            # StageStarted 事件更新
    stage_progress: Optional[Tuple[int, int]] = None  # (done, total)
    gate_stage: Optional[str] = None               # GateReached 事件更新
    plan_summary: Optional[dict] = None            # count/apply_tag/ai_enabled
    selected_count: Optional[int] = None           # GateReached payload / 重建时从 outputs 抄
    drive_active: bool = False                     # DriveJob 入队 True，闸门/终态事件 False

    def photo_count(self) -> int:
        if self.run_id is None:
            return 0
        return len(list(incoming_dir_for(self.incoming_root, self.run_id).iterdir()))

    def describe(self) -> str:
        # COLLECTING/PLANNED/AWAITING_GATE 三条逐字对齐旧
        # _status_snapshot_text；RUNNING 是 2.0 新增分支（旧实现跑批期间
        # 根本收不到消息，没有这个应答场景）。
        if self.status == RunStatus.COLLECTING:
            return f"目前收到 {self.photo_count()} 张照片，还没告诉我想怎么处理"
        if self.status == RunStatus.PLANNED and self.plan_summary is not None:
            if self.plan_summary.get("count") is None:
                # deferred 形状（W2026-07-21 目标三案例二）：Curate 数量待
                # 定，这一步只说"去重"，不预告后面还要问什么（真机反馈）。
                return (f"目前收到 {self.photo_count()} 张照片，方案是："
                        f"先帮你去重，"
                        f"标签叫\"{self.plan_summary['apply_tag']}\"")
            ai_desc = ("AI 帮你从相似照片里挑更好的" if self.plan_summary.get("ai_enabled")
                       else "按拍摄时间挑")
            return (f"目前收到 {self.photo_count()} 张照片，方案是："
                    f"去重复后留 {self.plan_summary['count']} 张（{ai_desc}），"
                    f"标签叫\"{self.plan_summary['apply_tag']}\"")
        if self.status == RunStatus.AWAITING_GATE:
            if self.gate_stage == "Curate":
                return "去重完了，等你说要不要再筛选一下"
            return f"已经选好了 {self.selected_count or 0} 张，等你回复"
        if self.status == RunStatus.RUNNING:
            base = STAGE_PROGRESS_MESSAGES.get(self.current_stage or "", "正在处理...")
            progress = ""
            if self.stage_progress is not None:
                progress = f"，已完成 {self.stage_progress[0]}/{self.stage_progress[1]} 张"
            return f"{base.rstrip('.')}{progress}"
        return "没什么可说的"


def view_from_run(run: RunState, incoming_root: Path) -> SessionView:
    """启动恢复用：能从落盘 RunState 抄的都抄上。current_stage/进度抄不
    到（那是 drive 过程中的瞬时态，不落盘），留空等续跑事件重新填。"""
    view = SessionView(incoming_root=Path(incoming_root), run_id=run.run_id,
                       project_id=run.project_id, status=run.status)
    curate = next((s for s in run.plan.stages if s.name == "Curate"), None)
    if curate is not None:
        view.plan_summary = {
            "count": curate.params.get("count"),
            "apply_tag": curate.params.get("apply_tag"),
            "ai_enabled": curate.params.get("ai_enabled"),
        }
    curate_output = run.outputs.get("Curate")
    if curate_output is not None:
        view.selected_count = len(curate_output.data.get("selected", []))
    if run.gate_state is not None:
        view.gate_stage = run.gate_state.stage_name
    return view
