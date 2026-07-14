"""Telegram 场景下的会话路由：把 InboundMessage 分发到"当前唯一活跃
run"上，串起 Collecting -> compose_plan(提议) -> PLANNED(等用户确认) ->
Driver 跑批 -> 送 Gate 预览这条链路。子增量 F1 只支持单聊天单活跃 run
（handle_message 里用 assert 顶住），多会话/多项目并发不在这个子增量
范围内。PLANNED 是子增量 F3 加的：意图解析成 Plan 之后不直接开跑，先
把参数回显给用户确认，避免 compose_plan 理解错了却要等 Evaluate 真花
了云端额度才发现。
"""
from __future__ import annotations

import time
from pathlib import Path
from typing import Any, Callable, Optional

from compose.adjustment_parser import AdjustmentError
from compose.llm_client import LlmRequestError
from compose.validate import ValidationError, validate_plan
from orchestrator.driver import Driver
from orchestrator.types import RunState, RunStatus, StageStatus
from pzt_client import PztClient, PztCommandError
from router.collecting import (
    drain_queue_into,
    incoming_dir_for,
    new_collecting_run,
    new_run_id,
    queue_incoming_photo,
    stage_incoming_photo,
)
from store.run_store import RunStore
from transport.base import InboundMessage

_APPROVE_KEYWORDS = {"", "approve", "同意", "可以", "好", "好的", "ok"}
_REJECT_KEYWORDS = {"取消", "cancel", "算了"}


class SessionRouter:
    def __init__(self, store: RunStore, driver: Driver, transport: Any, client: PztClient, chat_id: str,
                 incoming_root: Path, preview_root: Path, deliver_out_folder: Path,
                 compose_plan_fn: Any, classify_gate_reply_fn: Any, refine_plan_confirmation_fn: Any,
                 now_fn: Callable[[], float] = time.time, idle_reminder_seconds: float = 300.0) -> None:
        self.store = store
        self.driver = driver
        self.transport = transport
        self.client = client  # 预览导出用，见 _send_preview
        self.chat_id = chat_id
        self.incoming_root = Path(incoming_root)
        self.preview_root = Path(preview_root)  # 预览导出目的地，跟 DeliverStage 的 staging_dir 同构
        self.deliver_out_folder = Path(deliver_out_folder)
        self.compose_plan_fn = compose_plan_fn
        self.classify_gate_reply_fn = classify_gate_reply_fn
        self.refine_plan_confirmation_fn = refine_plan_confirmation_fn
        self.now_fn = now_fn
        self.idle_reminder_seconds = idle_reminder_seconds

    def handle_message(self, msg: InboundMessage) -> Optional[RunState]:
        if msg.chat_id != self.chat_id:
            return None

        active = self.store.list_active()
        assert len(active) <= 1, f"F1 只支持单聊天单活跃 run，当前有 {len(active)} 个"
        if active:
            run = active[0]
        else:
            run = self._mint_collecting_run()

        return self._dispatch(run, msg)

    def _mint_collecting_run(self) -> RunState:
        run = new_collecting_run(new_run_id())
        run.last_activity_at = self.now_fn()
        moved = drain_queue_into(self.incoming_root, run.run_id)
        self.store.save(run)
        if moved:
            self.transport.send_text(self.chat_id, f"之前排队的 {len(moved)} 张已经并进这一批了")
        return run

    def _dispatch(self, run: RunState, msg: InboundMessage) -> RunState:
        if run.status == RunStatus.AWAITING_REVIEW:
            self.driver.approve(run)
            run = self._mint_collecting_run()

        if run.status in (RunStatus.COLLECTING, RunStatus.PLANNED, RunStatus.AWAITING_GATE):
            # 每条真正落到一个活跃 run 上的消息都刷新一次"最近活跃时
            # 间"、清掉"已经提醒过"的标记 -- 静默计时器算的是"用户有多
            # 久没搭理这个 run"，不是"run 存在了多久"。
            run.last_activity_at = self.now_fn()
            run.reminder_sent = False
            self.store.save(run)

        if run.status == RunStatus.COLLECTING:
            return self._handle_collecting(run, msg)

        if run.status == RunStatus.PLANNED:
            return self._handle_planned(run, msg)

        if run.status == RunStatus.AWAITING_GATE:
            return self._handle_gate(run, msg)

        if run.status == RunStatus.RUNNING:
            # 崩溃后续跑：drive_to_stop 之后要是停在 AWAITING_GATE，用
            # 户还是得看到预览，所以走带通知的版本，不能只 _drive_to_stop。
            self._drive_to_stop_and_notify(run)
            return self._dispatch(run, msg)

        return run

    def check_idle_timers(self) -> None:
        active = self.store.list_active()
        if not active:
            return
        run = active[0]
        if run.reminder_sent or run.last_activity_at is None:
            return
        if self.now_fn() - run.last_activity_at < self.idle_reminder_seconds:
            return

        if run.status == RunStatus.COLLECTING:
            count = len(list(incoming_dir_for(self.incoming_root, run.run_id).iterdir()))
            self.transport.send_text(self.chat_id, f"看到你发了 {count} 张，想怎么处理？")
        elif run.status == RunStatus.PLANNED:
            self.transport.send_text(self.chat_id, "还在等你确认要不要这么处理，满意就说\"好的\"")
        elif run.status == RunStatus.AWAITING_GATE:
            self.transport.send_text(
                self.chat_id,
                "还在等你的回复呢，满意就说\"好的\"，不满意说说想怎么调，不要了就说\"取消\"",
            )
        else:
            return

        run.reminder_sent = True
        self.store.save(run)

    def _handle_gate(self, run: RunState, msg: InboundMessage) -> RunState:
        if msg.kind in ("photo", "file"):
            if msg.file_path:
                queue_incoming_photo(self.incoming_root, msg.file_path)
            self.transport.send_text(self.chat_id, "先帮你收着，这批处理完就接着看这些新照片")
            return run

        text = (msg.text or "").strip()
        normalized = text.lower()

        if normalized in _REJECT_KEYWORDS:
            self.driver.cancel(run)
            self.transport.send_text(self.chat_id, "已取消")
            return run

        if normalized in _APPROVE_KEYWORDS:
            self.driver.resolve_gate(run, "proceed")
            self._drive_to_stop(run)
            if run.status == RunStatus.AWAITING_REVIEW:
                self.driver.approve(run)
            return run

        try:
            reply = self.classify_gate_reply_fn(text, run)
        except AdjustmentError:
            self.transport.send_text(
                self.chat_id,
                "没听懂这句话，能再说清楚点吗？满意就说\"好的\"，不满意说说想怎么调，不要了就说\"取消\"",
            )
            return run

        if reply.action == "approve":
            self.driver.resolve_gate(run, "proceed")
            self._drive_to_stop(run)
            if run.status == RunStatus.AWAITING_REVIEW:
                self.driver.approve(run)
            return run

        if reply.action == "reject":
            self.driver.cancel(run)
            self.transport.send_text(self.chat_id, "已取消")
            return run

        self.driver.apply_adjustment(run, reply.delta)
        return self._drive_to_stop_and_notify(run)

    def _handle_collecting(self, run: RunState, msg: InboundMessage) -> RunState:
        if msg.kind in ("photo", "file"):
            if msg.file_path:
                stage_incoming_photo(self.incoming_root, run.run_id, msg.file_path)
            self.store.save(run)
            return run  # 逐张不回复：一批多张照片连发不该刷屏

        text = (msg.text or "").strip()
        if not text:
            return run
        return self._propose_plan(run, text)

    def _propose_plan(self, run: RunState, intent_text: str) -> RunState:
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
        run.status = RunStatus.PLANNED
        self.store.save(run)
        self._send_plan_confirmation(run)
        return run

    def _send_plan_confirmation(self, run: RunState) -> None:
        evaluate = next(s for s in run.plan.stages if s.name == "Evaluate")
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        auto_reject_desc = "自动剔除不合格照片" if evaluate.params["auto_reject"] else "不自动剔除照片"
        self.transport.send_text(
            self.chat_id,
            f"理解你想：用 {evaluate.params['provider']} 评估，{auto_reject_desc}，"
            f"留 {curate.params['count']} 张，标签叫\"{curate.params['apply_tag']}\"，对吗？"
            "\n满意就说\"好的\"，不对就直接说想怎么改，不要了就说\"取消\"",
        )

    def _handle_planned(self, run: RunState, msg: InboundMessage) -> RunState:
        if msg.kind in ("photo", "file"):
            if msg.file_path:
                # PLANNED 时 Ingest 还没跑，这个文件夹还没被任何 Stage
                # 消费过，新照片直接并进去完全安全，不用像 AwaitingGate
                # 那样排队(那边 Curate 已经跑完了，排队是为了不把结果
                # 弄脏)。
                stage_incoming_photo(self.incoming_root, run.run_id, msg.file_path)
            return run

        text = (msg.text or "").strip()
        normalized = text.lower()

        if normalized in _REJECT_KEYWORDS:
            self.driver.cancel(run)
            self.transport.send_text(self.chat_id, "已取消")
            return run

        if normalized in _APPROVE_KEYWORDS:
            return self._begin_running(run)

        current_params = self._current_plan_params(run)
        try:
            reply = self.refine_plan_confirmation_fn(run.intent_raw, current_params, text)
        except AdjustmentError:
            self.transport.send_text(
                self.chat_id,
                "没听懂，满意就说\"好的\"，不满意说说想怎么改，不要了就说\"取消\"",
            )
            return run

        if reply.action == "clarify":
            self.transport.send_text(self.chat_id, reply.question)
            return run

        evaluate = next(s for s in run.plan.stages if s.name == "Evaluate")
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        evaluate.params["provider"] = reply.provider
        evaluate.params["auto_reject"] = reply.auto_reject
        curate.params["count"] = reply.count
        curate.params["apply_tag"] = reply.apply_tag
        self.store.save(run)
        return self._begin_running(run)

    def _current_plan_params(self, run: RunState) -> dict:
        evaluate = next(s for s in run.plan.stages if s.name == "Evaluate")
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        return {
            "provider": evaluate.params["provider"],
            "auto_reject": evaluate.params["auto_reject"],
            "count": curate.params["count"],
            "apply_tag": curate.params["apply_tag"],
        }

    def _begin_running(self, run: RunState) -> RunState:
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
        failed_count = 0
        for path in selected:
            preview_path = str(preview_dir / Path(path).name)
            try:
                self.transport.send_photo(self.chat_id, preview_path)
            except Exception:
                # 压缩预览图有 Telegram 自己的体积上限(真机验证时撞到过
                # BadRequest: File is too big)，比普通文件上传严格得
                # 多。这里不能让一张超标的图把整个预览循环、乃至后面
                # "选好了 N 张"这句提示一起带崩——那样用户会卡在不知道
                # 该做什么的地方。退化成按文件发一次，实在也发不出去就
                # 跳过，最后统一在总结里报一句。
                try:
                    self.transport.send_file(self.chat_id, preview_path)
                except Exception:
                    failed_count += 1
        summary = f"选好了 {len(selected)} 张"
        if failed_count:
            summary += f"(其中 {failed_count} 张预览发送失败，交付时仍会正常导出)"
        summary += "，满意就回复\"好的\"，不满意说说想怎么调，不要了就说\"取消\""
        self.transport.send_text(self.chat_id, summary)
