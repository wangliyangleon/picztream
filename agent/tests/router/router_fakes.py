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

import json

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
from stages.style_apply_all import StyleApplyAllStage
from store.run_store import RunStore
from transport.base import InboundMessage

CHAT_ID = "42"

_FIXED_RESPONSES = {
    "new": '{"project": "run-1", "image_count": 2}',
    "eval": '{"submitted": 2, "evaluated": [], "failed": []}',
    "dedup": '{"groups": 2, "tagged": 0, "skipped_no_capture_time": 0}',
    "curate": '{"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"]}',
    "tag": '{}',
    "recipe": '{"applied": true, "recipe_name": "Havana 1959"}',
    "export-images": '{"exported": 2, "skipped": [], "created_dir": true}',
}


def _fake_style_http_post(recipe_name: str = "Havana 1959"):
    # StyleStage.run() 现在直接调 compose/llm_client.py::request_json(...)
    # 做文本匹配，不再经过 PztClient 那条子进程边界(那条已经被
    # _fake_runner 接管)，需要单独给一个 http_post 假实现，否则测试跑
    # 到 Style 真正执行时会打真实网络请求到本地 Ollama——这次会话已经
    # 真机复现过一次因为漏了 fake 而导致 pytest 挂起的事故，同一类坑。
    def fn(url, headers, body):
        del url, headers, body
        response = {"message": {"content": json.dumps(
            {"recipe_name": recipe_name, "reasoning": "fits the mood"})}}
        return 200, json.dumps(response)
    return fn


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
        # provider="local"：request_json 的 gemini/claude 分支即使传了
        # http_post 也会先无条件校验 API key(见 llm_client.py)，测试没
        # 配 key，必须用 local 分支才能真正吃到下面注入的假 http_post。
        StageSpec(name="Style", params={"provider": "local"}, gate="required"),
        StageSpec(name="StyleApplyAll", gate="required"),
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
                  idle_reminder_seconds: float = 300.0, progress_interval_seconds: float = 60.0,
                  refine_plan_confirmation_fn: Callable = refine_plan_confirmation,
                  classify_collecting_message_fn: Callable = _default_classify_collecting_message,
                  style_http_post: Callable = None,
                  ) -> Tuple[SessionRouter, RunStore, FakeTransport, PztClient]:
    client = PztClient(pzt_bin="/fake/pzt", runner=runner)
    transport = FakeTransport()
    store = RunStore(tmp_path / "runs")
    stages = {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Style": StyleStage(client=client, http_post=style_http_post or _fake_style_http_post()),
        "StyleApplyAll": StyleApplyAllStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered",
                                 staging_dir=tmp_path / "staging", chat_id=CHAT_ID, inputs=["StyleApplyAll"]),
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
        progress_interval_seconds=progress_interval_seconds,
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
    # 停在 Curate 选片结果的 Deliver 闸门预览——这是这个 helper 对一大批
    # 不关心 Style 细节的测试保持的既有契约，新增的 Style/StyleApplyAll
    # 两段闸门在中途被这里顺手驱动过去，不暴露给调用方。
    tmp_path = router.incoming_root.parent
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                          file_path=_stage_source_photo(tmp_path, "a.jpg", b"a")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                          file_path=_stage_source_photo(tmp_path, "b.jpg", b"b")))
    router.handle_message(_text_msg("筛一下留2张"))  # 现在只会停在 PLANNED，等确认
    router.handle_message(_text_msg("好的"))          # PLANNED -> RUNNING -> 停在 Style 闸门
    router.handle_message(_text_msg("复古暖色调"))     # Style 闸门：给描述 -> 停在 StyleApplyAll 闸门
    return router.handle_message(_text_msg("好的"))    # StyleApplyAll 闸门：同意 -> 停在 Deliver 闸门
