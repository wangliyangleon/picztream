"""run_telegram2.build_runtime 的接线测试 + 一个真线程端到端冒烟。前者
验证 wiring（stages/client/queue 都接对、compose/classify 用真函数、
Deliver chat_id 对）；后者起真 worker 线程，用假 transport 把一条完整
的收图->意图->确认->drive->两段风格闸门->交付走通，验证双线程 + 队列
在真并发下不死锁、消息顺序合理（这是 step() 单步测试覆盖不到的一层）。
"""
from __future__ import annotations

import queue
import threading
import time
from typing import List, Tuple

from compose.adjustment_parser import AdjustmentError
from run_telegram2 import build_runtime
from transport.base import InboundMessage

from session_fakes import CHAT_ID, FakeClient, _fake_style_http_post, bare_compose_plan


def _raising_collecting(text, n):
    # 分类基础设施不可用 -> 走"当意图处理"降级路径（无真 Ollama）。
    raise AdjustmentError("no_llm", "faked in smoke test")


class ScriptedTransport:
    """真线程冒烟用的 transport 替身：inbox 线程安全，send_* 记录带序号
    以便断言顺序。receive() 每次弹出当前 inbox。"""

    def __init__(self) -> None:
        self._inbox: "queue.Queue[InboundMessage]" = queue.Queue()
        self._lock = threading.Lock()
        self.log: List[Tuple[str, str]] = []  # ("text"|"photo"|"file", payload)

    def feed(self, msg: InboundMessage) -> None:
        self._inbox.put(msg)

    def receive(self):
        out = []
        while True:
            try:
                out.append(self._inbox.get_nowait())
            except queue.Empty:
                return out

    def send_text(self, chat_id, text):
        with self._lock:
            self.log.append(("text", text))

    def send_photo(self, chat_id, path):
        with self._lock:
            self.log.append(("photo", path))

    def send_file(self, chat_id, path):
        with self._lock:
            self.log.append(("file", path))

    def texts(self) -> List[str]:
        with self._lock:
            return [payload for kind, payload in self.log if kind == "text"]


def test_build_runtime_wires_queues_stages_and_real_functions(tmp_path, monkeypatch):
    from compose.adjustment_parser import (
        classify_collecting_message,
        classify_gate_reply,
        refine_plan_confirmation,
    )
    from compose.plan_composer import compose_plan

    transport = ScriptedTransport()
    consumer, worker = build_runtime(state_dir=tmp_path, transport=transport, chat_id="42")

    # 同一对 job/event 队列
    assert consumer.jobs is worker.jobs
    assert consumer.events is worker.events
    # worker 与 consumer 用不同 client 实例（布防 vs 只读）
    assert worker.client is not consumer.readonly_client
    # 真函数接上
    assert worker.compose_plan_fn is compose_plan
    assert worker.classify_gate_reply_fn is classify_gate_reply
    assert worker.refine_plan_confirmation_fn is refine_plan_confirmation
    assert worker.classify_collecting_message_fn is classify_collecting_message
    # Deliver 的 chat_id 与目录
    assert worker.driver.stages["Deliver"].chat_id == "42"
    assert (tmp_path / "incoming").is_dir()
    assert (tmp_path / "preview").is_dir()
    assert (tmp_path / "deliver-out").is_dir()


def _wait_until(predicate, timeout=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return False


def test_two_thread_end_to_end_smoke(tmp_path):
    # 用假 client/分类函数把 LLM 和 pzt 子进程都短路掉，只验证 consumer
    # 主循环 + 真 worker 线程 + 两个队列在真并发下把整条链路走通。
    transport = ScriptedTransport()
    consumer, worker = build_runtime(state_dir=tmp_path, transport=transport, chat_id=CHAT_ID)

    fake_client = FakeClient()
    for stage in worker.driver.stages.values():
        stage.client = fake_client
    worker.driver.stages["Style"].http_post = _fake_style_http_post()
    worker.client = fake_client
    worker.compose_plan_fn = lambda intent, profile, last: bare_compose_plan()
    worker.classify_collecting_message_fn = _raising_collecting

    stop_event = threading.Event()
    worker_thread = threading.Thread(target=worker.run, args=(stop_event, 0.05), daemon=True)
    worker_thread.start()

    def pump(msg, until, timeout=3.0):
        transport.feed(msg)
        deadline = time.time() + timeout
        while time.time() < deadline:
            consumer.step()
            if until():
                return True
            time.sleep(0.02)
        return False

    try:
        assert pump(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                    file_path=_write(tmp_path, "a.jpg", b"a")),
                     lambda: (tmp_path / "incoming").exists() and any(
                         (tmp_path / "incoming").glob("tg-*/a.jpg")))
        # 意图 -> classify 降级 -> compose -> PLANNED 确认文案
        assert pump(InboundMessage(kind="text", chat_id=CHAT_ID, text="筛一下留2张"),
                     lambda: any("理解你想" in t for t in transport.texts()))
        # 确认 -> drive 到 Style 闸门
        assert pump(InboundMessage(kind="text", chat_id=CHAT_ID, text="好的"),
                     lambda: any("想要什么风格" in t for t in transport.texts()))
        # 给描述 -> StyleApplyAll 预览闸门
        assert pump(InboundMessage(kind="text", chat_id=CHAT_ID, text="复古暖色调"),
                     lambda: any("套用的效果" in t for t in transport.texts()))
        # 确认风格 -> Deliver 闸门
        assert pump(InboundMessage(kind="text", chat_id=CHAT_ID, text="好的"),
                     lambda: any("选好了 2 张" in t for t in transport.texts()))
        # 确认交付 -> 完成（Deliver stage 自己发文件）
        assert pump(InboundMessage(kind="text", chat_id=CHAT_ID, text="好的"),
                     lambda: any(kind == "file" for kind, _ in transport.log))
        assert _wait_until(lambda: consumer.store.list_active() == [])
    finally:
        stop_event.set()
        worker_thread.join(timeout=2)


def _write(tmp_path, name, content):
    d = tmp_path / "downloaded"
    d.mkdir(parents=True, exist_ok=True)
    p = d / name
    p.write_bytes(content)
    return str(p)
