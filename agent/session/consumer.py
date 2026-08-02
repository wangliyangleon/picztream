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

import logging
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
    RunRewound,
    StageCost,
    StageProgress,
    StageStarted,
)
from session.view import (
    STAGE_PROGRESS_MESSAGES,
    SessionView,
    describe_ai_cost,
    describe_cancel_partial,
    describe_progress_done,
    view_from_run,
)

_log = logging.getLogger("pzt.agent.consumer")

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
# 票 10：AI 开销告知那条消息上的"停下"。它**不是**上面说的那种确认按钮
# （不跟"好的✅"并排、不挂在闸门上），点它只是打开既有的二次确认，误点
# 仍然被那一道拦住 - 危险的是"一下就炸掉整批"，不是"有地方能发起取
# 消"。没有这个入口的话，这条消息就只是通知，G5 在 Telegram 上仍然不
# 可达。
_BTN_STOP = "stop"
_BTN_KEEP = "keep"
_BTN_SKIP_CURATE = "skip_curate"
_BTN_AI_DEDUP = "ai_dedup"
_BTN_AI_CURATE = "ai_curate"
_BTN_AI_NARROW = "ai_narrow"
_CONFIRM_BUTTONS = [("好的 ✅", _BTN_APPROVE)]
_DELIVER_BUTTONS = [("满意 ✅", _BTN_APPROVE), ("重选 🔄", _BTN_RESTYLE)]
_STYLE_APPLY_ALL_BUTTONS = [("满意 ✅", _BTN_APPROVE), ("重选 🔄", _BTN_RESTYLE)]
_CANCEL_CONFIRM_BUTTONS = [("确认取消 ⚠️", _BTN_CONFIRM_CANCEL), ("不取消", _BTN_KEEP)]
_DEDUP_FOLLOWUP_BUTTONS = [("不筛选了", _BTN_SKIP_CURATE)]
_COST_BUTTONS = [("停下 ⛔", _BTN_STOP)]

# Telegram /命令快路径（AG-16.2）：确定性、零 LLM、零延迟，随时可用，与按钮
# 互补（按钮只挂在闸门消息上）。这一份是单一来源——/help 文案和 bot 菜单注
# 册（run_telegram.main -> transport.register_commands）都读它。
BOT_COMMANDS = [
    ("status", "看当前进度到哪了"),
    ("cancel", "取消当前这批（会再确认一次）"),
    ("help", "看可用命令"),
]

# 引导用户说出意图的示例文案，下面五处共用这一份（票 02 收敛，票 08 扩写）。
#
# 之所以带一个 verb 参数而不是光秃秃一个常量：去重追问那一处的上文是"告诉我
# 留几张"，例子跟着用"留"，其余四处用"选"；除这个动词外五处逐字相同。
#
# 票 08（PRD 决策三）：例子必须自己示范题材偏好，否则用户永远只说"选3张发朋
# 友圈"，抽出来的选片简述就是空的，整条按描述选片的通路是死的 - 机器有能力读
# 懂"有景有人、表情活泼"，但没人会主动这么说。张数写成"三"而不是"3"是跟着这
# 句话的口语调子走的，compose_plan 的提示词里已经点名中文数字也算数。
def _intent_hint_example(verb: str = "选") -> str:
    return f"{verb}三张有景有人、表情活泼的照片发朋友圈"


# 重复文案收敛（AG-20）。
_MSG_EXPIRED = "这个选项已经过期了，看我最新的消息哈"
_MSG_NEED_INTENT = f"还没告诉我想怎么处理呢，说一句吧，比如\"{_intent_hint_example()}\""


class SessionConsumer:
    def __init__(self, store: Any, driver: Any, transport: Any, chat_id: str,
                 incoming_root: Path, deliver_out_folder: Path,
                 classify_jobs: "queue.Queue", drive_jobs: "queue.Queue",
                 events: "queue.Queue", readonly_client: Any,
                 now_fn: Callable[[], float] = time.time,
                 idle_reminder_seconds: float = 300.0,
                 progress_interval_seconds: float = 60.0,
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
        self.readonly_client = readonly_client  # 只读查询用的独立实例(如终态清扫时的 pzt delete)，不跟 worker 专属 client 共享
        self.now_fn = now_fn
        self.idle_reminder_seconds = idle_reminder_seconds
        self.progress_interval_seconds = progress_interval_seconds
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
        self._cancel_confirm_pending: bool = False
        # 进度消息原地编辑的 (message_id, last_text) 槽（AG-16.3）。
        self._collecting_progress: Optional[tuple] = None
        # 运行期进度（T-8）用的第二个槽 + 上次真的发出去的时刻。两者都在
        # StageStarted 时清零：每个 stage 一条自己的进度消息，且第一条不
        # 等节流窗口。都是纯内存态，不落盘 —— DriveJob 期间 run 的所有权
        # 在 worker 手上，consumer 写不了 run.last_progress_notified_at。
        self._stage_progress: Optional[tuple] = None
        self._stage_progress_notified_at: Optional[float] = None
        # 这一批里已经报过一次 AI 开销（票 10）。真机上 Dedup 先报、Curate
        # 再报，第二条要读成接续而不是重复。随会话重置。
        self._cost_announced: bool = False
        # 去重追问打字给了数量后，等用户确认才真正执行——纯内存态、不落
        # 盘，跟 _cancel_confirm_pending 同一个先例（真机反馈，见目标三）。
        self._curate_narrow_pending: Optional[dict] = None
        # Style 闸门拆两阶段：先确认选片结果（真正筛选过才有），再问风格
        # 描述。True＝还在等选片确认，此时打字/按钮走 gate_reply 语义
        # （真机反馈：选片确认要放在滤镜之前）。
        self._pending_selection_approval: bool = False

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
                _log.info(f"[consumer] 启动自愈：{run_id} 曾被取消未收尾，补 cancel 不复活")
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
                _log.info(f"[consumer] 启动自愈：多活跃 run，取消较旧的 {r.run_id}")
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
                _log.warning(f"[consumer] 清扫：pzt delete {run_id} 跳过（{e!r}）")
            self._cleanup_run_files(run_id)
            self.store.delete_run(run_id)
            _log.info(f"[consumer] 清扫：终态超 {self.terminal_retention_seconds:.0f}s 的 run {run_id} 已清")

    def step(self) -> None:
        for msg in self.transport.receive():
            # per-item 隔离：一条消息处理炸了不连累同批其它消息（receive() 已
            # 把整批取成 list，不隔离的话故障半径是整批，AG-11）。
            try:
                self._handle_inbound(msg)
            except Exception as e:  # noqa: BLE001
                _log.warning(f"[consumer] 处理入站消息出错，已跳过该条：{e!r}")
        self._drain_events()
        self._maybe_dispatch_next_text()
        self._check_timers()

    # -- 入站分派 --

    def _handle_inbound(self, msg: Any) -> None:
        if msg.chat_id != self.chat_id:
            _log.info(f"[consumer] 忽略非白名单 chat_id={msg.chat_id}")
            return
        _log.info(f"[consumer] 收到 kind={msg.kind} text={(msg.text or '')!r} "
                  f"file={msg.file_path!r} data={getattr(msg, 'data', None)!r} "
                  f"status={self.view.status}")
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
        """打字"取消"（或点开销消息上的"停下"）不立即执行 - 先弹二次确认。
        取消会炸掉整批（照片 + 处理结果都丢），是危险操作，故意让用户多确
        认一步（真机反馈）。

        票 10：正跑在"写入逐张"的阶段上时，补一句说清哪部分不会作废。用户
        读到的**决策时刻**是这条，不是事后回执 - 只把回执改诚实、这条仍然
        说"都会作废"，等于在他做决定的那一刻仍然骗他，而这一票存在的理由
        正是那个决定要真的可做（G5）。没有要保留的东西时一字不动：
        Collecting 态和比较段的"都会作废"本来就是准确的。"""
        if not self._has_active_batch():
            self._send("现在没有在处理的批次")
            return
        self._touch_activity()
        self._cancel_confirm_pending = True
        text = "确定要取消整批吗？取消后这批照片和已处理的结果都会作废。"
        preserved = self._preserved_on_cancel()
        if preserved is not None:
            text += f"不过{preserved}。"
        self._send_buttons(text, _CANCEL_CONFIRM_BUTTONS)

    def _preserved_on_cancel(self) -> Optional[str]:
        """此刻取消的话，哪一部分不会作废；没有就 None。

        判据跟取消回执同一个（worker.PARTIAL_ON_CANCEL_KINDS 按 kind 分），
        数据来自 view 里那份最近进度 - consumer 手上本来就有，不需要问
        worker。"""
        if self.view.stage_progress is None:
            return None
        done, total, kind = self.view.stage_progress
        return describe_cancel_partial(kind, done, total)

    def _do_stop(self) -> None:
        """开销消息上的"停下"：叫停这一次 AI 尝试，**不作废整批**（真机反馈
        2026-08-02）。

        跟 _do_cancel 几乎处处相反，所以是独立一条路而不是它的一个分支：
        不弹二次确认（这已经不是危险操作，误点的代价只是回到上一个问题），
        不落 cancelling 标记（那是给 bootstrap 看的"别复活这个 run"），不
        _reset_session（同一批要接着走，generation 一动，紧接着回来的
        RunRewound 就会被当过期事件丢掉）。

        先写 on_cancel 再置位事件：worker 只有观察到事件之后才会去读那个
        字段，这个顺序保证它读到的是 rewind 而不是默认的 cancel。
        """
        if not self.view.drive_active or self.active_drive_job is None:
            # 跑完了才点到（分钟级任务里很常见：最后一次比较刚返回）。
            self._send("这一步已经跑完了，没什么可停的")
            return
        self.active_drive_job.on_cancel = "rewind"
        self.active_drive_job.cancel_event.set()
        self._send("正在停下来...")

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
        _log.info(f"[consumer] 按钮点击 action={action!r} run_id={run_id!r}")

        if action in (_BTN_CONFIRM_CANCEL, _BTN_KEEP):
            self._resolve_cancel_confirmation_button(action, run_id)
            return

        if action == _BTN_STOP:
            # 跟二次确认按钮一样排在 drive_active 校验之前：这条按钮只在
            # 跑批期间出现，而那时 run 归 worker，常规校验会把它拦成"过
            # 期"。校验 run_id 防误触旧消息里的按钮。
            if (self.view.run_id or "") != run_id:
                self._send(_MSG_EXPIRED)
                return
            self._do_stop()
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
                if self._curate_narrow_pending is not None:
                    # 去重后追问打字给了数量，这一下"好的"确认的是那次解析
                    # 结果（真机反馈，目标三）。
                    pending, self._curate_narrow_pending = self._curate_narrow_pending, None
                    self._enqueue_drive("rerun_curate", self.run.run_id, {"params": pending})
                    return
                if self._pending_selection_approval:
                    # Style 闸门阶段一：选片确认完，问风格（真机反馈：选片
                    # 确认放在滤镜之前）。
                    self._pending_selection_approval = False
                    self._ask_style_description()
                    return
                # StyleApplyAll 预览确认是"批准即推进"。Style 问描述那步没
                # 有批准按钮，不会走到这里。
                self._enqueue_drive("resolve_gate", self.run.run_id)
                return
        if action == _BTN_SKIP_CURATE and self.run.status == RunStatus.AWAITING_GATE:
            # 去重后追问的"不筛选了"快捷按钮，等价于跳过分类器直接走底层
            # action（跟 _BTN_APPROVE/_BTN_RESTYLE 同一个粒度，不构造假的
            # DedupFollowupReply 走 _on_dedup_followup_reply）。
            self._enqueue_drive("rerun_curate", self.run.run_id, {"params": {"count": None}})
            return
        if action in (_BTN_AI_CURATE, _BTN_AI_DEDUP) and self.run.status == RunStatus.PLANNED:
            # AI 可发现性快捷按钮（目标三决策五）：等价于文字里说了"AI帮我
            # 选"，两个 token 效果完全一样，只是两种 Plan 形状下按钮标签不
            # 同（"AI筛选" vs "AI去重"）。
            curate = next(s for s in self.run.plan.stages if s.name == "Curate")
            self._apply_confirmed_plan_params(curate.params["count"], curate.params["apply_tag"],
                                              True, curate.params.get("provider", "local"))
            return
        if action == _BTN_AI_NARROW and self.run.status == RunStatus.AWAITING_GATE:
            # 追问闸门的"AI筛选"快捷按钮：不问具体数字，AI 直接按 remaining
            # 张的默认策略跑（决策五）。Dedup 已经跑完，不用管它的 ai_enabled。
            curate = next(s for s in self.run.plan.stages if s.name == "Curate")
            self._enqueue_drive("rerun_curate", self.run.run_id, {"params": {
                "count": self._dedup_remaining(self.run),
                "ai_enabled": True,
                "provider": curate.params.get("provider", "local"),
            }})
            return
        if action == _BTN_RESTYLE and self.run.status == RunStatus.AWAITING_GATE:
            # 重选：不带新内容，提示用户打字说想怎么改，下一条文字走既有路径
            # （StyleApplyAll -> rerun_style 换风格；Style 阶段一(选片确认)
            # -> gate_reply 调整）。
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
        self._stage_progress = None       # 同上，运行期那条（T-8）
        self._stage_progress_notified_at = None
        self._cost_announced = False      # 同上，开销告知的接续状态（票 10）
        self._curate_narrow_pending = None
        self._pending_selection_approval = False

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
                if self._pending_selection_approval:
                    # 阶段一：确认/调整选片结果，跟原来 Deliver 闸门同一套
                    # 分类器（真机反馈：选片确认放在滤镜之前）。
                    self._submit_classify("gate_reply", text, {"run_id": self.run.run_id})
                    return
                # 阶段二：问描述那步，交 style_describe 分类器判
                # describe/skip/cancel/query（AG-01/AG-02）。不再把"算了不
                # 弄了"/"有哪些风格"当风格描述。
                self._submit_classify("style_describe", text, {"run_id": self.run.run_id})
                return
            if gate_stage == "StyleApplyAll":
                # 预览确认：交 style_gate 分类器判 approve/redescribe/cancel/query。
                self._submit_classify("style_gate", text, {"run_id": self.run.run_id})
                return
            if gate_stage == "Curate":
                # 去重后追问：交 dedup_followup 分类器判 narrow/skip/query/cancel。
                self._submit_classify("dedup_followup", text,
                                      {"remaining": self._dedup_remaining(self.run)})
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
        self._enqueue_drive_job(job)

    # -- 事件应用 --

    def _drain_events(self) -> None:
        while True:
            try:
                event = self.events.get_nowait()
            except queue.Empty:
                return
            stale = "" if event.generation == self.generation else " [过期丢弃]"
            _log.info(f"[consumer] 事件 {type(event).__name__} gen={event.generation}{stale}")
            # per-item 隔离：事件已出队，处理中途炸了不连累同批其它事件（否则
            # GateReached 发文案失败会把后续事件一起吞掉，用户只能靠 idle 兜底，AG-11）。
            try:
                self._apply_event(event)
            except Exception as e:  # noqa: BLE001
                _log.warning(f"[consumer] 应用事件 {type(event).__name__} 出错，已跳过：{e!r}")

    def _apply_event(self, event: Any) -> None:
        if (isinstance(event, RunFinished) and event.status == RunStatus.CANCELLED.value
                and event.run_id == self.cancelling_run_id):
            # 取消回执唯一例外地跨代接收：用户要的就是"真的停了"这句确认。
            self.cancelling_run_id = None
            self.store.clear_cancelling(event.run_id)  # 正常收尾即清标记（AG-12）
            self._cleanup_run_files(event.run_id)      # 终态即删大文件（AG-14）
            self._send(self._cancel_receipt(event))
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
        elif isinstance(event, StageProgress):
            self._on_stage_progress(event)
        elif isinstance(event, StageCost):
            self._on_stage_cost(event)
        elif isinstance(event, GateReached):
            self._on_gate_reached(event)
        elif isinstance(event, RunFinished):
            self._on_run_finished(event)
        elif isinstance(event, RunRewound):
            self._on_run_rewound(event)
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
        if event.kind == "dedup_followup":
            self._on_dedup_followup_reply(result)
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
        self._apply_confirmed_plan_params(reply.count, reply.apply_tag,
                                          reply.ai_enabled, reply.provider)

    def _on_gate_reply(self, reply: Any) -> None:
        # 现在只服务 Style 闸门的阶段一（选片确认，真机反馈挪到滤镜之前）—
        # Deliver 不再挂闸门，approve 不再是"resolve_gate"，是"选片确认完，
        # 问风格"。
        if self.run is None or self.run.status != RunStatus.AWAITING_GATE:
            return
        if reply.action == "approve":
            self._pending_selection_approval = False
            self._ask_style_description()
            return
        if reply.action == "reject":
            self._prompt_cancel_confirmation()  # LLM 判成取消也要二次确认
            return
        if reply.action == "query":
            self._send(self.view.describe())
            return
        self._enqueue_drive("adjustment", self.run.run_id, {"delta": reply.delta})

    def _on_dedup_followup_reply(self, reply: Any) -> None:
        if self.run is None or self.run.status != RunStatus.AWAITING_GATE:
            return
        if reply.action == "query":
            self._send(self.view.describe())
            return
        if reply.action == "cancel":
            self._curate_narrow_pending = None
            self._prompt_cancel_confirmation()  # LLM 判成取消也要二次确认
            return
        if reply.action == "skip":
            # 明确的一次性决定，不用二次确认（不筛选了/AI筛选按钮同理）。不
            # 手动发"正在筛选..."：rerun_curate 会先发 StageStarted(Curate)，
            # _on_stage_started 已经会自动发这句，手动再发一遍是重复消息。
            self._curate_narrow_pending = None
            self._enqueue_drive("rerun_curate", self.run.run_id, {"params": {"count": None}})
            return
        if reply.action == "approve":
            if self._curate_narrow_pending is not None:
                pending, self._curate_narrow_pending = self._curate_narrow_pending, None
                self._enqueue_drive("rerun_curate", self.run.run_id, {"params": pending})
            else:
                self._send(self.view.describe())  # 没有待确认的东西，当查状态处理
            return
        # narrow：识别出数量(和可能的标签)后先回显确认，不直接执行——用户
        # 打字给张数和目的这一步必须过一遍确认，不能一句话打完就直接跑
        # （真机反馈）。query 不清 _curate_narrow_pending：用户可能只是顺
        # 口问一句"还剩几张"，之后仍可能回来确认之前那次 narrow。
        curate = next(s for s in self.run.plan.stages if s.name == "Curate")
        apply_tag = reply.apply_tag or curate.params.get("apply_tag", "精选")
        self._curate_narrow_pending = {"count": reply.count, "apply_tag": apply_tag}
        # 票 08：这次没提题材偏好就整个不放这个 key。rerun_stage 是
        # params.update() 语义，塞个空串进去会把组装意图时抽出来的那份冲掉
        # （"去重，挑有人的"里简述来自最初那句话，追问只补了张数）。
        if reply.selection_brief:
            self._curate_narrow_pending["selection_brief"] = reply.selection_brief
        self._send_buttons(
            f"留 {reply.count} 张，选中的加个标签\"{apply_tag}\"，可以吗？\n满意就点\"好的\"，想改直接打字说",
            _CONFIRM_BUTTONS,
        )

    def _on_compose_done(self, event: ComposeDone) -> None:
        inflight, self.inflight = self.inflight, None
        if inflight is None or self.run is None or self.run.status != RunStatus.COLLECTING:
            return
        plan = event.plan
        # 参数注入对齐旧 _propose_plan：Ingest 收图目录、Deliver 目的地。
        # Deliver 不挂闸门（真机反馈：滤镜确认完直接交付，不再二次预览全部
        # 选片），选片确认已经挪到 Style 闸门的阶段一。
        ingest_spec = next(s for s in plan.stages if s.name == "Ingest")
        deliver_spec = next(s for s in plan.stages if s.name == "Deliver")
        ingest_spec.params["folder"] = str(incoming_dir_for(self.incoming_root, self.run.run_id))
        deliver_spec.params["out_folder"] = str(self.deliver_out_folder)
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

    def _finalize_stage_progress(self) -> None:
        """把这一条进度消息改成终态（真机反馈）。

        只有 3/3 还不够 —— 那句话仍然是"正在…"，读起来像还在跑，然后下一
        件事无预警发生。调用点都是"上一段确实跑完了"的时刻：下一个 stage
        开跑、停在闸门、整批完成，以及票 09 起的第四个 - 同一个 stage 内
        换 phase（开 AI 的 curate 先比较、后逐张评估）。**取消和失败不
        调**，把半截进度改成"套完了"是在撒谎。

        原地编辑同一条消息而不是新发一条：进度本来就只占一条（AG-16.3），
        每个 stage 再追一条"完成"会把对话刷成流水账。"""
        if self._stage_progress is None or self.view.stage_progress is None:
            return
        _, total, kind = self.view.stage_progress
        self._send_progress(describe_progress_done(kind, total), "_stage_progress")
        self._stage_progress = None
        self._stage_progress_notified_at = None

    def _close_progress_slot(self) -> None:
        """收尾当前这条进度并腾空槽位，下一条进度于是会是一条新消息。

        换 stage 和换 phase 共用这一步。**腾空必须无条件做，不能只依赖
        _finalize_stage_progress**：那一步在槽是空的时候会提前返回，而槽正
        是上一次发送失败时被 _send_progress 置空的（拿不到 message_id）。
        只走收尾的话计时器会残留下来，下一段的第一帧落进上一段的节流窗口
        里被吃掉。
        """
        self._finalize_stage_progress()
        # 换槽的理由（AG-16.3 的进度是原地编辑的）：不换的话下一段的进度会
        # 去改写上一段那条，用户往回翻看到的历史是错的。时间戳一起清零，下
        # 一段的第一条立刻发得出去。
        self._stage_progress = None
        self._stage_progress_notified_at = None

    def _on_stage_started(self, event: StageStarted) -> None:
        # 下一个 stage 开跑 = 上一个跑完了，先把它那条进度收尾。
        self._close_progress_slot()
        self.view.current_stage = event.stage
        self.view.stage_progress = None
        message = STAGE_PROGRESS_MESSAGES.get(event.stage)
        if message:
            self._send(message)

    def _cancel_receipt(self, event: RunFinished) -> str:
        """取消回执。写入逐张的那些阶段被打断时一定留下部分成果，只说"已
        取消"等于假装什么都没发生 - 用户回头看到一半照片带滤镜会更困惑，
        而 curate 的评估段更糟：那几次调用的结果其实留在库里、下次还能
        省钱，不说的话用户以为白花了（票 10 决策二/四）。

        零写入的那些阶段走裸回执，worker 那边就不会给 cancelled_partial
        （决策五）。措辞按 kind 分，落在 view 里跟其它 prose 一起。"""
        partial = event.cancelled_partial
        if partial is None:
            return "已取消"
        _, done, total, kind = partial
        detail = describe_cancel_partial(kind, done, total)
        return "已取消" if detail is None else f"已取消（{detail}）"

    def _on_stage_progress(self, event: StageProgress) -> None:
        """T-8：运行期进度。view 每条都刷新（用户主动问"到哪了"要拿到最
        新的），往 Telegram 发则按 progress_interval_seconds 节流 —— 一个
        20 张的簇是 19 次比较，每次发一条会被限流。决策二：core/cli 侧不
        节流，谁播报谁节流。

        换 phase 时另起一条消息（票 09）：票 05 之后开 AI 的 curate 会在一
        个 Curate stage 里先比较、后逐张评估，两段数的东西不同（次 / 张）。
        挤在同一条里原地编辑的话，用户会看到"已完成 160/160 次"直接变成
        "已完成 1/6 张" - 分子分母同时跳，读起来像进度条倒退；而且比较那条
        的终态句永远发不出去，因为收尾原本只在 stage 边界触发，评估的第一
        帧会把它原地覆盖掉。换 stage 与换 phase 共用 _close_progress_slot。
        """
        if (self.view.stage_progress is not None
                and self.view.stage_progress[2] != event.kind):
            # 必须在覆盖 view.stage_progress 之前收尾：终态句的 kind 和
            # total 都是从它里面读的。
            self._close_progress_slot()
        self.view.stage_progress = (event.done, event.total, event.kind)
        now = self.now_fn()
        last = self._stage_progress_notified_at
        # 最后一跳不节流：它往往紧跟着前一跳（2/3 -> 3/3 中间可能只隔几十
        # 毫秒），落在窗口里被吃掉的话，可见的最后一条永远停在 2/3（真机
        # 反馈）。
        terminal = event.total > 0 and event.done >= event.total
        if not terminal and last is not None and now - last < self.progress_interval_seconds:
            return
        self._send_progress(self.view.describe(), "_stage_progress")
        self._stage_progress_notified_at = now

    def _on_stage_cost(self, event: StageCost) -> None:
        """票 10 决策一：AI 开跑前的开销告知。

        独立一条带按钮的新消息，**不进** _stage_progress 那个原地编辑的
        槽 - 占了的话接下来第一条进度就会把这条账单改写掉，用户翻回去看
        不到自己被告知过什么。也不节流：它一趟最多两条（Dedup 一条、
        Curate 一条），而且晚发就失去了全部意义。

        零开销不发（describe_ai_cost 返回 None）：core 在没有 AI 调用要发
        时本来就不报，真报了也不该翻译成一句"接下来要跑 0 次"。
        """
        provider = (self.view.plan_summary or {}).get("provider", "local")
        text = describe_ai_cost(event.comparisons, event.evaluations, provider,
                                 first=not self._cost_announced)
        if text is None:
            return
        self._cost_announced = True
        self._send_buttons(text, _COST_BUTTONS)

    def _on_gate_reached(self, event: GateReached) -> None:
        self._finalize_stage_progress()  # 停在闸门 = 闸门前那个 stage 跑完了
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
            if payload.get("match_failed"):
                # 上一句描述没匹配上任何 preset：原地重问，不报废整批（AG-01）。
                self._send("没能选出对应的风格，换个说法再说说？比如\"复古暖色调\"、"
                           "\"黑白胶片\"；不想套滤镜就说\"原图就行\"")
                return
            curate = next(s for s in run.plan.stages if s.name == "Curate")
            if curate.params.get("count") is not None:
                # 真正筛选过（不是"不筛选了"passthrough）：先确认/调整选片
                # 结果，确认了才问风格（真机反馈：选片确认放在滤镜之前，
                # 不然到滤镜那一步才发现选片不对就晚了）。
                self._pending_selection_approval = True
                self._render_selection_confirm_gate(payload)
                return
            self._pending_selection_approval = False
            self._ask_style_description()
            return
        if event.stage == "Curate":
            self._render_dedup_followup_gate(payload)
        elif event.stage == "StyleApplyAll":
            self._render_style_apply_all_gate(payload)

    def _render_dedup_followup_gate(self, payload: dict) -> None:
        ai_enabled = payload.get("ai_enabled", False)
        hint = "" if ai_enabled else "（可以用 AI 帮你选，更准但更慢）"
        buttons = _DEDUP_FOLLOWUP_BUTTONS if ai_enabled else _DEDUP_FOLLOWUP_BUTTONS + [
            ("AI筛选 🤖", _BTN_AI_NARROW)]
        self._send_buttons(
            f"去重后还剩 {payload.get('remaining', 0)} 张，要不要再筛选一下？"
            f"留全部点\"不筛选了\"；要筛选就告诉我留几张、想发去哪，"
            f"比如\"{_intent_hint_example('留')}\"。{hint}",
            buttons,
        )

    def _render_style_apply_all_gate(self, payload: dict) -> None:
        chosen = payload.get("chosen_recipe")
        if not chosen:
            # 无风格（用户选了原图直出）：没有预览可确认，直接推进到交付
            # （Deliver 不再挂闸门，批准即直接发文件）。匹配失败现在已在
            # Style 闸门就拦下重问，这里 chosen=None 只可能是 skip（AG-16.1）。
            self._send("这批不套滤镜，直接交付了")
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

    def _ask_style_description(self) -> None:
        self._send("想要什么风格？用一句话描述就行，比如\"复古暖色调\"")

    def _render_selection_confirm_gate(self, payload: dict) -> None:
        # 选片确认（真机反馈：挪到滤镜之前），这一步风格还没套，不提
        # "已套用风格"。
        if payload.get("export_error"):
            self._send(f"预览导出失败：{payload['export_error']}")
            return
        summary = f"选好了 {payload.get('selected_count', 0)} 张"
        if payload.get("preview_failed_count"):
            summary += f"(其中 {payload['preview_failed_count']} 张预览发送失败，风格/交付时仍会正常导出)"
        # T-8：整簇 AI 比较失败会退化成"按拍摄时间选最新"。不说出来的话，
        # 用户在这个闸门上点"满意"，以为自己认可的是 AI 的判断。缺 key 走
        # 0（老 run 续跑时盘上的 payload 没有它）。
        if payload.get("ai_fallback_count"):
            summary += f"(其中 {payload['ai_fallback_count']} 组 AI 比较失败，是按拍摄时间挑的)"
        summary += "，满意就点\"满意\"，想调整点\"重选\"或直接打字说"
        self._send_buttons(summary, _DELIVER_BUTTONS)

    def _on_run_finished(self, event: RunFinished) -> None:
        self.active_drive_job = None
        if event.status == RunStatus.FAILED.value:
            self._send(f"处理失败：{event.detail or '未知错误'}")
        elif event.status == RunStatus.DONE.value:
            self._finalize_stage_progress()
            # Deliver stage 自己已说"选好了 N 张"，这里补一句收尾，明确告诉
            # 用户这批结束了、可以开新的（真机反馈）。
            self._send("这批就处理完啦～想开新的一批，随时把照片发给我就行 📷")
        self._cleanup_run_files(event.run_id)  # 终态即删大文件（AG-14）
        self.run = None
        self.view = SessionView(incoming_root=self.incoming_root)

    def _on_run_rewound(self, event: RunRewound) -> None:
        """被"停下"叫停的那一步已经退回未运行，run 还活着（真机反馈
        2026-08-02）。这里要做的是回到**当初问过要不要用 AI 的那一步**，
        重新问一次。

        为什么不是回到最初的方案确认：已经跑完的上游（比如去重）是有效
        成果，退掉等于让用户白等一遍。回哪一步因此由被停的是谁决定。
        """
        self.active_drive_job = None
        run = self.store.load(event.run_id)  # 所有权交回：从盘上取
        self.run = run
        self._touch_activity()
        # 进度那条消息**不收尾**：停下不是跑完，把半截进度改写成"两两比较
        # 跑完了，共 18 次"是撒谎（同取消路径的规矩）。只腾空槽位，下一段
        # 进度会是一条新消息。
        self._stage_progress = None
        self._stage_progress_notified_at = None

        detail = None
        if event.partial is not None:
            _, done, total, kind = event.partial
            detail = describe_cancel_partial(kind, done, total)
        self._send("好，停下了" + (f"（{detail}）" if detail else ""))

        # AI 开关关掉再问：刚被停下的就是它，默认再开一次等于没听见 —— 而
        # 且方案确认那条消息在 ai_enabled 为真时只给一个"好的"，用户唯一
        # 能点的按钮会是"再跑一次 AI"。全局开关，两个 stage 一起关
        # （SPEC §3.3）。
        for name in ("Dedup", "Curate"):
            spec = next((s for s in run.plan.stages if s.name == name), None)
            if spec is not None:
                spec.params["ai_enabled"] = False

        curate = next((s for s in run.plan.stages if s.name == "Curate"), None)
        followup = (event.stage == "Curate" and curate is not None and curate.gate != "off")
        if followup:
            # count 待定那条 Plan：要不要用 AI 选片是在去重后的追问闸门上
            # 问的，不在最初的方案确认上。
            self.driver.rearm_gate(run, "Curate")
            self.view = view_from_run(run, self.incoming_root)
            self._render_dedup_followup_gate({"remaining": self._dedup_remaining(run),
                                               "ai_enabled": False})
            return
        run.status = RunStatus.PLANNED
        self.store.save(run)
        self.view = view_from_run(run, self.incoming_root)
        self._send_plan_confirmation(run)

    def _on_job_crashed(self, event: JobCrashed) -> None:
        # 静默崩溃是最糟的失败模式（用户和终端都看不到），必须回一句话过去
        # （真机反馈）。只清崩掉那条 lane 的状态：两条 lane 并发，动了没崩的
        # 那条会连带副作用（drive 误触 resume 排双 job / classify 那条文本没
        # 有任何回复）。lane 由 worker 从 job 类型判定，不再靠 view 猜。
        _log.warning(f"[consumer] worker job 崩了（已兜底，lane={event.lane}）：{event.error}")
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
                       f"比如\"{_intent_hint_example()}\"")

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
        _log.info(f"[consumer] 投递(classify lane) {type(job).__name__} gen={job.generation}")
        self.classify_jobs.put(job)

    def _enqueue_drive_job(self, job: Any) -> None:
        _log.info(f"[consumer] 投递(drive lane) {type(job).__name__} gen={job.generation}")
        self.drive_jobs.put(job)

    def _send(self, text: str) -> Optional[str]:
        _log.info(f"[consumer] 回复: {text!r}")
        # Telegram 抖动/超时是真机常见故障：退避重试一次后放弃，不外抛（否则
        # 会把整轮 step 拖挂、连累同批消息，AG-11）。不追求消息不丢的强保证。
        # 返回 message_id（progress 原地编辑用，AG-16.3），失败/不支持则 None。
        try:
            return self.transport.send_text(self.chat_id, text)
        except Exception as e:  # noqa: BLE001
            _log.warning(f"[consumer] send_text 失败，退避重试一次：{e!r}")
            time.sleep(self.send_retry_backoff_seconds)
            try:
                return self.transport.send_text(self.chat_id, text)
            except Exception as e2:  # noqa: BLE001
                _log.warning(f"[consumer] send_text 重试仍失败，放弃这条：{e2!r}")
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
                _log.warning(f"[consumer] 进度消息编辑失败，改发新的：{e!r}")
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
                "\n\n平时把照片发给我、再说一句想怎么处理就行，"
                f"比如\"{_intent_hint_example()}\"。")

    def _send_help(self) -> None:
        self._send(
            "我是帮你选照片的小助手 📷\n"
            "把要处理的照片发给我，再用一句话说想怎么弄就行，比如：\n"
            f"· {_intent_hint_example()}\n"
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
        _log.info(f"[consumer] 回复(带按钮): {text!r} buttons={[l for l, _ in actions]}")
        send_buttons(self.chat_id, text, options)

    def _current_plan_params(self, run: RunState) -> dict:
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        return {
            "count": curate.params["count"],
            "apply_tag": curate.params["apply_tag"],
            "ai_enabled": curate.params.get("ai_enabled", False),
            "provider": curate.params.get("provider", "local"),
        }

    def _plan_summary(self, run: RunState) -> dict:
        """view.describe() 用的展示口径，比 _current_plan_params 多一个选片
        简述。刻意分成两个方法：_current_plan_params 还喂着 refine_plan 那
        次分类的提示词，而 PlanConfirmationReply 没有 selection_brief 字段
        - 把它塞进提示词，模型改不了，只会当噪音读，还可能误以为自己该改。

        另一份 plan_summary 由 view.view_from_run 在重启恢复时装配，两边的
        key 集合本来就不完全一样（那边不抄 provider，describe() 也不读它）。
        describe() 真正读的 count/apply_tag/ai_enabled/selection_brief 四个
        必须两边都有。
        """
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        return {
            "count": curate.params["count"],
            "apply_tag": curate.params["apply_tag"],
            "ai_enabled": curate.params.get("ai_enabled", False),
            "provider": curate.params.get("provider", "local"),
            "selection_brief": curate.params.get("selection_brief") or "",
        }

    def _apply_confirmed_plan_params(self, count, apply_tag, ai_enabled, provider) -> None:
        # ai_enabled/provider 是全局开关，Dedup/Curate 两份拷贝一起改，不是
        # 共享引用；Dedup 这次可能压根不在 Plan 里（W2026-07-21 目标三"没提
        # 去重"分支），必须带默认值，否则 StopIteration 会被 _drain_events
        # 的兜底吞掉、这条回复静默无效。同时被 _on_refine_reply 的 confirmed
        # 分支和决策五的 ai_curate/ai_dedup 快捷按钮调用，逻辑不重复两份。
        dedup = next((s for s in self.run.plan.stages if s.name == "Dedup"), None)
        curate = next(s for s in self.run.plan.stages if s.name == "Curate")
        curate.params["count"] = count
        curate.params["apply_tag"] = apply_tag
        curate.params["ai_enabled"] = ai_enabled
        curate.params["provider"] = provider
        if count is not None:
            # 提前给了数量 = 提前回答了 Dedup 后才会问的追问，解除 Curate 的
            # 待定状态（目标三决策七）。这里直接改 Plan 对象、Driver 还没开
            # 始跑，不会碰到"运行中重新触发闸门"那个坑（那是 apply_adjustment
            # 才会踩的，rerun_curate 走的是另一条不看 .gate 的路径）。
            curate.gate = "off"
        if dedup is not None:
            dedup.params["ai_enabled"] = ai_enabled
            dedup.params["provider"] = provider
        self.store.save(self.run)
        self.view.plan_summary = self._plan_summary(self.run)
        self._send_plan_confirmation(self.run)

    def _send_plan_confirmation(self, run: RunState) -> None:
        curate = next(s for s in run.plan.stages if s.name == "Curate")
        ai_enabled = curate.params.get("ai_enabled", False)
        # AI 可发现性（目标三决策五）：ai_enabled 为 False 时才提醒 + 给快
        # 捷按钮，追加在已有句子之后（新起一行），不改动前面的措辞。
        hint = "" if ai_enabled else "\n这一步也可以用 AI 帮你挑更准，更慢一点。"
        if curate.params["count"] is None:
            # deferred 形状（W2026-07-21 目标三案例二）：Curate 数量待定，
            # 不能把 None 插进"留 None 张"这种文案里。这一步只确认"去重"这
            # 件事，不预告去重完还要问什么（真机反馈：太啰嗦），追问闸门
            # 自己会在 Dedup 跑完之后再问。
            text = (f"理解你想：先帮你去重，"
                    f"留下的加个标签\"{curate.params['apply_tag']}\"，可以吗？{hint}"
                    "\n满意就点\"好的\"，想改直接打字说")
            ai_button = ("AI去重 🤖", _BTN_AI_DEDUP)
        else:
            # 票 08 的选片简述在这一步之前对用户完全不可见，他没有任何地
            # 方能核对自己那句题材要求被认成了什么（真机反馈 2026-08-02：
            # 抽取和传送其实都是好的，用户仍然判定"brief 没被识别"，因为
            # 屏幕上从头到尾没出现过它）。
            #
            # 原样拼进句子，不改写 - 改写要么再花一次 LLM 调用，要么用规
            # 则截断，两者都可能把用户正要核对的那几个字弄没。简述本身由
            # 模型写成"发朋友圈用，有人有景，人物表情活泼"这种短句列表，
            # 直接当"照片"的定语读得通。
            # 关 AI 时不说"按拍摄时间" - 那是当前的实现选择，不是对用户的
            # 承诺，将来会变（真机反馈 2026-08-02）。用户需要知道的是"这次
            # 不用 AI"，而下面那句"这一步也可以用 AI 帮你挑更准"已经说了。
            picker = "使用AI帮你选择" if ai_enabled else "帮你选择"
            brief = curate.params.get("selection_brief", "")
            subject = f"{brief}的照片" if brief else "照片"
            text = (f"理解你想：{picker} {curate.params['count']} 张{subject}，"
                    f"选中的加个标签\"{curate.params['apply_tag']}\"，可以吗？{hint}"
                    "\n满意就点\"好的\"，想改直接打字说")
            ai_button = ("AI筛选 🤖", _BTN_AI_CURATE)
        buttons = _CONFIRM_BUTTONS if ai_enabled else _CONFIRM_BUTTONS + [ai_button]
        self._send_buttons(text, buttons)

    def _dedup_remaining(self, run: RunState) -> int:
        ingest_output = run.outputs.get("Ingest")
        dedup_output = run.outputs.get("Dedup")
        total = ingest_output.data.get("image_count", 0) if ingest_output else 0
        tagged = dedup_output.data.get("tagged", 0) if dedup_output else 0
        return total - tagged


def _looks_like_infra_error(message: str) -> bool:
    return any(hint in message for hint in _INFRA_ERROR_HINTS)
