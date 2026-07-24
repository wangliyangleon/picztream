"""SessionWorker：2.0 运行时的工作线程（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第五、六节）。单队列 FIFO 串行执行三类 job，事件上报给
consumer，不做任何会话决策、不渲染对话文本。

所有权：DriveJob 活跃期间它独占对应 run 的变更与落盘（Driver 内部每个
stage 边界 save 就是断点续跑检查点），交接靠"落盘 + 事件"，不共享内存
对象——consumer 收到事件后要读 run 就自己重新 load。

取消：cancel_event 在 stage 边界（推进循环每轮开头）必查；可杀 stage
（Dedup/Curate，AI 开时可能跑到分钟级）advance 前把事件挂到 worker 专属
的 client 实例上，PztCancelledError 从 stage.run/driver.advance 一路穿
透到这里（它不是 PztCommandError，stages 吞不掉），统一走 CANCELLED 收
尾。其余 stage 秒级，跑完为止。

step() 单步驱动是测试口径，线程只是 run() 这层薄壳。
"""
from __future__ import annotations

import logging
import queue
import shutil
import threading
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

_log = logging.getLogger("pzt.agent.worker")

KILLABLE_STAGES = ("Dedup", "Curate")


class SessionWorker:
    """双 lane：分类/编排 lane（`classify_jobs`，纯 LLM，轻）和 drive lane
    （`drive_jobs`，pzt 子进程，重）各一条线程并发跑（见 Eng Design 真机
    反馈"彻底去关键词"一节）。拆开是为了让"处理中"也能跑取消/进度的 LLM
    分类——单 lane 时 classify 会排在几分钟的 drive 后面饿死。两 lane 无
    共享可变态：classify/compose 是纯 LLM（不碰 self.client、不改 run），
    drive 独占 run 的变更与落盘；唯一交集是线程安全的 event 队列。"""

    def __init__(self, classify_jobs: "queue.Queue", drive_jobs: "queue.Queue",
                 events: "queue.Queue", driver: Any, store: Any,
                 client: Any, transport: Any, chat_id: str, preview_root: Path,
                 compose_plan_fn: Callable, classify_fns: "dict[str, Callable]",
                 killable_stages: Tuple[str, ...] = KILLABLE_STAGES) -> None:
        self.classify_jobs = classify_jobs
        self.drive_jobs = drive_jobs
        self.events = events
        self.driver = driver
        self.store = store
        self.client = client  # worker 专属实例，consumer 的只读查询用另一个
        self.transport = transport  # 只发 stage 产物媒体，对话文本归 consumer
        self.chat_id = chat_id
        self.preview_root = Path(preview_root)
        self.compose_plan_fn = compose_plan_fn
        # kind -> 分类函数 注册表（AG-20）。参数装配各 kind 不同，见 _execute_classify。
        self.classify_fns = classify_fns
        self.killable_stages = set(killable_stages)

    # -- 线程壳（两条 lane 各起一条线程） --

    def run_classify(self, stop_event: threading.Event, poll_seconds: float = 0.5) -> None:
        while not stop_event.is_set():
            self.step_classify(timeout=poll_seconds)

    def run_drive(self, stop_event: threading.Event, poll_seconds: float = 0.5) -> None:
        while not stop_event.is_set():
            self.step_drive(timeout=poll_seconds)

    def step_classify(self, timeout: float = 0.0) -> bool:
        return self._step(self.classify_jobs, timeout)

    def step_drive(self, timeout: float = 0.0) -> bool:
        return self._step(self.drive_jobs, timeout)

    def _step(self, jobs: "queue.Queue", timeout: float) -> bool:
        """从指定 lane 取一个 job 执行；队列空返回 False。未预期异常兜底
        成 JobCrashed 事件——worker 线程本身永不带病死掉。"""
        try:
            if timeout:
                job = jobs.get(timeout=timeout)
            else:
                job = jobs.get_nowait()
        except queue.Empty:
            return False
        _log.info(f"[worker] 开始 {self._describe_job(job)}")
        try:
            self._execute(job)
        except Exception as e:  # noqa: BLE001 兜底见 docstring
            # 静默崩溃是最糟的失败模式（用户和终端都看不到）：打全栈 +
            # 发 JobCrashed 让 consumer 回一句话过去。
            _log.exception(f"[worker] 崩了 {self._describe_job(job)}：{e!r}")
            # DriveJob 走 drive lane；ClassifyJob/ComposeJob 同在 classify lane。
            lane = "drive" if isinstance(job, DriveJob) else "classify"
            self.events.put(JobCrashed(generation=job.generation, lane=lane, error=repr(e)))
        else:
            _log.info(f"[worker] 完成 {self._describe_job(job)}")
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
        fn = self.classify_fns.get(job.kind)
        if fn is None:
            raise ValueError(f"unknown classify kind: {job.kind!r}")
        try:
            # fn 从注册表查（AG-20）；参数装配各 kind 不同，仍显式分派。
            if job.kind == "collecting":
                result = fn(job.text, job.context["photo_count"])
            elif job.kind == "gate_reply":
                # classify_gate_reply 的 swap_out 解析需要 run 的 Curate
                # 输出和 plan——load 一份只读副本传进去（读不受所有权协
                # 议限制，RunStore.save 是原子替换，读到的永远是完整文件）。
                run = self.store.load(job.context["run_id"])
                result = fn(job.text, run)
            elif job.kind == "refine_plan":
                result = fn(job.context["intent_raw"], job.context["current_params"], job.text)
            elif job.kind == "dedup_followup":
                result = fn(job.text, job.context["remaining"])
            else:
                # style_describe / style_gate / running / cancel_confirm：单 text。
                result = fn(job.text)
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
            # 闸门放行 = 现在才真正运行被闸门挡住的这个 stage（driver 内部经
            # _run_stage），_drive_to_stop 的循环只覆盖闸门之后的下游，所以
            # 这个 stage 的 StageStarted 要在这里补发（"正在交付..."出现在批准
            # 之后，AG-05）。
            if run.gate_state is not None:
                self.events.put(StageStarted(job.generation, run.run_id,
                                             run.gate_state.stage_name))
            self.driver.resolve_gate(run, "proceed")
        elif job.action == "adjustment":
            self.driver.apply_adjustment(run, job.args["delta"])
        elif job.action == "rerun_style":
            # rerun_style 跳过闸门直接跑 Style，同样在 driver 内部运行，补发。
            self.events.put(StageStarted(job.generation, run.run_id, "Style"))
            self.driver.rerun_stage(run, "Style", {"style_description": job.args["style_description"]})
            style_out = run.outputs.get("Style")
            if style_out is not None and style_out.data.get("match_failed"):
                # 描述没匹配上任何 preset：软失败，退回 Style 闸门重新问，不往下
                # 推进（AG-01）。不走 _drive_to_stop/_report_stop。
                self.driver.rearm_gate(run, "Style")
                self.events.put(GateReached(job.generation, run.run_id, "Style",
                                            {"match_failed": True}))
                return
        elif job.action == "rerun_curate":
            # 去重后追问的回复已经落地(留几张/不筛)，直接跑 Curate 拿答案
            # 生效，不需要闸门再问一遍（W2026-07-21 目标三决策四；同 rerun_style
            # 的用法）。Curate 在 KILLABLE_STAGES 里，这条路径绕开了
            # _drive_to_stop 的循环布防，这里手动布防/解防、接住取消。
            self.events.put(StageStarted(job.generation, run.run_id, "Curate"))
            self.client.cancel_event = job.cancel_event
            try:
                self.driver.rerun_stage(run, "Curate", job.args["params"])
            except PztCancelledError:
                # 不 return：跟 _drive_to_stop 循环里的取消处理一样，落到方法
                # 尾部共享的 _report_stop 才会真的发 RunFinished(cancelled)。
                self.driver.cancel(run)
            finally:
                self.client.cancel_event = None
        elif job.action != "resume":
            raise ValueError(f"unknown drive action: {job.action!r}")
        self._drive_to_stop(run, job)
        self._report_stop(run, job)

    def _drive_to_stop(self, run: RunState, job: DriveJob) -> None:
        while run.status == RunStatus.RUNNING:
            if job.cancel_event.is_set():
                self.driver.cancel(run)
                return
            next_spec = self.driver.peek_next_spec(run)
            # 只为"这一轮真的会运行"的 stage 发 StageStarted。带闸门且闸门未
            # 开的 stage，advance() 只会停在闸门不运行它（判据同 driver.advance），
            # 此时发"正在交付..."会紧贴闸门提问自相矛盾（AG-05）——闸门放行后的
            # 实际运行由 _execute_drive 在 resolve_gate/rerun_style 前补发。
            stops_at_gate = (next_spec is not None
                             and next_spec.gate != "off" and run.gate_state is None)
            if next_spec is not None and not stops_at_gate:
                _log.info(f"[worker] run={run.run_id} 运行 stage={next_spec.name}")
                self.events.put(StageStarted(job.generation, run.run_id, next_spec.name))
            armed = (next_spec.name if next_spec else None) in self.killable_stages
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
            # 阶段一：真正筛选过（不是"不筛选了"passthrough）才有选片结果
            # 可展示；确认了才问风格（真机反馈：选片确认放在滤镜之前，
            # Deliver 也不再挂闸门二次预览，见 consumer.py 决策同批改动）。
            curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
            if curate_spec.params.get("count") is None:
                return {}
            curate_output = run.outputs.get("Curate")
            selected = curate_output.data.get("selected", []) if curate_output else []
            payload = {"selected_count": len(selected), "preview_failed_count": 0, "export_error": None}
            if not selected:
                return payload
            export_error = self._export_previews(run, selected)
            if export_error is not None:
                payload["export_error"] = export_error
                return payload
            payload["preview_failed_count"] = self._send_preview_media(run, selected, numbered=True)
            return payload
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
        if stage == "Curate":
            # 去重后追问："还剩几张，要不要再筛"（W2026-07-21 目标三决策四）。
            # ai_enabled 一起带上：consumer 靠它判断要不要提醒"可以用 AI"（决策五）。
            ingest_output = run.outputs.get("Ingest")
            dedup_output = run.outputs.get("Dedup")
            total = ingest_output.data.get("image_count", 0) if ingest_output else 0
            tagged = dedup_output.data.get("tagged", 0) if dedup_output else 0
            curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
            return {"remaining": total - tagged, "ai_enabled": curate_spec.params.get("ai_enabled", False)}
        return {}  # Deliver 不挂闸门，这里理论上不会被调用；留一个安全兜底

    def _export_previews(self, run: RunState, selected: list) -> "str | None":
        # selected 是项目 root_path 相对路径，必须先过一次 export-images
        # 才有真字节可发（跟旧 _send_preview / stages/deliver.py 同理）。
        preview_dir = self.preview_root / run.run_id
        # 先清掉上一次导出的预览：export-images 遇到同名目标会消歧成
        # name_2.jpg（core/export/export.cpp::resolve_collision），而
        # _send_preview_media 永远发 name.jpg——不清就会把换风格前的旧预览
        # 又发一遍（真机反馈：换滤镜后预览图没跟着变，但交付的图是对的）。
        shutil.rmtree(preview_dir, ignore_errors=True)
        try:
            self.client.call("export-images", run.project_id, *selected, str(preview_dir))
        except PztCommandError as e:
            return e.message
        return None

    def _send_preview_media(self, run: RunState, selected: list, numbered: bool = False) -> int:
        # 逐张 图->文件 降级重试，最后统计失败数（旧 _send_preview 的
        # "一张超标图不能带崩整个预览循环"语义原样保留）。numbered=True 时每
        # 张带"第 N 张"caption（Deliver 的"换掉第3张"以此为锚，AG-15）。
        preview_dir = self.preview_root / run.run_id
        failed = 0
        for i, path in enumerate(selected, start=1):
            preview_path = str(preview_dir / Path(path).name)
            caption = f"第 {i} 张" if numbered else None
            try:
                self.transport.send_photo(self.chat_id, preview_path, caption=caption)
            except Exception:  # noqa: BLE001 Telegram 体积上限等，降级不挑异常
                try:
                    self.transport.send_file(self.chat_id, preview_path)
                except Exception:  # noqa: BLE001
                    if numbered:
                        # 保序占位：这张发不出去也占个位，用户按"第 N 张"调整
                        # 不会数错（AG-15）。
                        try:
                            self.transport.send_text(self.chat_id, f"第 {i} 张预览发送失败")
                        except Exception:  # noqa: BLE001
                            pass
                    failed += 1
        return failed
