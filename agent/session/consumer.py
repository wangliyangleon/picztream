"""SessionConsumer：2.0 运行时的消息线程（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第七节），旧 router/session_router.py 的会话逻辑按状态迁
入，LLM 调用点全部换成投 job + 等事件。它是唯一渲染与发送对话文本的
线程；对话文案逐字对齐旧实现（对齐清单见 Eng Design 第八节）。

所有权：非 DriveJob 活跃期间独占 RunState（self.run 非 None 即持有）；
投出 DriveJob 的同时放手（self.run = None），此后只凭 SessionView 应
答，直到 GateReached/RunFinished 事件把所有权经"落盘 + 事件"交回来。

串行规则：文本消息严格 FIFO，上一条的 classify/compose 结果没回来
（inflight 非 None）不处理下一条——否则"改成30张"还没解析完，"好的"
就会拿旧参数抢跑。只有"取消"关键词跳过队列；照片走独立路径永不排队等
LLM。
"""
from __future__ import annotations

import queue
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable, Optional

from orchestrator.types import RunState, RunStatus, StageStatus
from router.collecting import (
    drain_queue_into,
    incoming_dir_for,
    new_collecting_run,
    new_run_id,
    queue_incoming_photo,
    stage_incoming_photo,
)
from session.protocol import (
    ClassifyDone,
    ClassifyFailed,
    ClassifyJob,
    ComposeDone,
    ComposeFailed,
    ComposeJob,
    DriveJob,
    GateReached,
    JobCrashed,
    RunFinished,
    StageStarted,
)
from session.view import STAGE_PROGRESS_MESSAGES, SessionView, view_from_run

_APPROVE_KEYWORDS = {"", "approve", "同意", "可以", "好", "好的", "ok"}
_REJECT_KEYWORDS = {"取消", "cancel", "算了"}
_STYLE_GATE_STAGES = ("Style", "StyleApplyAll")

# LLM/网络这类基础设施失败的痕迹：命中就说"AI 连不上"而不是"没看懂"
# （后者会误导用户以为是自己表述的问题，见真机反馈）。
_INFRA_ERROR_HINTS = ("network_error", "http_error", "missing_api_key", "unknown_provider",
                       "timed out", "Connection", "connection")

# inline 按钮的动作 token。callback_data 拼成 "{token}:{run_id}"，点击回
# 来时校验 run_id 防误触旧消息（见 _handle_callback）。真机反馈：所有
# yes/no 确认点都改按钮，精确关键词匹配 + "非关键词一律当新风格描述" 太
# 容易踩坑（"不错"/"ok好的" 被当描述 -> hallucinate）。打字仍然照旧可
# 用（关键词批准/自由文本调整/重描述），按钮只是额外的无歧义快路径。
_BTN_APPROVE = "approve"
_BTN_REJECT = "reject"
_BTN_RESTYLE = "restyle"
_CONFIRM_BUTTONS = [("好的 ✅", _BTN_APPROVE), ("取消 ✖", _BTN_REJECT)]
_DELIVER_BUTTONS = [("满意 ✅", _BTN_APPROVE), ("取消 ✖", _BTN_REJECT)]
_STYLE_APPLY_ALL_BUTTONS = [("满意 ✅", _BTN_APPROVE), ("重选 🔄", _BTN_RESTYLE), ("取消 ✖", _BTN_REJECT)]
_STYLE_ASK_BUTTONS = [("取消 ✖", _BTN_REJECT)]  # 问描述是开放式，只给取消


class SessionConsumer:
    def __init__(self, store: Any, driver: Any, transport: Any, chat_id: str,
                 incoming_root: Path, deliver_out_folder: Path,
                 jobs: "queue.Queue", events: "queue.Queue", readonly_client: Any,
                 now_fn: Callable[[], float] = time.time,
                 idle_reminder_seconds: float = 300.0,
                 progress_interval_seconds: float = 60.0,
                 eval_poll_interval_seconds: float = 60.0) -> None:
        self.store = store
        self.driver = driver  # 只用 cancel/approve 这类不碰 stages 的即时小操作
        self.transport = transport
        self.chat_id = chat_id
        self.incoming_root = Path(incoming_root)
        self.deliver_out_folder = Path(deliver_out_folder)
        self.jobs = jobs
        self.events = events
        self.readonly_client = readonly_client  # 只读亚秒级查询(eval 进度轮询)的唯一例外
        self.now_fn = now_fn
        self.idle_reminder_seconds = idle_reminder_seconds
        self.progress_interval_seconds = progress_interval_seconds
        self.eval_poll_interval_seconds = eval_poll_interval_seconds

        self.generation = 0
        self.run: Optional[RunState] = None
        self.view = SessionView(incoming_root=self.incoming_root)
        self.pending_texts: deque = deque()
        self.inflight: Optional[dict] = None  # {"type": kind, "text": 原文本}
        self.active_drive_job: Optional[DriveJob] = None
        self.cancelling_run_id: Optional[str] = None
        self._last_eval_poll_at: Optional[float] = None

    # -- 生命周期 --

    def bootstrap(self) -> None:
        """启动恢复（Eng Design 第七节第 7 条）。"""
        active = self.store.list_active()
        assert len(active) <= 1, f"单聊天单活跃 run 的既有约束被打破：{len(active)} 个"
        if not active:
            return
        run = active[0]
        if run.status == RunStatus.RUNNING:
            self.view = view_from_run(run, self.incoming_root)
            self._send("上次处理被中断，正在接着跑…")
            self._enqueue_drive("resume", run.run_id)
        elif run.status == RunStatus.AWAITING_REVIEW:
            # 旧 _dispatch 惰性 approve（下一条消息才收尾），启动时直接收掉。
            self.driver.approve(run)
        else:
            self._adopt(run)

    def step(self) -> None:
        for msg in self.transport.receive():
            self._handle_inbound(msg)
        self._drain_events()
        self._maybe_dispatch_next_text()
        self._check_timers()

    # -- 入站分派 --

    def _handle_inbound(self, msg: Any) -> None:
        if msg.chat_id != self.chat_id:
            print(f"[consumer] 忽略非白名单 chat_id={msg.chat_id}", flush=True)
            return
        print(f"[consumer] 收到 kind={msg.kind} text={(msg.text or '')!r} "
              f"file={msg.file_path!r} data={getattr(msg, 'data', None)!r} "
              f"status={self.view.status}", flush=True)
        if msg.kind == "callback":
            self._handle_callback(getattr(msg, "data", None))
            return
        if (self.view.status == RunStatus.RUNNING and not self.view.drive_active
                and self.active_drive_job is None and self.view.run_id is not None):
            # worker job 崩过、run 停在检查点：下一条用户消息触发续跑
            # （旧 _dispatch RUNNING 分支的语义；不自动重试防崩溃循环）。
            self._enqueue_drive("resume", self.view.run_id)
        if msg.kind in ("photo", "file"):
            self._handle_photo(msg)
            return
        text = (msg.text or "").strip()
        if not text:
            return
        if text.lower() in _REJECT_KEYWORDS:
            self._handle_cancel()
            return
        self._touch_activity()
        self.pending_texts.append(text)

    def _handle_photo(self, msg: Any) -> None:
        if self.view.drive_active:
            # RUNNING 是 2.0 新可达状态：对齐 AwaitingGate 的排队行为。
            if msg.file_path:
                queue_incoming_photo(self.incoming_root, msg.file_path)
            self._send("先帮你收着，这批处理完就接着看这些新照片")
            return
        if self.run is None:
            self._mint_collecting_run()
        self._touch_activity()
        if self.run.status in (RunStatus.COLLECTING, RunStatus.PLANNED):
            # 逐张不回复：一批多张照片连发不该刷屏（旧行为）。
            if msg.file_path:
                stage_incoming_photo(self.incoming_root, self.run.run_id, msg.file_path)
            self.store.save(self.run)
            return
        if self.run.status == RunStatus.AWAITING_GATE:
            if msg.file_path:
                queue_incoming_photo(self.incoming_root, msg.file_path)
            self._send("先帮你收着，这批处理完就接着看这些新照片")

    def _handle_cancel(self) -> None:
        """"取消"跳过文本串行队列的全局快路径（含旧实现缺失的 Collecting 态）。"""
        if self.view.drive_active and self.active_drive_job is not None:
            self._send("正在停下来...")
            self.active_drive_job.cancel_event.set()
            self.cancelling_run_id = self.view.run_id
            self._reset_session()
            return
        if self.run is not None:
            self.driver.cancel(self.run)  # Collecting 取消：已收照片随 run 废弃
            self._send("已取消")
            self._reset_session()
            return
        if self.view.status == RunStatus.RUNNING and self.view.run_id is not None:
            # worker job 崩后无主的 RUNNING run：直接标记取消，不再续跑。
            run = self.store.load(self.view.run_id)
            self.driver.cancel(run)
            self._send("已取消")
            self._reset_session()
            return
        self._send("现在没有在处理的批次")

    def _handle_callback(self, data: Optional[str]) -> None:
        """inline 按钮点击。callback_data 形如 "approve:tg-xxxxxxxx"，校验
        run_id 防误触旧消息里的按钮（换了 run 或已进入 drive 的旧按钮一律
        算过期）。批准/取消/重选映射到跟打字关键词完全一致的处理路径。"""
        action, _, run_id = (data or "").partition(":")
        print(f"[consumer] 按钮点击 action={action!r} run_id={run_id!r}", flush=True)
        if self.view.drive_active or self.run is None or self.view.run_id != run_id:
            self._send("这个选项已经过期了，看我最新的消息哈")
            return
        self._touch_activity()
        if action == _BTN_REJECT:
            self.driver.cancel(self.run)
            self._send("已取消")
            self._reset_session()
            return
        if action == _BTN_APPROVE:
            if self.run.status == RunStatus.PLANNED:
                self._begin_running()
                return
            if self.run.status == RunStatus.AWAITING_GATE:
                # StyleApplyAll 预览确认、Deliver 选图确认都是"批准即推进"。
                # Style 问描述那步没有批准按钮，不会走到这里。
                self._enqueue_drive("resolve_gate", self.run.run_id)
                return
        if action == _BTN_RESTYLE and self.run.status == RunStatus.AWAITING_GATE:
            # 重选：不带新描述，回到"等一句风格描述"，用户下一条文字即新描述。
            self._send("想要什么风格？直接打字告诉我，比如\"复古暖色调\"")
            return
        self._send("这个操作现在用不了，看我最新的消息哈")

    def _reset_session(self) -> None:
        self.generation += 1  # 之后到达的旧事件全部过期丢弃
        self.inflight = None
        self.pending_texts.clear()
        self.run = None
        self.view = SessionView(incoming_root=self.incoming_root)
        self.active_drive_job = None

    # -- 文本串行处理 --

    def _maybe_dispatch_next_text(self) -> None:
        while self.inflight is None and self.pending_texts:
            self._process_text(self.pending_texts.popleft())

    def _process_text(self, text: str) -> None:
        normalized = text.lower()
        if self.view.drive_active:
            # RUNNING 期间不做 LLM 分类（单 worker 正被 drive 占用，排队
            # 等于饿死）：模板应答足够回答"到哪了"。
            self._send(self.view.describe())
            return
        if self.run is None:
            self._mint_collecting_run()

        if self.run.status == RunStatus.COLLECTING:
            self._submit_classify("collecting", text, {"photo_count": self.view.photo_count()})
            return
        if self.run.status == RunStatus.PLANNED:
            if normalized in _APPROVE_KEYWORDS:
                self._begin_running()
                return
            self._submit_classify("refine_plan", text, {
                "intent_raw": self.run.intent_raw,
                "current_params": self._current_plan_params(self.run),
            })
            return
        if self.run.status == RunStatus.AWAITING_GATE:
            gate_stage = self.view.gate_stage
            if gate_stage is None and self.run.gate_state is not None:
                gate_stage = self.run.gate_state.stage_name
            if gate_stage in _STYLE_GATE_STAGES:
                self._handle_style_gate_text(gate_stage, text, normalized)
                return
            if normalized in _APPROVE_KEYWORDS:
                self._enqueue_drive("resolve_gate", self.run.run_id)
                return
            self._submit_classify("gate_reply", text, {"run_id": self.run.run_id})
            return
        self._send(self.view.describe())

    def _handle_style_gate_text(self, stage: str, text: str, normalized: str) -> None:
        # 风格闸门不走 LLM 分类：自由文本本身就是（新的）风格描述（旧
        # _handle_style_gate）。空文本在入站处已过滤，旧实现"空回车引导"
        # 分支在 2.0 不可达。
        if stage == "StyleApplyAll" and normalized in _APPROVE_KEYWORDS:
            self._enqueue_drive("resolve_gate", self.run.run_id)
            return
        self._send("正在重新选风格..." if stage == "StyleApplyAll" else "正在选风格...")
        self._enqueue_drive("rerun_style", self.run.run_id, {"style_description": text})

    # -- job 投递 --

    def _submit_classify(self, kind: str, text: str, context: dict) -> None:
        self.inflight = {"type": kind, "text": text}
        self._enqueue_job(ClassifyJob(generation=self.generation, kind=kind, text=text,
                                       context=context))

    def _submit_compose(self, intent_text: str) -> None:
        self.inflight = {"type": "compose", "text": intent_text}
        self._enqueue_job(ComposeJob(generation=self.generation, intent_text=intent_text))

    def _begin_running(self) -> None:
        self._send(f"开始处理了，共 {self.view.photo_count()} 张")
        run = self.run
        run.status = RunStatus.RUNNING
        self.store.save(run)
        self._enqueue_drive("start", run.run_id)

    def _enqueue_drive(self, action: str, run_id: str, args: Optional[dict] = None) -> None:
        job = DriveJob(generation=self.generation, action=action, run_id=run_id,
                        args=args or {})
        self.active_drive_job = job
        self.run = None  # 所有权交给 worker（交接靠落盘 + 事件，不共享对象）
        self.view.drive_active = True
        self.view.status = RunStatus.RUNNING
        self.view.gate_stage = None
        self.view.stage_progress = None
        self._last_eval_poll_at = None
        self._enqueue_job(job)

    # -- 事件应用 --

    def _drain_events(self) -> None:
        while True:
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                return
            stale = "" if event.generation == self.generation else " [过期丢弃]"
            print(f"[consumer] 事件 {type(event).__name__} gen={event.generation}{stale}", flush=True)
            self._apply_event(event)

    def _apply_event(self, event: Any) -> None:
        if (isinstance(event, RunFinished) and event.status == RunStatus.CANCELLED.value
                and event.run_id == self.cancelling_run_id):
            # 取消回执唯一例外地跨代接收：用户要的就是"真的停了"这句确认。
            self.cancelling_run_id = None
            self._send("已取消")
            return
        if event.generation != self.generation:
            return
        if isinstance(event, ClassifyDone):
            self._on_classify_done(event)
        elif isinstance(event, ClassifyFailed):
            self._on_classify_failed(event)
        elif isinstance(event, ComposeDone):
            self._on_compose_done(event)
        elif isinstance(event, ComposeFailed):
            self._on_compose_failed(event)
        elif isinstance(event, StageStarted):
            self._on_stage_started(event)
        elif isinstance(event, GateReached):
            self._on_gate_reached(event)
        elif isinstance(event, RunFinished):
            self._on_run_finished(event)
        elif isinstance(event, JobCrashed):
            self._on_job_crashed(event)

    def _on_classify_done(self, event: ClassifyDone) -> None:
        inflight, self.inflight = self.inflight, None
        if inflight is None or inflight["type"] != event.kind:
            return  # 防御：串行协议下不应发生
        result = event.result
        if event.kind == "collecting":
            if result.action == "query":
                self._send(self.view.describe())
            else:
                self._submit_compose(inflight["text"])
            return
        if event.kind == "refine_plan":
            self._on_refine_reply(result)
            return
        if event.kind == "gate_reply":
            self._on_gate_reply(result)

    def _on_classify_failed(self, event: ClassifyFailed) -> None:
        inflight, self.inflight = self.inflight, None
        if inflight is None:
            return
        if event.kind == "collecting":
            # 分类只是锦上添花，失败照旧当意图处理（旧降级路径）。
            self._submit_compose(inflight["text"])
            return
        # 旧实现只捕获 AdjustmentError，LlmRequestError 会让整条消息被主
        # 循环跳过（用户侧无声）；2.0 统一回引导文案，不再静默。
        if event.kind == "gate_reply":
            self._send("没听懂这句话，能再说清楚点吗？满意就说\"好的\"，不满意说说想怎么调，不要了就说\"取消\"")
            return
        self._send("没听懂，满意就说\"好的\"，不满意说说想怎么改，不要了就说\"取消\"")

    def _on_refine_reply(self, reply: Any) -> None:
        if self.run is None or self.run.status != RunStatus.PLANNED:
            return
        if reply.action == "clarify":
            self._send(reply.question)
            return
        if reply.action == "query":
            self._send(self.view.describe())
            return
        if reply.action == "approve":
            self._begin_running()
            return
        if reply.action == "reject":
            self.driver.cancel(self.run)
            self._send("已取消")
            self._reset_session()
            return
        # confirmed：更新参数、重新回显确认，不自动开跑（PLANNED 的存在
        # 意义就是"改完参数必须再看一眼"，旧拍板保持）。
        evaluate = next(s for s in self.run.plan.stages if s.name == "Evaluate")
        curate = next(s for s in self.run.plan.stages if s.name == "Curate")
        evaluate.params["provider"] = reply.provider
        evaluate.params["auto_reject"] = reply.auto_reject
        curate.params["count"] = reply.count
        curate.params["apply_tag"] = reply.apply_tag
        self.store.save(self.run)
        self.view.plan_summary = self._current_plan_params(self.run)
        self._send_plan_confirmation(self.run)

    def _on_gate_reply(self, reply: Any) -> None:
        if self.run is None or self.run.status != RunStatus.AWAITING_GATE:
            return
        if reply.action == "approve":
            self._enqueue_drive("resolve_gate", self.run.run_id)
            return
        if reply.action == "reject":
            self.driver.cancel(self.run)
            self._send("已取消")
            self._reset_session()
            return
        if reply.action == "query":
            self._send(self.view.describe())
            return
        self._enqueue_drive("adjustment", self.run.run_id, {"delta": reply.delta})

    def _on_compose_done(self, event: ComposeDone) -> None:
        inflight, self.inflight = self.inflight, None
        if inflight is None or self.run is None or self.run.status != RunStatus.COLLECTING:
            return
        plan = event.plan
        # 参数注入对齐旧 _propose_plan：Ingest 收图目录、Deliver 目的地 +
        # 必选闸门（两段式交付的关键决定）。
        ingest_spec = next(s for s in plan.stages if s.name == "Ingest")
        deliver_spec = next(s for s in plan.stages if s.name == "Deliver")
        ingest_spec.params["folder"] = str(incoming_dir_for(self.incoming_root, self.run.run_id))
        deliver_spec.params["out_folder"] = str(self.deliver_out_folder)
        deliver_spec.gate = "required"
        run = self.run
        run.plan = plan
        run.stage_states = {s.name: StageStatus.PENDING for s in plan.stages}
        run.intent_raw = inflight["text"]
        run.status = RunStatus.PLANNED
        self.store.save(run)
        self.view = view_from_run(run, self.incoming_root)
        self._send_plan_confirmation(run)

    def _on_compose_failed(self, event: ComposeFailed) -> None:
        self.inflight = None
        # 留在 Collecting，已收的照片不丢，下一条消息还能重试（旧行为）。
        if _looks_like_infra_error(event.message):
            self._send(f"AI 服务好像连不上，稍后再发一次试试（{event.message}）")
        else:
            self._send(f"没看懂这句意图，能换个说法再说一次吗？（{event.message}）")

    def _on_stage_started(self, event: StageStarted) -> None:
        self.view.current_stage = event.stage
        self.view.stage_progress = None
        if event.stage == "Evaluate":
            # 轮询基线设在 stage 开始时刻：第一次播报也等满一个间隔。
            self._last_eval_poll_at = self.now_fn()
        message = STAGE_PROGRESS_MESSAGES.get(event.stage)
        if message:
            self._send(message)

    def _on_gate_reached(self, event: GateReached) -> None:
        self.active_drive_job = None
        run = self.store.load(event.run_id)  # 所有权交回：从盘上取，不共享内存
        self.run = run
        self.view = view_from_run(run, self.incoming_root)
        if self.view.gate_stage is None:
            self.view.gate_stage = event.stage
        payload = event.payload
        if event.stage == "Style":
            self._send_buttons("想要什么风格？用一句话描述就行，比如\"复古暖色调\"", _STYLE_ASK_BUTTONS)
        elif event.stage == "StyleApplyAll":
            self._render_style_apply_all_gate(payload)
        else:
            self._render_deliver_gate(payload)

    def _render_style_apply_all_gate(self, payload: dict) -> None:
        chosen = payload.get("chosen_recipe")
        if not chosen:
            self._send("没能选出风格，直接说说想要什么风格吧")
            return
        if payload.get("export_error"):
            self._send(f"预览导出失败：{payload['export_error']}")
            return
        if not payload.get("preview_sent"):
            self._send("预览图发送失败，不过风格已经选好了")
        self._send_buttons(f"这是用「{chosen}」套用的效果，满意点\"满意\"，"
                           "想换点\"重选\"或直接打字说想要什么风格，不要了点\"取消\"",
                           _STYLE_APPLY_ALL_BUTTONS)

    def _render_deliver_gate(self, payload: dict) -> None:
        if payload.get("export_error"):
            self._send(f"预览导出失败：{payload['export_error']}")
            return
        summary = f"选好了 {payload.get('selected_count', 0)} 张"
        if payload.get("applied_recipe"):
            summary += f"，已套用风格「{payload['applied_recipe']}」"
        if payload.get("preview_failed_count"):
            summary += f"(其中 {payload['preview_failed_count']} 张预览发送失败，交付时仍会正常导出)"
        summary += "，满意点\"满意\"，想调整直接打字说，不要了点\"取消\""
        self._send_buttons(summary, _DELIVER_BUTTONS)

    def _on_run_finished(self, event: RunFinished) -> None:
        self.active_drive_job = None
        if event.status == RunStatus.FAILED.value:
            self._send(f"处理失败：{event.detail or '未知错误'}")
        # DONE 不发额外消息：Deliver stage 自己已说"选好了 N 张"（旧行为）。
        self.run = None
        self.view = SessionView(incoming_root=self.incoming_root)

    def _on_job_crashed(self, event: JobCrashed) -> None:
        # run 停在最后一次落盘检查点；视图保持 RUNNING，下一条用户消息触
        # 发 resume（见 _handle_inbound），不自动重试防崩溃循环。静默崩溃
        # 是最糟的失败模式，必须回一句话过去（真机反馈：脚本没输出、用户
        # 也不知道发生了什么）。
        print(f"[consumer] worker job 崩了（已兜底）：{event.error}", flush=True)
        was_driving = self.view.drive_active
        self.inflight = None
        self.active_drive_job = None
        self.view.drive_active = False
        if was_driving:
            self._send("处理过程中出了点问题，这批先停在这儿了，回句话我接着试")
        else:
            self._send("刚才那条没能处理，能再说一次吗？")

    # -- timers --

    def _check_timers(self) -> None:
        now = self.now_fn()
        self._check_idle_reminder(now)
        self._check_collecting_progress(now)
        self._check_eval_poll(now)

    def _check_idle_reminder(self, now: float) -> None:
        run = self.run
        if run is None or run.reminder_sent or run.last_activity_at is None:
            return
        if now - run.last_activity_at < self.idle_reminder_seconds:
            return
        if run.status == RunStatus.COLLECTING:
            self._send(f"看到你发了 {self.view.photo_count()} 张，想怎么处理？")
        elif run.status == RunStatus.PLANNED:
            self._send("还在等你确认要不要这么处理，满意就说\"好的\"")
        elif run.status == RunStatus.AWAITING_GATE:
            self._send("还在等你的回复呢，满意就说\"好的\"，不满意说说想怎么调，不要了就说\"取消\"")
        else:
            return
        run.reminder_sent = True
        self.store.save(run)

    def _check_collecting_progress(self, now: float) -> None:
        run = self.run
        if run is None or run.status != RunStatus.COLLECTING:
            return
        last = run.last_progress_notified_at
        if last is not None and now - last < self.progress_interval_seconds:
            return
        count = self.view.photo_count()
        if count == 0:
            return
        self._send(f"已收到 {count} 张图片")
        run.last_progress_notified_at = now
        self.store.save(run)

    def _check_eval_poll(self, now: float) -> None:
        """Evaluate 量化进度：轮询 `pzt images --json` 数 evaluated（零
        core/cli 改动，见 Eng Design 第七节第 6 条）。失败静默容忍。"""
        if not self.view.drive_active or self.view.current_stage != "Evaluate":
            return
        last = self._last_eval_poll_at
        if last is None:
            # 没见过 StageStarted(Evaluate)（重启续跑等），以现在为基线。
            self._last_eval_poll_at = now
            return
        if now - last < self.eval_poll_interval_seconds:
            return
        self._last_eval_poll_at = now
        project_id = self.view.project_id or self.view.run_id
        try:
            result = self.readonly_client.call("images", project_id)
        except Exception as e:  # noqa: BLE001 轮询失败无害，绝不打断 consumer
            print(f"[session] eval 进度轮询失败（容忍）：{e!r}")
            return
        images = result.get("images", [])
        done = sum(1 for item in images if item.get("evaluated"))
        self.view.stage_progress = (done, len(images))
        self._send(f"评估进行中，已完成 {done}/{len(images)} 张")

    # -- helpers --

    def _mint_collecting_run(self) -> None:
        run = new_collecting_run(new_run_id())
        run.last_activity_at = self.now_fn()
        run.last_progress_notified_at = self.now_fn()
        moved = drain_queue_into(self.incoming_root, run.run_id)
        self.store.save(run)
        if moved:
            self._send(f"之前排队的 {len(moved)} 张已经并进这一批了")
        self._adopt(run)

    def _adopt(self, run: RunState) -> None:
        self.run = run
        self.view = view_from_run(run, self.incoming_root)

    def _touch_activity(self) -> None:
        run = self.run
        if run is None:
            return
        if run.status in (RunStatus.COLLECTING, RunStatus.PLANNED, RunStatus.AWAITING_GATE):
            run.last_activity_at = self.now_fn()
            run.reminder_sent = False
            self.store.save(run)

    def _enqueue_job(self, job: Any) -> None:
        print(f"[consumer] 投递 {type(job).__name__} gen={job.generation}", flush=True)
        self.jobs.put(job)

    def _send(self, text: str) -> None:
        print(f"[consumer] 回复: {text!r}", flush=True)
        self.transport.send_text(self.chat_id, text)

    def _send_buttons(self, text: str, actions: list) -> None:
        """带 inline 按钮发一条确认。transport 没有 send_buttons 能力时
        （run_watchfolder 等非 Telegram 入口，或旧版 transport）降级成纯文
        本——打字关键词/自由文本那套仍然照常工作，不至于卡死。"""
        run_id = self.view.run_id or (self.run.run_id if self.run else None)
        send_buttons = getattr(self.transport, "send_buttons", None)
        if send_buttons is None or run_id is None:
            self._send(text)
            return
        options = [(label, f"{token}:{run_id}") for label, token in actions]
        print(f"[consumer] 回复(带按钮): {text!r} buttons={[l for l, _ in actions]}", flush=True)
        send_buttons(self.chat_id, text, options)

    def _current_plan_params(self, run: RunState) -> dict:
        evaluate = next(s for s in run.plan.stages if s.name == "Evaluate")
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        return {
            "provider": evaluate.params["provider"],
            "auto_reject": evaluate.params["auto_reject"],
            "count": curate.params["count"],
            "apply_tag": curate.params["apply_tag"],
        }

    def _send_plan_confirmation(self, run: RunState) -> None:
        # 不显示 provider（评估模型）：它由 Settings 决定，对用户是无用信息
        # （真机反馈）。仍保留在 plan 参数里，refine 想改 provider 仍可改。
        evaluate = next(s for s in run.plan.stages if s.name == "Evaluate")
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        auto_reject_desc = "自动剔除不合格照片" if evaluate.params["auto_reject"] else "不自动剔除照片"
        self._send_buttons(
            f"理解你想：留 {curate.params['count']} 张，标签叫\"{curate.params['apply_tag']}\"，"
            f"{auto_reject_desc}，对吗？"
            "\n满意点\"好的\"，想改直接打字说，不要了点\"取消\"",
            _CONFIRM_BUTTONS,
        )


def _looks_like_infra_error(message: str) -> bool:
    return any(hint in message for hint in _INFRA_ERROR_HINTS)
