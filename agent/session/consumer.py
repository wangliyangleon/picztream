"""SessionConsumer：2.0 运行时的消息线程（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第七节）：会话逻辑按状态分派，LLM 调用点全部走投 job + 等
事件（取代了早期单线程同步 router）。它是唯一渲染与发送对话文本的
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

import os
import queue
import shutil
import time
from collections import deque
from pathlib import Path
from typing import Any, Callable, Optional

from compose.style_matcher import describe_presets
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

# 真机反馈"彻底去文本精确匹配"：不再有任何 _APPROVE/_REJECT/_CONFIRM 之
# 类的关键词集，所有用户文本都交 LLM 分类（见各状态的 _submit_classify）。
# 唯一的确定性即时路径是 inline 按钮回调（callback_data token，不经文本）。

# LLM/网络这类基础设施失败的痕迹：命中就说"AI 连不上"而不是"没看懂"
# （后者会误导用户以为是自己表述的问题，见真机反馈）。
_INFRA_ERROR_HINTS = ("network_error", "http_error", "missing_api_key", "unknown_provider",
                       "timed out", "Connection", "connection")

# inline 按钮的动作 token。callback_data 拼成 "{token}:{run_id}"，点击回
# 来时校验 run_id 防误触旧消息（见 _handle_callback）。真机反馈：所有
# yes/no 确认点都改按钮，精确关键词匹配 + "非关键词一律当新风格描述" 太
# 容易踩坑（"不错"/"ok好的" 被当描述 -> hallucinate）。打字仍然照旧可
# 用（LLM 判批准/调整/重描述），按钮只是额外的无歧义快路径。
#
# 取消是"炸掉整批"的危险操作，**故意不放进任何确认按钮**（真机反馈：太
# 容易误点）。它靠 LLM 识别取消意图触发，且触发后先弹一道二次确认
# （[确认取消]/[不取消] 按钮，或打字由 LLM 判断），确认了才真的取消。
_BTN_APPROVE = "approve"
_BTN_RESTYLE = "restyle"
_BTN_CONFIRM_CANCEL = "confirm_cancel"
_BTN_KEEP = "keep"
_CONFIRM_BUTTONS = [("好的 ✅", _BTN_APPROVE)]
_DELIVER_BUTTONS = [("满意 ✅", _BTN_APPROVE), ("重选 🔄", _BTN_RESTYLE)]
_STYLE_APPLY_ALL_BUTTONS = [("满意 ✅", _BTN_APPROVE), ("重选 🔄", _BTN_RESTYLE)]
_CANCEL_CONFIRM_BUTTONS = [("确认取消 ⚠️", _BTN_CONFIRM_CANCEL), ("不取消", _BTN_KEEP)]

# Telegram /命令快路径（AG-16.2）：确定性、零 LLM、零延迟，随时可用，与按钮
# 互补（按钮只挂在闸门消息上）。这一份是单一来源——/help 文案和 bot 菜单注
# 册（run_telegram.main -> transport.register_commands）都读它。
BOT_COMMANDS = [
    ("status", "看当前进度到哪了"),
    ("cancel", "取消当前这批（会再确认一次）"),
    ("help", "看可用命令"),
]

# 重复文案收敛（AG-20）。
_MSG_EXPIRED = "这个选项已经过期了，看我最新的消息哈"
_MSG_NEED_INTENT = "还没告诉我想怎么处理呢，说一句吧，比如\"选3张发朋友圈\""


class SessionConsumer:
    def __init__(self, store: Any, driver: Any, transport: Any, chat_id: str,
                 incoming_root: Path, deliver_out_folder: Path,
                 classify_jobs: "queue.Queue", drive_jobs: "queue.Queue",
                 events: "queue.Queue", readonly_client: Any,
                 now_fn: Callable[[], float] = time.time,
                 idle_reminder_seconds: float = 300.0,
                 progress_interval_seconds: float = 60.0,
                 eval_poll_interval_seconds: float = 60.0,
                 send_retry_backoff_seconds: float = 1.0,
                 preview_root: Optional[Path] = None,
                 staging_dir: Optional[Path] = None,
                 marker_dir: Optional[Path] = None,
                 terminal_retention_seconds: float = 7 * 86400) -> None:
        self.store = store
        self.driver = driver  # 只用 cancel/approve 这类不碰 stages 的即时小操作
        self.transport = transport
        self.chat_id = chat_id
        self.incoming_root = Path(incoming_root)
        self.deliver_out_folder = Path(deliver_out_folder)
        self.classify_jobs = classify_jobs  # LLM lane：分类/编排
        self.drive_jobs = drive_jobs        # pzt lane：drive
        self.events = events
        self.readonly_client = readonly_client  # 只读亚秒级查询(eval 进度轮询)的唯一例外
        self.now_fn = now_fn
        self.idle_reminder_seconds = idle_reminder_seconds
        self.progress_interval_seconds = progress_interval_seconds
        self.eval_poll_interval_seconds = eval_poll_interval_seconds
        self.send_retry_backoff_seconds = send_retry_backoff_seconds
        # AG-14：终态即删该 run 的大文件（incoming/preview/staging/marker），
        # 保留 deliver-out 与 run JSON；启动低频清扫超 retention 的终态 run。
        self.preview_root = Path(preview_root) if preview_root else None
        self.staging_dir = Path(staging_dir) if staging_dir else None
        self.marker_dir = Path(marker_dir) if marker_dir else None
        self.terminal_retention_seconds = terminal_retention_seconds

        self.generation = 0
        self.run: Optional[RunState] = None
        self.view = SessionView(incoming_root=self.incoming_root)
        self.pending_texts: deque = deque()
        self.inflight: Optional[dict] = None  # {"type": kind, "text": 原文本}
        self.active_drive_job: Optional[DriveJob] = None
        self.cancelling_run_id: Optional[str] = None
        self._last_eval_poll_at: Optional[float] = None
        self._cancel_confirm_pending: bool = False
        # 进度消息原地编辑的 (message_id, last_text) 槽（AG-16.3）。
        self._collecting_progress: Optional[tuple] = None
        self._eval_progress: Optional[tuple] = None

    # -- 生命周期 --

    def bootstrap(self) -> None:
        """启动恢复（Eng Design 第七节第 7 条）+ 取消/崩溃竞态自愈（AG-12）
        + 低频清扫超龄终态 run（AG-14）。"""
        _terminal = (RunStatus.DONE, RunStatus.FAILED, RunStatus.CANCELLED)
        self._sweep_terminal_runs()
        # 曾被取消但 worker 没来得及收尾就崩了：补 cancel、不复活。标记无论如
        # 何都清掉，避免陈旧堆积。
        for run_id in self.store.list_cancelling():
            try:
                r = self.store.load(run_id)
            except FileNotFoundError:
                self.store.clear_cancelling(run_id)
                continue
            if r.status not in _terminal:
                print(f"[consumer] 启动自愈：{run_id} 曾被取消未收尾，补 cancel 不复活", flush=True)
                self.driver.cancel(r)
                self._cleanup_run_files(run_id)
            self.store.clear_cancelling(run_id)
        active = self.store.list_active()  # 上面 cancel 过的已终态、被排除
        if len(active) > 1:
            # 取消瞬间发新照片 mint 了新批、旧批还没被 worker 收尾等竞态会留下
            # 多个非终态 run。不再 assert 拒绝启动：保留 last_activity_at 最新
            # 的一个，其余直接 cancel 落盘。
            active.sort(key=lambda r: r.last_activity_at or 0, reverse=True)
            for r in active[1:]:
                print(f"[consumer] 启动自愈：多活跃 run，取消较旧的 {r.run_id}", flush=True)
                self.driver.cancel(r)
                self._cleanup_run_files(r.run_id)
            active = active[:1]
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

    def _sweep_terminal_runs(self) -> None:
        # 启动低频清扫：终态超过保留窗口的 run 连 JSON + pzt 项目 + 残留大文件
        # 一起清（AG-14）。deliver-out 与近期 run JSON 保留。best-effort：单个
        # run 清理失败不阻塞启动。
        now = self.now_fn()
        for run_id in self.store.terminal_runs_older_than(self.terminal_retention_seconds, now):
            try:
                self.readonly_client.call("delete", run_id, "--force")
            except Exception as e:  # noqa: BLE001 项目可能已不存在/删除失败，容忍
                print(f"[consumer] 清扫：pzt delete {run_id} 跳过（{e!r}）", flush=True)
            self._cleanup_run_files(run_id)
            self.store.delete_run(run_id)
            print(f"[consumer] 清扫：终态超 {self.terminal_retention_seconds:.0f}s 的 run {run_id} 已清", flush=True)

    def step(self) -> None:
        for msg in self.transport.receive():
            # per-item 隔离：一条消息处理炸了不连累同批其它消息（receive() 已
            # 把整批取成 list，不隔离的话故障半径是整批，AG-11）。
            try:
                self._handle_inbound(msg)
            except Exception as e:  # noqa: BLE001
                print(f"[consumer] 处理入站消息出错，已跳过该条：{e!r}", flush=True)
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
        if msg.kind == "text" and (msg.text or "").strip().startswith("/"):
            # /命令是确定性即时路径：不进 pending_texts、不触发 resume、不走
            # LLM（AG-16.2）。
            self._handle_command((msg.text or "").strip())
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
        # 全部文本走 LLM（含取消意图、二次确认的打字回复），串行入队，由
        # _process_text 按当前状态分派给对应分类器。没有任何关键词短路。
        self._touch_activity()
        self.pending_texts.append(text)

    def _handle_photo(self, msg: Any) -> None:
        # 取消二次确认挂起时又发照片：显然不想取消了，安全撤销待确认，与文本
        # other 分支的"安全撤销"语义对齐（AG-20）。
        self._cancel_confirm_pending = False
        if self.view.drive_active:
            # RUNNING 是 2.0 新可达状态：对齐 AwaitingGate 的排队行为。
            if msg.file_path:
                queue_incoming_photo(self.incoming_root, msg.file_path)
                self._discard_download(msg.file_path)
            self._send("先帮你收着，这批处理完就接着看这些新照片")
            return
        if self.run is None:
            self._mint_collecting_run()
        self._touch_activity()
        if self.run.status in (RunStatus.COLLECTING, RunStatus.PLANNED):
            # 逐张不回复：一批多张照片连发不该刷屏（旧行为）。
            if msg.file_path:
                stage_incoming_photo(self.incoming_root, self.run.run_id, msg.file_path)
                self._discard_download(msg.file_path)
            self.store.save(self.run)
            return
        if self.run.status == RunStatus.AWAITING_GATE:
            if msg.file_path:
                queue_incoming_photo(self.incoming_root, msg.file_path)
                self._discard_download(msg.file_path)
            self._send("先帮你收着，这批处理完就接着看这些新照片")

    def _has_active_batch(self) -> bool:
        return (self.view.drive_active or self.run is not None
                or (self.view.status == RunStatus.RUNNING and self.view.run_id is not None))

    def _prompt_cancel_confirmation(self) -> None:
        """打字"取消"不立即执行——先弹二次确认。取消会炸掉整批（照片 +
        处理结果都丢），是危险操作，故意让用户多确认一步（真机反馈）。"""
        if not self._has_active_batch():
            self._send("现在没有在处理的批次")
            return
        self._touch_activity()
        self._cancel_confirm_pending = True
        self._send_buttons(
            "确定要取消整批吗？取消后这批照片和已处理的结果都会作废。",
            _CANCEL_CONFIRM_BUTTONS,
        )

    def _do_cancel(self) -> None:
        """真正执行取消（已过二次确认）。覆盖 drive 中 / 持有 run / 崩后
        无主 RUNNING 三种情形。"""
        self._cancel_confirm_pending = False
        if self.view.drive_active and self.active_drive_job is not None:
            self._send("正在停下来...")
            self.active_drive_job.cancel_event.set()
            self.cancelling_run_id = self.view.run_id
            # 落 cancelling 标记：盘上 run 要等 worker 收尾才终态，worker 若在
            # 收尾前崩了，下次 bootstrap 靠这个标记补 cancel、不复活（AG-12）。
            if self.view.run_id is not None:
                self.store.mark_cancelling(self.view.run_id)
            self._reset_session()
            return
        if self.run is not None:
            self.driver.cancel(self.run)  # Collecting 取消：已收照片随 run 废弃
            self._cleanup_run_files(self.run.run_id)  # 终态即删大文件（AG-14）
            self._send("已取消")
            self._reset_session()
            return
        if self.view.status == RunStatus.RUNNING and self.view.run_id is not None:
            # worker job 崩后无主的 RUNNING run：直接标记取消，不再续跑。
            run = self.store.load(self.view.run_id)
            self.driver.cancel(run)
            self._cleanup_run_files(run.run_id)  # 终态即删大文件（AG-14）
            self._send("已取消")
            self._reset_session()
            return
        self._send("现在没有在处理的批次")

    def _handle_callback(self, data: Optional[str]) -> None:
        """inline 按钮点击。callback_data 形如 "approve:tg-xxxxxxxx"，校验
        run_id 防误触旧消息里的按钮。批准/重选映射到跟打字关键词完全一致
        的处理路径。取消的二次确认（confirm_cancel/keep）单独处理——它在
        drive 进行中也要能用（此时 run 归 worker，常规按钮的校验会拦掉）。"""
        action, _, run_id = (data or "").partition(":")
        print(f"[consumer] 按钮点击 action={action!r} run_id={run_id!r}", flush=True)

        if action in (_BTN_CONFIRM_CANCEL, _BTN_KEEP):
            self._resolve_cancel_confirmation_button(action, run_id)
            return

        if self.view.drive_active or self.run is None or self.view.run_id != run_id:
            self._send(_MSG_EXPIRED)
            return
        if self.inflight is not None:
            # 文本分类在途时点按钮会用旧参数抢跑（比如"改成6张"还没解析完就点
            # "好的"，refine 结果回来时 run 已易主、被静默丢弃）。让按钮也服从
            # 文本那套 FIFO 串行：提示稍等，分类落地后再点（AG-07）。cancel 二
            # 次确认按钮排在前面、不受此闸——它是 drive 中也要能用的逃生路径。
            self._send("上一条还在处理，稍等一下再点～")
            return
        self._touch_activity()
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
            # 重选：不带新内容，提示用户打字说想怎么改，下一条文字走既有路径
            # （StyleApplyAll -> rerun_style 换风格；Deliver -> gate_reply 调整）。
            gate = self.view.gate_stage or (
                self.run.gate_state.stage_name if self.run.gate_state else None)
            if gate == "StyleApplyAll":
                self._send("想要什么风格？直接打字告诉我，比如\"复古暖色调\"")
            else:
                self._send("想怎么调整？直接打字告诉我，比如\"换掉第3张\"、\"留5张\"")
            return
        self._send("这个操作现在用不了，看我最新的消息哈")

    def _resolve_cancel_confirmation_button(self, action: str, run_id: str) -> None:
        if not self._cancel_confirm_pending or (self.view.run_id or "") != run_id:
            self._send(_MSG_EXPIRED)
            return
        if action == _BTN_CONFIRM_CANCEL:
            self._do_cancel()
        else:  # _BTN_KEEP
            self._cancel_confirm_pending = False
            self._send("好，继续")

    def _cleanup_run_files(self, run_id: str) -> None:
        # 终态即删该 run 的大文件（原图/预览/暂存 + 交付幂等 marker），保留
        # deliver-out（最终图）与 run JSON（AG-14）。best-effort，不因删不掉炸掉。
        shutil.rmtree(self.incoming_root / run_id, ignore_errors=True)
        if self.preview_root is not None:
            shutil.rmtree(self.preview_root / run_id, ignore_errors=True)
        if self.staging_dir is not None:
            shutil.rmtree(self.staging_dir / run_id, ignore_errors=True)
        if self.marker_dir is not None:
            for m in self.marker_dir.glob(f"{run_id}-*.json"):
                m.unlink(missing_ok=True)

    @staticmethod
    def _discard_download(path: Optional[str]) -> None:
        # 照片入 incoming 后删掉 telegram-inbox 里的下载源，别让同一张长期落两
        # 份（AG-14）。删不掉（并发/权限）不致命，忽略。
        if not path:
            return
        try:
            os.unlink(path)
        except OSError:
            pass

    def _reset_session(self) -> None:
        self.generation += 1  # 之后到达的旧事件全部过期丢弃
        self.inflight = None
        self.pending_texts.clear()
        self.run = None
        self.view = SessionView(incoming_root=self.incoming_root)
        self.active_drive_job = None
        self._cancel_confirm_pending = False
        self._collecting_progress = None  # 进度消息槽随会话重置（AG-16.3）
        self._eval_progress = None

    # -- 文本串行处理 --

    def _maybe_dispatch_next_text(self) -> None:
        while self.inflight is None and self.pending_texts:
            self._process_text(self.pending_texts.popleft())

    def _process_text(self, text: str) -> None:
        if self._cancel_confirm_pending:
            # 二次确认的打字回复交 LLM 判 confirm/deny/other（按钮仍是即时
            # 确定性路径，见 _handle_callback）。
            self._submit_classify("cancel_confirm", text, {})
            return
        if self.view.drive_active:
            # 处理中：交 running 分类器判 cancel/query/other。这是拆双 lane
            # 的意义所在——drive 占着 pzt lane，classify lane 仍空闲。
            self._submit_classify("running", text, {})
            return
        if self.run is None:
            # 还没发过照片（run is None ⟺ 无暂存照片）：不 mint，交 collecting
            # 分类器决定回什么（见 _on_classify_done 的 run is None 分支），
            # 避免因为一句"取消"/"你好"就凭空建一个空批次。
            self._submit_classify("collecting", text, {"photo_count": 0})
            return

        if self.run.status == RunStatus.COLLECTING:
            self._submit_classify("collecting", text, {"photo_count": self.view.photo_count()})
            return
        if self.run.status == RunStatus.PLANNED:
            self._submit_classify("refine_plan", text, {
                "intent_raw": self.run.intent_raw,
                "current_params": self._current_plan_params(self.run),
            })
            return
        if self.run.status == RunStatus.AWAITING_GATE:
            gate_stage = self.view.gate_stage
            if gate_stage is None and self.run.gate_state is not None:
                gate_stage = self.run.gate_state.stage_name
            if gate_stage == "Style":
                # 问描述那步：交 style_describe 分类器判 describe/skip/cancel/query
                # （AG-01/AG-02）。不再把"算了不弄了"/"有哪些风格"当风格描述。
                self._submit_classify("style_describe", text, {"run_id": self.run.run_id})
                return
            if gate_stage == "StyleApplyAll":
                # 预览确认：交 style_gate 分类器判 approve/redescribe/cancel/query。
                self._submit_classify("style_gate", text, {"run_id": self.run.run_id})
                return
            self._submit_classify("gate_reply", text, {"run_id": self.run.run_id})
            return
        self._send(self.view.describe())

    # -- job 投递 --

    def _submit_classify(self, kind: str, text: str, context: dict) -> None:
        self.inflight = {"type": kind, "text": text}
        self._enqueue_classify(ClassifyJob(generation=self.generation, kind=kind, text=text,
                                            context=context))

    def _submit_compose(self, intent_text: str) -> None:
        self.inflight = {"type": "compose", "text": intent_text}
        self._enqueue_classify(ComposeJob(generation=self.generation, intent_text=intent_text))

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
        self._enqueue_drive_job(job)

    # -- 事件应用 --

    def _drain_events(self) -> None:
        while True:
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                return
            stale = "" if event.generation == self.generation else " [过期丢弃]"
            print(f"[consumer] 事件 {type(event).__name__} gen={event.generation}{stale}", flush=True)
            # per-item 隔离：事件已出队，处理中途炸了不连累同批其它事件（否则
            # GateReached 发文案失败会把后续事件一起吞掉，用户只能靠 idle 兜底，AG-11）。
            try:
                self._apply_event(event)
            except Exception as e:  # noqa: BLE001
                print(f"[consumer] 应用事件 {type(event).__name__} 出错，已跳过：{e!r}", flush=True)

    def _apply_event(self, event: Any) -> None:
        if (isinstance(event, RunFinished) and event.status == RunStatus.CANCELLED.value
                and event.run_id == self.cancelling_run_id):
            # 取消回执唯一例外地跨代接收：用户要的就是"真的停了"这句确认。
            self.cancelling_run_id = None
            self.store.clear_cancelling(event.run_id)  # 正常收尾即清标记（AG-12）
            self._cleanup_run_files(event.run_id)      # 终态即删大文件（AG-14）
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
            self._on_collecting_reply(result, inflight["text"])
            return
        if event.kind == "refine_plan":
            self._on_refine_reply(result)
            return
        if event.kind == "gate_reply":
            self._on_gate_reply(result)
            return
        if event.kind == "style_describe":
            self._on_style_describe_reply(result, inflight["text"])
            return
        if event.kind == "style_gate":
            self._on_style_gate_reply(result, inflight["text"])
            return
        if event.kind == "running":
            self._on_running_reply(result)
            return
        if event.kind == "cancel_confirm":
            self._on_cancel_confirm_reply(result, inflight["text"])
            return

    @staticmethod
    def _merge_intent(existing: Optional[str], new: str) -> str:
        # 草稿意图 + 补充意图拼接（compose_plan 的 prompt 天然消化多句意图）。
        existing = (existing or "").strip()
        return f"{existing}；{new}" if existing else new

    def _on_collecting_reply(self, reply: Any, text: str) -> None:
        # collecting -> planned 的转变有三条路（真机反馈"意图先于照片会被丢"）：
        # ① 有照片后直接说完整意图 -> compose；② 有草稿方案（intent_raw）后
        # 一句"开始" -> 用草稿 compose；③ 超时没新图 -> 见 _check_idle_reminder。
        if self.run is None:
            # 还没发照片（run is None ⟺ 无暂存照片）
            if reply.action == "intent":
                self._mint_collecting_run(draft_intent=text)  # 记草稿，等照片
            elif reply.action == "start":
                self._send(_MSG_NEED_INTENT)
            elif reply.action == "cancel":
                self._send("现在没有在处理的批次")
            else:
                self._send_help()
            return
        # 已有 run（COLLECTING）——注意可能仍是 0 照片的草稿态（意图先到）
        if reply.action == "intent":
            if self.view.photo_count() == 0:
                # 还没照片：并入草稿，不用 0 张去组方案（pzt new 空目录会失败，
                # 且新意图不该整句覆盖旧草稿的约束）（AG-08）。
                self.run.intent_raw = self._merge_intent(self.run.intent_raw, text)
                self.store.save(self.run)
                self._send(f"好的，记下了。把照片发给我，发完说一声（或直接说\"开始\"）"
                           f"我就按\"{self.run.intent_raw}\"来～")
            else:
                # 有照片：已有草稿则拼上新句子再组方案，否则这句本身即完整意图。
                self._submit_compose(self._merge_intent(self.run.intent_raw, text))
        elif reply.action == "start":
            if not self.run.intent_raw:
                self._send(_MSG_NEED_INTENT)
            elif self.view.photo_count() == 0:
                self._send(f"还没收到照片哦，发几张我就按\"{self.run.intent_raw}\"开始～")
            else:
                self._submit_compose(self.run.intent_raw)  # 用草稿方案开跑
        elif reply.action == "query":
            self._send(self.view.describe())
        elif reply.action == "cancel":
            self._prompt_cancel_confirmation()
        else:  # other：打招呼/闲聊/听不懂，给帮助，别硬编默认方案
            self._send_help()

    def _on_style_describe_reply(self, reply: Any, text: str) -> None:
        # 问描述闸门（还没预览）。describe→拿这句当描述重跑 Style；skip→原图
        # 直出（空描述空跑）；cancel→二次确认；query→列出可选 preset（AG-01/
        # AG-02/AG-16）。
        if self.run is None or self.run.status != RunStatus.AWAITING_GATE:
            return
        if reply.action == "cancel":
            self._prompt_cancel_confirmation()
            return
        if reply.action == "query":
            self._send(describe_presets())
            return
        if reply.action == "skip":
            self._send("好，这批不套滤镜，用原图直出～")
            self._enqueue_drive("rerun_style", self.run.run_id, {"style_description": ""})
            return
        self._send("正在选风格...")
        self._enqueue_drive("rerun_style", self.run.run_id, {"style_description": text})

    def _on_style_gate_reply(self, reply: Any, text: str) -> None:
        # 预览确认闸门。approve→套全批；redescribe→拿这句当新描述重挑；
        # cancel→二次确认；query→报状态。redescribe 用原文本当风格描述。
        if self.run is None or self.run.status != RunStatus.AWAITING_GATE:
            return
        if reply.action == "approve":
            self._enqueue_drive("resolve_gate", self.run.run_id)
            return
        if reply.action == "cancel":
            self._prompt_cancel_confirmation()
            return
        if reply.action == "query":
            self._send(self.view.describe())
            return
        self._send("正在重新选风格...")
        self._enqueue_drive("rerun_style", self.run.run_id, {"style_description": text})

    def _on_running_reply(self, reply: Any) -> None:
        # 处理中：只区分取消/问进度/其它，都不打断 drive。
        if reply.action == "cancel":
            self._prompt_cancel_confirmation()
            return
        self._send(self.view.describe())  # query / other 都回当前进度

    def _on_cancel_confirm_reply(self, reply: Any, text: str) -> None:
        if not self._cancel_confirm_pending:
            return  # 期间已被按钮解决或会话重置
        if reply.action == "confirm":
            self._do_cancel()
            return
        if reply.action == "deny":
            self._cancel_confirm_pending = False
            self._send("好，继续")
            return
        # other：没明确确认，安全起见撤掉待确认、不取消，把这条当普通消息重
        # 新处理（用户很可能改了主意、直接发了新指令）。
        self._cancel_confirm_pending = False
        self.pending_texts.appendleft(text)

    def _on_classify_failed(self, event: ClassifyFailed) -> None:
        inflight, self.inflight = self.inflight, None
        if inflight is None:
            return
        if event.kind == "collecting":
            if self.run is None:
                self._send_help()  # 没照片还没法处理，给帮助
                return
            # 分类只是锦上添花，失败照旧当意图处理（旧降级路径）。
            self._submit_compose(inflight["text"])
            return
        if event.kind == "cancel_confirm":
            # 取消确认分类失败：安全起见当作"没确认"，撤掉待确认、重新处理。
            self._cancel_confirm_pending = False
            self.pending_texts.appendleft(inflight["text"])
            return
        if event.kind == "running":
            self._send(self.view.describe())  # 分类失败就回进度
            return
        # 下面几支默认回"没听懂"，但基础设施故障(Ollama 连不上等)不是表达问
        # 题，别把服务故障误导成"没看懂"（真机反馈刻意区分，见 AG-10）。
        # retryable=True 统一回可重试提示；上面 collecting/cancel_confirm/
        # running 三支的降级不是"没听懂"文案、对 infra 已自洽，不动。
        if event.retryable:
            self._send("AI 服务好像连不上，稍后再试一下～")
            return
        if event.kind in ("style_describe", "style_gate"):
            # 分类失败：退回"当作新风格描述"（历史默认），不卡住用户。
            if self.run is not None and self.run.status == RunStatus.AWAITING_GATE:
                self._send("正在选风格...")
                self._enqueue_drive("rerun_style", self.run.run_id,
                                     {"style_description": inflight["text"]})
            return
        if event.kind == "gate_reply":
            self._send("没听懂这句话，能再说清楚点吗？满意就点\"满意\"，想调整直接说想怎么调")
            return
        self._send("没听懂，满意就点\"好的\"，想改直接说想怎么改")

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
            self._prompt_cancel_confirmation()  # LLM 判成取消也要二次确认
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
            self._prompt_cancel_confirmation()  # LLM 判成取消也要二次确认
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
            self._eval_progress = None  # 新一轮评估新进度消息（AG-16.3）
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
        # 闸门弹出＝现在才开始"等用户回复"，把 idle 计时器归零。否则跑了
        # 几分钟 eval 之后，last_activity_at 停在用户上一条消息的时刻、早
        # 已超时，闸门提问后会立刻又追一句 idle 提醒（真机反馈）。
        self._touch_activity()
        payload = event.payload
        if event.stage == "Style":
            # 问描述是开放式，只能打字，不挂按钮。
            if payload.get("match_failed"):
                # 上一句描述没匹配上任何 preset：原地重问，不报废整批（AG-01）。
                self._send("没能选出对应的风格，换个说法再说说？比如\"复古暖色调\"、"
                           "\"黑白胶片\"；不想套滤镜就说\"原图就行\"")
                return
            style_spec = next((s for s in run.plan.stages if s.name == "Style"), None)
            prev = style_spec.params.get("style_description") if style_spec else None
            if prev is not None:
                # 调整选片后 Style 被 apply_adjustment 连带重置，但风格早已答过
                # （style_description key 只有答过才有）：不重问，按上次的重套
                # 用（AG-04）。想换仍可在 StyleApplyAll 预览闸门重选。
                self._send(f"还按「{prev}」重新套用" if prev.strip() else "这批还是不套滤镜，用原图～")
                self._enqueue_drive("rerun_style", run.run_id, {"style_description": prev})
                return
            self._send("想要什么风格？用一句话描述就行，比如\"复古暖色调\"")
            return
        if event.stage == "StyleApplyAll":
            self._render_style_apply_all_gate(payload)
        else:
            self._render_deliver_gate(payload)

    def _render_style_apply_all_gate(self, payload: dict) -> None:
        chosen = payload.get("chosen_recipe")
        if not chosen:
            # 无风格（用户选了原图直出）：没有预览可确认，直接推进到交付闸门。
            # 匹配失败现在已在 Style 闸门就拦下重问，这里 chosen=None 只可能是
            # skip（AG-16.1）。
            self._send("这批不套滤镜，直接看选片吧")
            self._enqueue_drive("resolve_gate", self.run.run_id)
            return
        if payload.get("export_error"):
            self._send(f"预览导出失败：{payload['export_error']}")
            return
        if not payload.get("preview_sent"):
            self._send("预览图发送失败，不过风格已经选好了")
        self._send_buttons(f"这是用「{chosen}」套用的效果，满意点\"满意\"，"
                           "想换风格点\"重选\"或直接打字描述",
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
        summary += "，满意点\"满意\"，想调整点\"重选\"或直接打字说"
        self._send_buttons(summary, _DELIVER_BUTTONS)

    def _on_run_finished(self, event: RunFinished) -> None:
        self.active_drive_job = None
        if event.status == RunStatus.FAILED.value:
            self._send(f"处理失败：{event.detail or '未知错误'}")
        elif event.status == RunStatus.DONE.value:
            # Deliver stage 自己已说"选好了 N 张"，这里补一句收尾，明确告诉
            # 用户这批结束了、可以开新的（真机反馈）。
            self._send("这批就处理完啦～想开新的一批，随时把照片发给我就行 📷")
        self._cleanup_run_files(event.run_id)  # 终态即删大文件（AG-14）
        self.run = None
        self.view = SessionView(incoming_root=self.incoming_root)

    def _on_job_crashed(self, event: JobCrashed) -> None:
        # 静默崩溃是最糟的失败模式（用户和终端都看不到），必须回一句话过去
        # （真机反馈）。只清崩掉那条 lane 的状态：两条 lane 并发，动了没崩的
        # 那条会连带副作用（drive 误触 resume 排双 job / classify 那条文本没
        # 有任何回复）。lane 由 worker 从 job 类型判定，不再靠 view 猜。
        print(f"[consumer] worker job 崩了（已兜底，lane={event.lane}）：{event.error}", flush=True)
        if event.lane == "drive":
            # drive lane 崩了：run 停在最后一次落盘检查点，视图退出 RUNNING，
            # 下一条用户消息触发 resume（见 _handle_inbound），不自动重试防崩
            # 溃循环。不碰 inflight——classify lane 若有在途分类与本次崩溃无关。
            self.active_drive_job = None
            self.view.drive_active = False
            self._send("处理过程中出了点问题，这批先停在这儿了，回句话我接着试")
        else:
            # classify lane 崩了：只清在途分类。drive lane（若在跑）是好的，
            # 动它的状态会误触 resume 与真 DriveJob 排两个（预览重发、闸门重问）。
            self.inflight = None
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
            count = self.view.photo_count()
            if run.intent_raw and count > 0 and self.inflight is None:
                # 有草稿方案 + 有照片 + 一段时间没新图：直接按草稿组方案，交
                # 给用户在 PLANNED 确认（草稿三条转变路径之一，见真机反馈）。
                self._submit_compose(run.intent_raw)
            elif run.intent_raw and count == 0:
                self._send(f"还没收到照片哦，发几张我就按\"{run.intent_raw}\"开始～")
            else:
                self._send(f"看到你发了 {count} 张，想怎么处理？")
        elif run.status == RunStatus.PLANNED:
            self._send("还在等你确认要不要这么处理，满意就点\"好的\"")
        elif run.status == RunStatus.AWAITING_GATE:
            # Style 问描述那步没有按钮，别提示"点按钮"（真机反馈）。
            gate = self.view.gate_stage or (run.gate_state.stage_name if run.gate_state else None)
            if gate == "Style":
                self._send("还在等你说想要什么风格呢，一句话描述就行，比如\"复古暖色调\"")
            else:
                self._send("还在等你的回复呢，满意就点按钮，想调整直接打字说")
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
        self._send_progress(f"已收到 {count} 张图片", "_collecting_progress")
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
        self._send_progress(f"评估进行中，已完成 {done}/{len(images)} 张", "_eval_progress")

    # -- helpers --

    def _mint_collecting_run(self, draft_intent: str = "") -> None:
        self._collecting_progress = None  # 新批新进度消息（AG-16.3）
        run = new_collecting_run(new_run_id())
        run.last_activity_at = self.now_fn()
        run.last_progress_notified_at = self.now_fn()
        run.intent_raw = draft_intent  # 草稿方案：意图先于照片到达时先记下
        moved = drain_queue_into(self.incoming_root, run.run_id)
        self.store.save(run)
        self._adopt(run)
        # 一批的开始立刻回一句确认：初始那波照片在后台逐张下载时之前是完
        # 全静默的（尤其图多时延迟明显），给用户一个"收到、任务开始了"的
        # 即时反馈（真机反馈）。只在新建 run 时发一次，后续照片仍逐张不回
        # 复、不刷屏。
        if draft_intent:
            # 意图先来、照片还没来：记下草稿，等照片 + 一句"开始"或超时再组方案。
            self._send(f"好的，记下了。把照片发给我，发完说一声（或直接说\"开始\"）"
                       f"我就按\"{draft_intent}\"来～")
        elif moved:
            self._send(f"收到～新任务开始了！之前排队的 {len(moved)} 张也并进这一批了，"
                       "照片尽管发，发完告诉我想怎么处理就行")
        else:
            self._send("收到～新任务开始了！照片尽管发，发完告诉我想怎么处理就行，"
                       "比如\"选3张发朋友圈\"")

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

    def _enqueue_classify(self, job: Any) -> None:
        print(f"[consumer] 投递(classify lane) {type(job).__name__} gen={job.generation}", flush=True)
        self.classify_jobs.put(job)

    def _enqueue_drive_job(self, job: Any) -> None:
        print(f"[consumer] 投递(drive lane) {type(job).__name__} gen={job.generation}", flush=True)
        self.drive_jobs.put(job)

    def _send(self, text: str) -> Optional[str]:
        print(f"[consumer] 回复: {text!r}", flush=True)
        # Telegram 抖动/超时是真机常见故障：退避重试一次后放弃，不外抛（否则
        # 会把整轮 step 拖挂、连累同批消息，AG-11）。不追求消息不丢的强保证。
        # 返回 message_id（progress 原地编辑用，AG-16.3），失败/不支持则 None。
        try:
            return self.transport.send_text(self.chat_id, text)
        except Exception as e:  # noqa: BLE001
            print(f"[consumer] send_text 失败，退避重试一次：{e!r}", flush=True)
            time.sleep(self.send_retry_backoff_seconds)
            try:
                return self.transport.send_text(self.chat_id, text)
            except Exception as e2:  # noqa: BLE001
                print(f"[consumer] send_text 重试仍失败，放弃这条：{e2!r}", flush=True)
                return None

    def _send_progress(self, text: str, slot: str) -> None:
        # 进度播报原地编辑（AG-16.3）：同一批的进度只占一条消息。slot 是存
        # (message_id, last_text) 的实例属性名。
        prev = getattr(self, slot)
        if prev is not None and prev[1] == text:
            return  # 内容没变，不刷不编辑（省掉 Telegram "message is not modified"）
        edit = getattr(self.transport, "edit_text", None)
        if edit is not None and prev is not None:
            try:
                edit(self.chat_id, prev[0], text)
                setattr(self, slot, (prev[0], text))
                return
            except Exception as e:  # noqa: BLE001 消息太老/被删等 -> 降级发新
                print(f"[consumer] 进度消息编辑失败，改发新的：{e!r}", flush=True)
        mid = self._send(text)
        setattr(self, slot, (mid, text) if mid is not None else None)

    def _handle_command(self, text: str) -> None:
        # /命令快路径（AG-16.2）。取首 token、去 @botname 后缀、小写。
        cmd = text.split()[0].split("@", 1)[0].lower()
        if cmd in ("/help", "/start"):
            self._send(self._command_help_text())
        elif cmd == "/status":
            self._send(self._status_text())
        elif cmd == "/cancel":
            # 取消是炸整批的危险操作，沿用全局二次确认（无活跃批次时它自己回
            # "现在没有在处理的批次"）。
            self._prompt_cancel_confirmation()
        else:
            self._send("没有这个命令哦，发 /help 看看能用哪些～")

    def _status_text(self) -> str:
        if not self._has_active_batch():
            return "现在没有在处理的批次～把照片发给我就能开始"
        return self.view.describe()

    @staticmethod
    def _command_help_text() -> str:
        lines = "\n".join(f"/{name} - {desc}" for name, desc in BOT_COMMANDS)
        return ("可以随时发这些命令：\n" + lines +
                "\n\n平时把照片发给我、再说一句想怎么处理就行，比如\"选3张发朋友圈\"。")

    def _send_help(self) -> None:
        self._send(
            "我是帮你选照片的小助手 📷\n"
            "把要处理的照片发给我，再用一句话说想怎么弄就行，比如：\n"
            "· 选3张发朋友圈\n"
            "· 挑5张精修\n"
            "· 筛一下，糊的去掉\n"
            "发完照片说一声，我就开始～"
        )

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
        # 去重（Dedup）总会做，跟 auto_reject 无关；auto_reject 只控制"要
        # 不要连不合格的也一起剔"。所以两个分支都点明"重复照片会去掉"。
        auto_reject_desc = ("自动剔除不合格和重复的照片" if evaluate.params["auto_reject"]
                            else "只去重复、保留不合格的照片")
        self._send_buttons(
            f"理解你想：留 {curate.params['count']} 张，标签叫\"{curate.params['apply_tag']}\"，"
            f"{auto_reject_desc}，对吗？"
            "\n满意就点\"好的\"，想改直接打字说",
            _CONFIRM_BUTTONS,
        )


def _looks_like_infra_error(message: str) -> bool:
    return any(hint in message for hint in _INFRA_ERROR_HINTS)
