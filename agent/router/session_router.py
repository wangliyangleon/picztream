"""Telegram 场景下的会话路由：把 InboundMessage 分发到"当前唯一活跃
run"上，串起 Collecting -> compose_plan -> Driver 跑批 -> 送 Gate 预览
这条链路。子增量 F1 只支持单聊天单活跃 run（handle_message 里用
assert 顶住），多会话/多项目并发不在这个子增量范围内。
"""
from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

from compose.llm_client import LlmRequestError
from compose.validate import ValidationError, validate_plan
from orchestrator.driver import Driver
from orchestrator.types import RunState, RunStatus, StageStatus
from pzt_client import PztClient, PztCommandError
from router.collecting import incoming_dir_for, new_collecting_run, new_run_id, stage_incoming_photo
from store.run_store import RunStore
from transport.base import InboundMessage


class SessionRouter:
    def __init__(self, store: RunStore, driver: Driver, transport: Any, client: PztClient, chat_id: str,
                 incoming_root: Path, preview_root: Path, deliver_out_folder: Path,
                 compose_plan_fn: Any, parse_adjustment_fn: Any) -> None:
        self.store = store
        self.driver = driver
        self.transport = transport
        self.client = client  # 预览导出用，见 _send_preview
        self.chat_id = chat_id
        self.incoming_root = Path(incoming_root)
        self.preview_root = Path(preview_root)  # 预览导出目的地，跟 DeliverStage 的 staging_dir 同构
        self.deliver_out_folder = Path(deliver_out_folder)
        self.compose_plan_fn = compose_plan_fn
        self.parse_adjustment_fn = parse_adjustment_fn

    def handle_message(self, msg: InboundMessage) -> Optional[RunState]:
        if msg.chat_id != self.chat_id:
            return None

        active = self.store.list_active()
        assert len(active) <= 1, f"F1 只支持单聊天单活跃 run，当前有 {len(active)} 个"
        if active:
            run = active[0]
        else:
            run = new_collecting_run(new_run_id())
            self.store.save(run)

        return self._handle_collecting(run, msg)

    def _handle_collecting(self, run: RunState, msg: InboundMessage) -> RunState:
        if msg.kind in ("photo", "file"):
            if msg.file_path:
                stage_incoming_photo(self.incoming_root, run.run_id, msg.file_path)
            self.store.save(run)
            return run  # 逐张不回复：一批多张照片连发不该刷屏

        text = (msg.text or "").strip()
        if not text:
            return run
        return self._start_run_from_intent(run, text)

    def _start_run_from_intent(self, run: RunState, intent_text: str) -> RunState:
        try:
            plan = validate_plan(self.compose_plan_fn(intent_text, None, None))
        except (ValidationError, LlmRequestError) as e:
            self.transport.send_text(self.chat_id, f"没看懂这句意图，能换个说法再说一次吗？（{e.message}）")
            return run  # 留在 Collecting，已收的照片不丢，下一条消息还能重试

        ingest_spec = next(s for s in plan.stages if s.name == "Ingest")
        deliver_spec = next(s for s in plan.stages if s.name == "Deliver")
        ingest_spec.params["folder"] = str(incoming_dir_for(self.incoming_root, run.run_id))
        deliver_spec.params["out_folder"] = str(self.deliver_out_folder)
        deliver_spec.gate = "required"  # 两段式交付的关键决定：Deliver 之前必须停下来等用户确认

        run.plan = plan
        run.stage_states = {s.name: StageStatus.PENDING for s in plan.stages}
        run.intent_raw = intent_text
        run.status = RunStatus.RUNNING
        self.store.save(run)
        return self._drive_to_stop_and_notify(run)

    def _drive_to_stop(self, run: RunState) -> RunState:
        while run.status == RunStatus.RUNNING:
            self.driver.advance(run)
        return run

    def _drive_to_stop_and_notify(self, run: RunState) -> RunState:
        self._drive_to_stop(run)
        if run.status == RunStatus.AWAITING_GATE:
            self._send_preview(run)
        elif run.status == RunStatus.FAILED:
            self.transport.send_text(self.chat_id, f"处理失败：{self._first_failure_detail(run)}")
        return run

    def _first_failure_detail(self, run: RunState) -> str:
        for name, status in run.stage_states.items():
            if status == StageStatus.FAILED:
                output = run.outputs.get(name)
                error = output.error if output else None
                return f"{name}：{error or '未知错误'}"
        return "未知错误"

    def _send_preview(self, run: RunState) -> None:
        # pzt curate 的 selected 是项目 root_path 相对路径（见
        # cli/commands/commands.cpp 的 cmd_curate/cmd_export_images），
        # 不是能直接读的磁盘文件，必须先过一次 pzt export-images 才有
        # 真字节可发，做法跟 stages/deliver.py 交付最终文件时一样，只
        # 是落到 preview_root 而不是 DeliverStage 自己的 staging_dir。
        curate_output = run.outputs.get("Curate")
        selected = curate_output.data.get("selected", []) if curate_output else []
        preview_dir = self.preview_root / run.run_id
        try:
            self.client.call("export-images", run.project_id, *selected, str(preview_dir))
        except PztCommandError as e:
            self.transport.send_text(self.chat_id, f"预览导出失败：{e.message}")
            return
        for path in selected:
            self.transport.send_photo(self.chat_id, str(preview_dir / Path(path).name))
        self.transport.send_text(
            self.chat_id,
            f"选好了 {len(selected)} 张，满意就回复\"好的\"，不满意说说想怎么调，不要了就说\"取消\"",
        )
