"""router.SessionRouter 测试专用共享 fake/helper，跟 tests/orchestrator/
fakes.py 的约定一致：同目录裸模块名导入，不建 conftest.py（仓库里目前
没有任何 conftest.py，per-test-dir 共享 helper 模块是既有约定）。命名
成 router_fakes 而不是 fakes：pytest 默认 import mode 下裸模块名全局
只认一个，tests/orchestrator/fakes.py 已经占了 "fakes" 这个名字，两个
目录都叫 fakes.py 会在跑全量套件时互相顶掉。
"""
from __future__ import annotations

import subprocess
import time
from pathlib import Path
from typing import Callable, List, Optional, Tuple

from compose.adjustment_parser import AdjustmentError, classify_gate_reply, refine_plan_confirmation
from orchestrator.driver import Driver
from orchestrator.types import Plan, StageSpec
from pzt_client import PztClient
from router.session_router import SessionRouter
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from stages.style import StyleStage
from store.run_store import RunStore
from transport.base import InboundMessage

CHAT_ID = "42"

_FIXED_RESPONSES = {
    "new": '{"project": "run-1", "image_count": 2}',
    "eval": '{"submitted": 2, "evaluated": [], "failed": []}',
    "dedup": '{"groups": 2, "tagged": 0, "skipped_no_capture_time": 0}',
    "curate": '{"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"]}',
    "tag": '{}',
    "recipe": '{"recipe_name": "Havana 1959", "reasoning": "warm mood fits"}',
    "export-images": '{"exported": 2, "skipped": [], "created_dir": true}',
}


def _default_classify_collecting_message(text: str, photo_count: int):
    # _run_to_gate 发的"筛一下留2张"意图消息会经过 _handle_collecting 的
    # classify_collecting_message_fn 这一步——真实实现现在默认走本地
    # Ollama(meta_provider="local")，测试里没有真的 Ollama 可连，用一个
    # 立刻失败的假实现复现"分类只是锦上添花，失败就照老办法当意图处
    # 理"这条既有降级路径(router/session_router.py::_handle_collecting
    # 的 try/except)，不让测试真的发网络请求。需要测分类结果本身的用
    # classify_collecting_message_fn= 显式传自己的假实现覆盖这个默认值
    # (见 test_session_router_query.py)。
    del text, photo_count
    raise AdjustmentError("no_llm_in_tests", "classify_collecting_message not faked in this test")


def _fake_runner(argv: List[str]) -> subprocess.CompletedProcess:
    subcommand = argv[1]
    return subprocess.CompletedProcess(argv, 0, stdout=_FIXED_RESPONSES[subcommand], stderr="")


def _fake_compose_plan(intent: str, profile: Optional[str], last_config: Optional[Plan]) -> Plan:
    del intent, profile, last_config  # 只回一份固定 Plan，真实 compose_plan 的 LLM 逻辑不在这里测
    return Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Evaluate", params={"provider": "gemini", "auto_reject": True}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 2, "apply_tag": "精选"}),
        StageSpec(name="Style", params={"provider": "gemini"}),
        StageSpec(name="Deliver"),
    ])


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


def _make_router(tmp_path: Path, compose_plan_fn: Callable = _fake_compose_plan,
                  runner: Callable = _fake_runner, now_fn: Callable[[], float] = time.time,
                  idle_reminder_seconds: float = 300.0,
                  refine_plan_confirmation_fn: Callable = refine_plan_confirmation,
                  classify_collecting_message_fn: Callable = _default_classify_collecting_message
                  ) -> Tuple[SessionRouter, RunStore, FakeTransport, PztClient]:
    client = PztClient(pzt_bin="/fake/pzt", runner=runner)
    transport = FakeTransport()
    store = RunStore(tmp_path / "runs")
    stages = {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Style": StyleStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered",
                                 staging_dir=tmp_path / "staging", chat_id=CHAT_ID),
    }
    driver = Driver(stages=stages, store=store)
    router = SessionRouter(
        store=store, driver=driver, transport=transport, client=client, chat_id=CHAT_ID,
        incoming_root=tmp_path / "incoming", preview_root=tmp_path / "preview",
        deliver_out_folder=tmp_path / "deliver-out",
        compose_plan_fn=compose_plan_fn, classify_gate_reply_fn=classify_gate_reply,
        refine_plan_confirmation_fn=refine_plan_confirmation_fn,
        classify_collecting_message_fn=classify_collecting_message_fn,
        now_fn=now_fn, idle_reminder_seconds=idle_reminder_seconds,
    )
    return router, store, transport, client


def _text_msg(text: str) -> InboundMessage:
    return InboundMessage(kind="text", chat_id=CHAT_ID, text=text)


def _stage_source_photo(tmp_path: Path, name: str, content: bytes = b"x") -> str:
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir(parents=True, exist_ok=True)
    src = downloaded / name
    src.write_bytes(content)
    return str(src)


def _run_to_gate(router: SessionRouter):
    tmp_path = router.incoming_root.parent
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                          file_path=_stage_source_photo(tmp_path, "a.jpg", b"a")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                          file_path=_stage_source_photo(tmp_path, "b.jpg", b"b")))
    router.handle_message(_text_msg("筛一下留2张"))  # 现在只会停在 PLANNED，等确认
    return router.handle_message(_text_msg("好的"))  # 确认后才真正开跑，到 AwaitingGate
