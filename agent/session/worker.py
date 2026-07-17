"""SessionWorker：2.0 运行时的工作线程（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第五、六节）。单队列 FIFO 串行执行三类 job，事件上报给
consumer，不做任何会话决策、不渲染对话文本。

所有权：DriveJob 活跃期间它独占对应 run 的变更与落盘（Driver 内部每个
stage 边界 save 就是断点续跑检查点），交接靠"落盘 + 事件"，不共享内存
对象——consumer 收到事件后要读 run 就自己重新 load。

取消：cancel_event 在 stage 边界（推进循环每轮开头）必查；可杀 stage
（Evaluate/Dedup，仅有的分钟级子进程）advance 前把事件挂到 worker 专属
的 client 实例上，PztCancelledError 从 stage.run/driver.advance 一路穿
透到这里（它不是 PztCommandError，stages 吞不掉），统一走 CANCELLED 收
尾。其余 stage 秒级，跑完为止。

step() 单步驱动是测试口径，线程只是 run() 这层薄壳。
"""
from __future__ import annotations

import queue
import threading
import traceback
from pathlib import Path
from typing import Any, Callable, Tuple

from compose.adjustment_parser import AdjustmentError
from compose.llm_client import LlmRequestError
from compose.validate import ValidationError, validate_plan
from orchestrator.types import RunState, RunStatus, StageStatus
from pzt_client import PztCancelledError, PztCommandError
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

KILLABLE_STAGES = ("Evaluate", "Dedup")


class SessionWorker:
    def __init__(self, jobs: "queue.Queue", events: "queue.Queue", driver: Any, store: Any,
                 client: Any, transport: Any, chat_id: str, preview_root: Path,
                 compose_plan_fn: Callable, classify_collecting_message_fn: Callable,
                 classify_gate_reply_fn: Callable, refine_plan_confirmation_fn: Callable,
                 killable_stages: Tuple[str, ...] = KILLABLE_STAGES) -> None:
        self.jobs = jobs
        self.events = events
        self.driver = driver
        self.store = store
        self.client = client  # worker 专属实例，consumer 的只读查询用另一个
        self.transport = transport  # 只发 stage 产物媒体，对话文本归 consumer
        self.chat_id = chat_id
        self.preview_root = Path(preview_root)
        self.compose_plan_fn = compose_plan_fn
        self.classify_collecting_message_fn = classify_collecting_message_fn
        self.classify_gate_reply_fn = classify_gate_reply_fn
        self.refine_plan_confirmation_fn = refine_plan_confirmation_fn
        self.killable_stages = set(killable_stages)

    # -- 线程壳 --

    def run(self, stop_event: threading.Event, poll_seconds: float = 0.5) -> None:
        while not stop_event.is_set():
            self.step(timeout=poll_seconds)

    def step(self, timeout: float = 0.0) -> bool:
        """执行一个 job；队列空返回 False。未预期异常兜底成 JobCrashed
        事件——worker 线程本身永不带病死掉（对齐旧主循环"单条消息炸了
        不拖死常驻进程"的语义）。"""
        try:
            if timeout:
                job = self.jobs.get(timeout=timeout)
            else:
                job = self.jobs.get_nowait()
        except queue.Empty:
            return False
        print(f"[worker] 开始 {self._describe_job(job)}", flush=True)
        try:
            self._execute(job)
        except Exception as e:  # noqa: BLE001 兜底见 docstring
            # 静默崩溃是最糟的失败模式（用户和终端都看不到）：打全栈 +
            # 发 JobCrashed 让 consumer 回一句话过去。
            print(f"[worker] 崩了 {self._describe_job(job)}：{e!r}", flush=True)
            traceback.print_exc()
            self.events.put(JobCrashed(generation=job.generation, error=repr(e)))
        else:
            print(f"[worker] 完成 {self._describe_job(job)}", flush=True)
        return True

    @staticmethod
    def _describe_job(job: Any) -> str:
        kind = type(job).__name__
        if isinstance(job, DriveJob):
            return f"{kind}(action={job.action}, gen={job.generation})"
        if isinstance(job, ClassifyJob):
            return f"{kind}(kind={job.kind}, gen={job.generation}, text={job.text!r})"
        if isinstance(job, ComposeJob):
            return f"{kind}(gen={job.generation}, text={job.intent_text!r})"
        return f"{kind}(gen={getattr(job, 'generation', '?')})"

    # -- job 分派 --

    def _execute(self, job: Any) -> None:
        if isinstance(job, ClassifyJob):
            self._execute_classify(job)
        elif isinstance(job, ComposeJob):
            self._execute_compose(job)
        elif isinstance(job, DriveJob):
            self._execute_drive(job)
        else:
            raise ValueError(f"unknown job type: {type(job).__name__}")

    def _execute_classify(self, job: ClassifyJob) -> None:
        try:
            if job.kind == "collecting":
                result = self.classify_collecting_message_fn(job.text, job.context["photo_count"])
            elif job.kind == "gate_reply":
                # classify_gate_reply 的 swap_out 解析需要 run 的 Curate
                # 输出和 plan——load 一份只读副本传进去（读不受所有权协
                # 议限制，RunStore.save 是原子替换，读到的永远是完整文件）。
                run = self.store.load(job.context["run_id"])
                result = self.classify_gate_reply_fn(job.text, run)
            elif job.kind == "refine_plan":
                result = self.refine_plan_confirmation_fn(
                    job.context["intent_raw"], job.context["current_params"], job.text)
            else:
                raise ValueError(f"unknown classify kind: {job.kind!r}")
        except AdjustmentError:
            self.events.put(ClassifyFailed(job.generation, job.kind, retryable=False))
        except LlmRequestError:
            self.events.put(ClassifyFailed(job.generation, job.kind, retryable=True))
        else:
            self.events.put(ClassifyDone(job.generation, job.kind, result))

    def _execute_compose(self, job: ComposeJob) -> None:
        try:
            plan = validate_plan(self.compose_plan_fn(job.intent_text, None, None))
        except (ValidationError, LlmRequestError) as e:
            self.events.put(ComposeFailed(job.generation, e.message))
        else:
            self.events.put(ComposeDone(job.generation, plan))

    # -- drive --

    def _execute_drive(self, job: DriveJob) -> None:
        run = self.store.load(job.run_id)
        if job.action == "start":
            if run.status != RunStatus.RUNNING:  # consumer 已置好，这里兜底
                run.status = RunStatus.RUNNING
                self.store.save(run)
        elif job.action == "resolve_gate":
            self.driver.resolve_gate(run, "proceed")
        elif job.action == "adjustment":
            self.driver.apply_adjustment(run, job.args["delta"])
        elif job.action == "rerun_style":
            self.driver.rerun_stage(run, "Style", {"style_description": job.args["style_description"]})
        elif job.action != "resume":
            raise ValueError(f"unknown drive action: {job.action!r}")
        self._drive_to_stop(run, job)
        self._report_stop(run, job)

    def _drive_to_stop(self, run: RunState, job: DriveJob) -> None:
        while run.status == RunStatus.RUNNING:
            if job.cancel_event.is_set():
                self.driver.cancel(run)
                return
            next_stage = self.driver.peek_next_stage(run)
            if next_stage is not None:
                print(f"[worker] run={run.run_id} 运行 stage={next_stage}", flush=True)
                # 每个 stage 都发事件（view 要 current_stage）；哪些渲染
                # 成"正在..."文案是 consumer 按消息表判断的事。
                self.events.put(StageStarted(job.generation, run.run_id, next_stage))
            armed = next_stage in self.killable_stages
            if armed:
                self.client.cancel_event = job.cancel_event
            try:
                self.driver.advance(run)
            except PztCancelledError:
                self.driver.cancel(run)
                return
            finally:
                if armed:
                    self.client.cancel_event = None

    def _report_stop(self, run: RunState, job: DriveJob) -> None:
        if run.status == RunStatus.AWAITING_GATE:
            stage = run.gate_state.stage_name
            payload = self._prepare_gate_payload(run, stage)
            self.events.put(GateReached(job.generation, run.run_id, stage, payload))
            return
        if run.status == RunStatus.AWAITING_REVIEW:
            # 全部 stage 跑完的自动收尾，对齐旧 router 各路径的自动 approve。
            self.driver.approve(run)
        detail = self._first_failure_detail(run) if run.status == RunStatus.FAILED else None
        self.events.put(RunFinished(job.generation, run.run_id, run.status.value, detail))

    def _first_failure_detail(self, run: RunState) -> str:
        for name, status in run.stage_states.items():
            if status == StageStatus.FAILED:
                output = run.outputs.get(name)
                error = output.error if output else None
                return f"{name}：{error or '未知错误'}"
        return "未知错误"

    # -- 闸门媒体准备（对话文本由 consumer 按 payload 渲染） --

    def _prepare_gate_payload(self, run: RunState, stage: str) -> dict:
        if stage == "Style":
            return {}
        if stage == "StyleApplyAll":
            style_output = run.outputs.get("Style")
            chosen = style_output.data.get("chosen_recipe") if style_output else None
            preview_photo = style_output.data.get("preview_photo") if style_output else None
            if not chosen or not preview_photo:
                return {"chosen_recipe": None}
            export_error = self._export_previews(run, [preview_photo])
            if export_error is not None:
                return {"chosen_recipe": chosen, "preview_sent": False, "export_error": export_error}
            failed = self._send_preview_media(run, [preview_photo])
            return {"chosen_recipe": chosen, "preview_sent": failed == 0, "export_error": None}
        # 默认闸门（当前只有 Deliver）：Curate 选片预览
        curate_output = run.outputs.get("Curate")
        selected = curate_output.data.get("selected", []) if curate_output else []
        style_output = run.outputs.get("StyleApplyAll")
        applied = style_output.data.get("applied", {}) if style_output else {}
        payload = {
            "selected_count": len(selected),
            "applied_recipe": next(iter(applied.values())) if applied else None,
            "preview_failed_count": 0,
            "export_error": None,
        }
        if not selected:
            return payload
        export_error = self._export_previews(run, selected)
        if export_error is not None:
            payload["export_error"] = export_error
            return payload
        payload["preview_failed_count"] = self._send_preview_media(run, selected)
        return payload

    def _export_previews(self, run: RunState, selected: list) -> "str | None":
        # selected 是项目 root_path 相对路径，必须先过一次 export-images
        # 才有真字节可发（跟旧 _send_preview / stages/deliver.py 同理）。
        preview_dir = self.preview_root / run.run_id
        try:
            self.client.call("export-images", run.project_id, *selected, str(preview_dir))
        except PztCommandError as e:
            return e.message
        return None

    def _send_preview_media(self, run: RunState, selected: list) -> int:
        # 逐张 图->文件 降级重试，最后统计失败数（旧 _send_preview 的
        # "一张超标图不能带崩整个预览循环"语义原样保留）。
        preview_dir = self.preview_root / run.run_id
        failed = 0
        for path in selected:
            preview_path = str(preview_dir / Path(path).name)
            try:
                self.transport.send_photo(self.chat_id, preview_path)
            except Exception:  # noqa: BLE001 Telegram 体积上限等，降级不挑异常
                try:
                    self.transport.send_file(self.chat_id, preview_path)
                except Exception:  # noqa: BLE001
                    failed += 1
        return failed
