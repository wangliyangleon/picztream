"""tests/session 专用共享 fake/helper，命名 session_fakes 不叫 fakes：
pytest 默认 import mode 下裸模块名全局只认一个（tests/orchestrator/
fakes.py 已占用），所以每个 test 目录用带前缀的 helper 模块名。各目录的
fake 各自独立、互不跨目录 import（跨目录裸模块导入依赖 sys.path 插入顺
序，单跑一个文件时不可靠）。
"""
from __future__ import annotations

import copy
import json
import queue
from pathlib import Path
from typing import Callable, List, Optional, Tuple

from compose.adjustment_parser import AdjustmentError
from orchestrator.driver import Driver
from orchestrator.types import GateState, Plan, RunState, RunStatus, StageSpec, StageStatus
from pzt_client import PztCancelledError, PztCommandError
from router.collecting import incoming_dir_for
from session.worker import SessionWorker
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from stages.style import StyleStage
from stages.style_apply_all import StyleApplyAllStage
from store.run_store import RunStore

CHAT_ID = "42"

_FIXED_RESPONSES = {
    "new": {"project": "run-1", "image_count": 2},
    "eval": {"submitted": 2, "evaluated": [], "failed": []},
    "dedup": {"groups": 2, "tagged": 0, "skipped_no_capture_time": 0},
    "curate": {"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"]},
    "tag": {},
    "recipe": {"applied": True, "recipe_name": "Havana 1959"},
    "export-images": {"exported": 2, "skipped": [], "created_dir": True},
    "delete": {"deleted": "run-1"},
    # consumer 的 Evaluate 进度轮询用（cli/commands/commands.cpp::cmd_images
    # 的输出形状，逐张带 evaluated 布尔）。
    "images": {"project": "run-1", "images": [
        {"path": "a.jpg", "evaluated": True, "tags": []},
        {"path": "b.jpg", "evaluated": False, "tags": []},
    ]},
}


class FakeClient:
    """PztClient 替身（duck-type）：worker 只碰 cancel_event 布防点，
    stages 只调 call()。armed_during 记录每个子命令被调用那一刻布防与
    否，测"可杀白名单才布防"的协议；raise_cancelled_on 模拟"布防中的子
    进程执行途中被 terminate"——真实 terminate/kill 机械已在
    tests/test_pzt_client_cancel.py 单测过，这里不重复 Popen 细节。"""

    def __init__(self, raise_cancelled_on: Tuple[str, ...] = (),
                 raise_command_on: Tuple[str, ...] = ()) -> None:
        self.cancel_event = None
        self.calls: List[Tuple[str, ...]] = []
        self.armed_during: dict = {}
        self.raise_cancelled_on = set(raise_cancelled_on)
        # 命中这些子命令时抛 PztCommandError，模拟子进程返回非零（如
        # export-images 导出失败），测 stage 的 ok=False 通路。
        self.raise_command_on = set(raise_command_on)

    def call(self, *args: str) -> dict:
        subcommand = args[0]
        self.calls.append(args)
        self.armed_during[subcommand] = self.cancel_event is not None
        if self.cancel_event is not None and (
                subcommand in self.raise_cancelled_on or self.cancel_event.is_set()):
            raise PztCancelledError(list(args))
        if subcommand in self.raise_command_on:
            raise PztCommandError("fake_error", f"{subcommand} failed in test")
        return copy.deepcopy(_FIXED_RESPONSES[subcommand])


class FakeTransport:
    def __init__(self) -> None:
        self.inbox: List = []
        self.sent_texts: List[Tuple[str, str]] = []
        self.sent_photos: List[Tuple[str, str]] = []
        self.sent_photo_captions: List = []  # 与 sent_photos 一一对应的 caption
        self.sent_files: List[Tuple[str, str]] = []
        self.sent_buttons: List[Tuple[str, str, List]] = []

    def receive(self):
        messages, self.inbox = self.inbox, []
        return messages

    def send_text(self, chat_id: str, text: str) -> None:
        self.sent_texts.append((chat_id, text))

    def send_buttons(self, chat_id: str, text: str, options: List) -> None:
        # 也记进 sent_texts，让既有的 texts() 文案断言对带按钮的消息一样生效。
        self.sent_texts.append((chat_id, text))
        self.sent_buttons.append((chat_id, text, options))

    def button_tokens(self) -> List[str]:
        # 最近一条带按钮消息的动作 token（去掉 ":run_id" 后缀），方便断言。
        if not self.sent_buttons:
            return []
        return [data.split(":", 1)[0] for _, data in self.sent_buttons[-1][2]]

    def send_photo(self, chat_id: str, path: str, caption=None) -> None:
        self.sent_photos.append((chat_id, path))
        self.sent_photo_captions.append(caption)

    def send_file(self, chat_id: str, path: str) -> None:
        self.sent_files.append((chat_id, path))

    def texts(self) -> List[str]:
        return [text for _, text in self.sent_texts]


class FakeClock:
    def __init__(self, start: float = 1_700_000_000.0) -> None:
        self.now = start

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


def _fake_style_http_post(recipe_name: str = "Havana 1959"):
    # StyleStage 的文本匹配不经过 FakeClient 的子进程
    # 边界，必须单独假掉，否则测试会真打本地 Ollama。
    def fn(url, headers, body):
        del url, headers, body
        response = {"message": {"content": json.dumps(
            {"recipe_name": recipe_name, "reasoning": "fits the mood"})}}
        return 200, json.dumps(response)
    return fn


def _raising_classify(*args, **kwargs):
    raise AdjustmentError("no_llm_in_tests", "classify fn not faked in this test")


def bare_compose_plan() -> Plan:
    # compose_plan 的输出形状：还没经过 consumer 的参数注入（无 Ingest
    # folder / Deliver out_folder / Deliver 闸门）。
    return Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Evaluate", params={"provider": "gemini", "auto_reject": True}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 2, "apply_tag": "精选"}),
        StageSpec(name="Style", params={"provider": "local"}, gate="required"),
        StageSpec(name="StyleApplyAll", gate="required"),
        StageSpec(name="Deliver"),
    ])


def to_planned(env: "ConsumerEnv") -> RunState:
    """photo -> 意图文本 -> classify 降级 -> compose 成功 -> PLANNED。"""
    from session.protocol import ClassifyFailed, ComposeDone

    env.push_photo("a.jpg", b"a")
    env.consumer.step()
    env.push_text("筛一下留2张")
    env.consumer.step()
    env.drain_jobs()
    gen = env.consumer.generation
    env.put_event(ClassifyFailed(gen, "collecting", retryable=True))
    env.consumer.step()
    env.drain_jobs()
    env.put_event(ComposeDone(gen, bare_compose_plan()))
    env.consumer.step()
    return env.consumer.run


def to_running(env: "ConsumerEnv"):
    """to_planned -> "好的"(refine 分类判 approve) -> DriveJob(start)。返回 DriveJob。"""
    from compose.adjustment_parser import PlanConfirmationReply
    from session.protocol import ClassifyDone

    to_planned(env)
    env.push_text("好的")
    env.consumer.step()
    env.drain_jobs()  # 排掉 refine_plan ClassifyJob
    gen = env.consumer.generation
    env.put_event(ClassifyDone(gen, "refine_plan", PlanConfirmationReply(action="approve")))
    env.consumer.step()
    return env.drain_jobs()[-1]


def deliver_classify(env: "ConsumerEnv", kind: str, reply) -> None:
    """模拟 classify lane 返回：排掉 consumer 刚投的 ClassifyJob，回一个
    ClassifyDone(kind, reply) 事件再 step。彻底去关键词后，几乎每条用户文
    本都要过这一步。"""
    from session.protocol import ClassifyDone
    env.drain_jobs()
    env.put_event(ClassifyDone(env.consumer.generation, kind, reply))
    env.consumer.step()


def worker_saves_gate(env: "ConsumerEnv", run_id: str, stage: str) -> None:
    """模拟 worker 停在闸门时的落盘（GateReached 事件发出前 run 已是
    AWAITING_GATE + gate_state，见 Driver.advance）。"""
    run = env.store.load(run_id)
    run.status = RunStatus.AWAITING_GATE
    run.gate_state = GateState(stage_name=stage, setting="required")
    env.store.save(run)


def make_fixed_plan(incoming_dir: str, out_folder: str) -> Plan:
    # 形状对齐 router 侧 _propose_plan 的注入结果：Ingest folder、Deliver
    # out_folder + required 闸门、Style/StyleApplyAll required 闸门。
    return Plan(stages=[
        StageSpec(name="Ingest", params={"folder": incoming_dir}),
        StageSpec(name="Evaluate", params={"provider": "gemini", "auto_reject": True}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 2, "apply_tag": "精选"}),
        StageSpec(name="Style", params={"provider": "local"}, gate="required"),
        StageSpec(name="StyleApplyAll", gate="required"),
        StageSpec(name="Deliver", params={"out_folder": out_folder}, gate="required"),
    ])


class WorkerEnv:
    def __init__(self, tmp_path: Path, worker: SessionWorker, classify_jobs: "queue.Queue",
                 drive_jobs: "queue.Queue", events: "queue.Queue", store: RunStore,
                 transport: FakeTransport, client: FakeClient, driver: Driver) -> None:
        self.tmp_path = tmp_path
        self.worker = worker
        self.classify_jobs = classify_jobs
        self.drive_jobs = drive_jobs
        self.events = events
        self.store = store
        self.transport = transport
        self.client = client
        self.driver = driver

    def put_classify(self, job) -> None:
        self.classify_jobs.put(job)

    def put_drive(self, job) -> None:
        self.drive_jobs.put(job)

    def step_classify(self) -> bool:
        return self.worker.step_classify()

    def step_drive(self) -> bool:
        return self.worker.step_drive()

    def step(self) -> bool:
        # 测试便利：先跑 classify lane 再跑 drive lane（测试一次通常只投一
        # 种 job），返回是否执行了 job。
        return self.worker.step_classify() or self.worker.step_drive()

    def drain_events(self) -> list:
        out = []
        while True:
            try:
                out.append(self.events.get_nowait())
            except queue.Empty:
                return out

    def make_running_run(self, run_id: str = "tg-w1") -> RunState:
        # consumer 在投 DriveJob(start) 前就把 run 置成 RUNNING 并落盘
        # （见 Eng Design 第七节），这里直接从那个交接点开始。
        incoming = incoming_dir_for(self.tmp_path / "incoming", run_id)
        plan = make_fixed_plan(str(incoming), str(self.tmp_path / "deliver-out"))
        run = RunState(
            run_id=run_id, project_id=run_id, plan=plan,
            stage_states={s.name: StageStatus.PENDING for s in plan.stages},
            status=RunStatus.RUNNING,
        )
        self.store.save(run)
        return run


class ConsumerEnv:
    def __init__(self, tmp_path: Path, consumer, classify_jobs: "queue.Queue",
                 drive_jobs: "queue.Queue", events: "queue.Queue",
                 store: RunStore, transport: FakeTransport, client: FakeClient,
                 clock: FakeClock) -> None:
        self.tmp_path = tmp_path
        self.consumer = consumer
        self.classify_jobs = classify_jobs
        self.drive_jobs = drive_jobs
        self.events = events
        self.store = store
        self.transport = transport
        self.client = client
        self.clock = clock

    def push_text(self, text: str) -> None:
        from transport.base import InboundMessage
        self.transport.inbox.append(InboundMessage(kind="text", chat_id=CHAT_ID, text=text))

    def push_photo(self, name: str, content: bytes = b"x") -> Path:
        from transport.base import InboundMessage
        src_dir = self.tmp_path / "downloaded"
        src_dir.mkdir(parents=True, exist_ok=True)
        src = src_dir / name
        src.write_bytes(content)
        self.transport.inbox.append(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                                    file_path=str(src)))
        return src

    def push_callback(self, data: str) -> None:
        from transport.base import InboundMessage
        self.transport.inbox.append(InboundMessage(kind="callback", chat_id=CHAT_ID, data=data))

    def drain_jobs(self) -> list:
        # 两条 lane 合并排空，保持大量"投了哪个 job"断言不用区分 lane。
        out = []
        for q in (self.classify_jobs, self.drive_jobs):
            while True:
                try:
                    out.append(q.get_nowait())
                except queue.Empty:
                    break
        return out

    def put_event(self, event) -> None:
        self.events.put(event)


def make_consumer(tmp_path: Path, clock: Optional[FakeClock] = None,
                  client: Optional[FakeClient] = None,
                  idle_reminder_seconds: float = 300.0,
                  progress_interval_seconds: float = 60.0,
                  eval_poll_interval_seconds: float = 60.0,
                  terminal_retention_seconds: float = 7 * 86400) -> ConsumerEnv:
    from session.consumer import SessionConsumer

    clock = clock or FakeClock()
    client = client or FakeClient()
    transport = FakeTransport()
    store = RunStore(tmp_path / "runs")
    classify_jobs: "queue.Queue" = queue.Queue()
    drive_jobs: "queue.Queue" = queue.Queue()
    events: "queue.Queue" = queue.Queue()
    # consumer 只用 driver.cancel/approve，这两个方法不碰 stages。
    driver = Driver(stages={}, store=store)
    consumer = SessionConsumer(
        store=store, driver=driver, transport=transport, chat_id=CHAT_ID,
        incoming_root=tmp_path / "incoming", deliver_out_folder=tmp_path / "deliver-out",
        classify_jobs=classify_jobs, drive_jobs=drive_jobs, events=events,
        readonly_client=client, now_fn=clock,
        idle_reminder_seconds=idle_reminder_seconds,
        progress_interval_seconds=progress_interval_seconds,
        eval_poll_interval_seconds=eval_poll_interval_seconds,
        send_retry_backoff_seconds=0.0,  # 测试不真 sleep
        preview_root=tmp_path / "preview", staging_dir=tmp_path / "staging",
        marker_dir=tmp_path / "delivered",
        terminal_retention_seconds=terminal_retention_seconds,
    )
    return ConsumerEnv(tmp_path=tmp_path, consumer=consumer, classify_jobs=classify_jobs,
                       drive_jobs=drive_jobs, events=events,
                       store=store, transport=transport, client=client, clock=clock)


def make_worker(tmp_path: Path,
                client: Optional[FakeClient] = None,
                compose_plan_fn: Callable = None,
                classify_collecting_message_fn: Callable = _raising_classify,
                classify_gate_reply_fn: Callable = _raising_classify,
                refine_plan_confirmation_fn: Callable = _raising_classify,
                classify_style_gate_reply_fn: Callable = _raising_classify,
                classify_running_message_fn: Callable = _raising_classify,
                classify_cancel_confirmation_fn: Callable = _raising_classify,
                classify_style_describe_fn: Callable = _raising_classify,
                ) -> WorkerEnv:
    client = client or FakeClient()
    transport = FakeTransport()
    store = RunStore(tmp_path / "runs")
    classify_jobs: "queue.Queue" = queue.Queue()
    drive_jobs: "queue.Queue" = queue.Queue()
    events: "queue.Queue" = queue.Queue()
    stages = {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Style": StyleStage(client=client, http_post=_fake_style_http_post()),
        "StyleApplyAll": StyleApplyAllStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport,
                                 marker_dir=tmp_path / "delivered",
                                 staging_dir=tmp_path / "staging", chat_id=CHAT_ID,
                                 inputs=["StyleApplyAll"]),
    }
    driver = Driver(stages=stages, store=store)
    worker = SessionWorker(
        classify_jobs=classify_jobs, drive_jobs=drive_jobs, events=events,
        driver=driver, store=store, client=client,
        transport=transport, chat_id=CHAT_ID, preview_root=tmp_path / "preview",
        compose_plan_fn=compose_plan_fn or (lambda intent, profile, last: make_fixed_plan(
            str(tmp_path / "incoming" / "unused"), str(tmp_path / "deliver-out"))),
        classify_collecting_message_fn=classify_collecting_message_fn,
        classify_gate_reply_fn=classify_gate_reply_fn,
        refine_plan_confirmation_fn=refine_plan_confirmation_fn,
        classify_style_gate_reply_fn=classify_style_gate_reply_fn,
        classify_running_message_fn=classify_running_message_fn,
        classify_cancel_confirmation_fn=classify_cancel_confirmation_fn,
        classify_style_describe_fn=classify_style_describe_fn,
    )
    return WorkerEnv(tmp_path=tmp_path, worker=worker, classify_jobs=classify_jobs,
                     drive_jobs=drive_jobs, events=events,
                     store=store, transport=transport, client=client, driver=driver)
