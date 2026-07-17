"""tests/session 专用共享 fake/helper，命名 session_fakes 不叫 fakes：
pytest 默认 import mode 下裸模块名全局只认一个（tests/orchestrator/
fakes.py 已占用），跟 tests/router/router_fakes.py 的既有约定一致。
FakeTransport 与 router_fakes 里的实现重复是有意的——per-test-dir 独立
helper 模块、互不跨目录 import（跨目录裸模块导入依赖 sys.path 插入顺
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
from orchestrator.types import Plan, RunState, RunStatus, StageSpec, StageStatus
from pzt_client import PztCancelledError
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
}


class FakeClient:
    """PztClient 替身（duck-type）：worker 只碰 cancel_event 布防点，
    stages 只调 call()。armed_during 记录每个子命令被调用那一刻布防与
    否，测"可杀白名单才布防"的协议；raise_cancelled_on 模拟"布防中的子
    进程执行途中被 terminate"——真实 terminate/kill 机械已在
    tests/test_pzt_client_cancel.py 单测过，这里不重复 Popen 细节。"""

    def __init__(self, raise_cancelled_on: Tuple[str, ...] = ()) -> None:
        self.cancel_event = None
        self.calls: List[Tuple[str, ...]] = []
        self.armed_during: dict = {}
        self.raise_cancelled_on = set(raise_cancelled_on)

    def call(self, *args: str) -> dict:
        subcommand = args[0]
        self.calls.append(args)
        self.armed_during[subcommand] = self.cancel_event is not None
        if self.cancel_event is not None and (
                subcommand in self.raise_cancelled_on or self.cancel_event.is_set()):
            raise PztCancelledError(list(args))
        return copy.deepcopy(_FIXED_RESPONSES[subcommand])


class FakeTransport:
    def __init__(self) -> None:
        self.sent_texts: List[Tuple[str, str]] = []
        self.sent_photos: List[Tuple[str, str]] = []
        self.sent_files: List[Tuple[str, str]] = []

    def receive(self):
        return []

    def send_text(self, chat_id: str, text: str) -> None:
        self.sent_texts.append((chat_id, text))

    def send_photo(self, chat_id: str, path: str) -> None:
        self.sent_photos.append((chat_id, path))

    def send_file(self, chat_id: str, path: str) -> None:
        self.sent_files.append((chat_id, path))


class FakeClock:
    def __init__(self, start: float = 1_700_000_000.0) -> None:
        self.now = start

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


def _fake_style_http_post(recipe_name: str = "Havana 1959"):
    # 同 router_fakes：StyleStage 的文本匹配不经过 FakeClient 的子进程
    # 边界，必须单独假掉，否则测试会真打本地 Ollama。
    def fn(url, headers, body):
        del url, headers, body
        response = {"message": {"content": json.dumps(
            {"recipe_name": recipe_name, "reasoning": "fits the mood"})}}
        return 200, json.dumps(response)
    return fn


def _raising_classify(*args, **kwargs):
    raise AdjustmentError("no_llm_in_tests", "classify fn not faked in this test")


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
    def __init__(self, tmp_path: Path, worker: SessionWorker, jobs: "queue.Queue",
                 events: "queue.Queue", store: RunStore, transport: FakeTransport,
                 client: FakeClient, driver: Driver) -> None:
        self.tmp_path = tmp_path
        self.worker = worker
        self.jobs = jobs
        self.events = events
        self.store = store
        self.transport = transport
        self.client = client
        self.driver = driver

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


def make_worker(tmp_path: Path,
                client: Optional[FakeClient] = None,
                compose_plan_fn: Callable = None,
                classify_collecting_message_fn: Callable = _raising_classify,
                classify_gate_reply_fn: Callable = _raising_classify,
                refine_plan_confirmation_fn: Callable = _raising_classify,
                ) -> WorkerEnv:
    client = client or FakeClient()
    transport = FakeTransport()
    store = RunStore(tmp_path / "runs")
    jobs: "queue.Queue" = queue.Queue()
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
        jobs=jobs, events=events, driver=driver, store=store, client=client,
        transport=transport, chat_id=CHAT_ID, preview_root=tmp_path / "preview",
        compose_plan_fn=compose_plan_fn or (lambda intent, profile, last: make_fixed_plan(
            str(tmp_path / "incoming" / "unused"), str(tmp_path / "deliver-out"))),
        classify_collecting_message_fn=classify_collecting_message_fn,
        classify_gate_reply_fn=classify_gate_reply_fn,
        refine_plan_confirmation_fn=refine_plan_confirmation_fn,
    )
    return WorkerEnv(tmp_path=tmp_path, worker=worker, jobs=jobs, events=events,
                     store=store, transport=transport, client=client, driver=driver)
