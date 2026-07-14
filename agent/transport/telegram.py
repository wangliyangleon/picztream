"""真机 Telegram 传输：把 telegram_client.py 的 async 接口桥接成
transport/base.py::Transport 要求的同步接口。后台线程自己起一个
asyncio event loop 常驻轮询 get_updates，主线程的 receive()/send_*
经 asyncio.run_coroutine_threadsafe 跟那个 loop 打交道，彼此靠
queue.Queue 传消息，谁都不用互相等对方的调度节奏。
"""
from __future__ import annotations

import asyncio
import queue
import threading
import uuid
from pathlib import Path
from typing import Any, Callable, List, Optional

from .base import InboundMessage
from .telegram_client import TelegramBotClient


class TelegramTransport:
    def __init__(self, token: str, chat_id: str, download_dir: Path,
                 bot_client_factory: Optional[Callable[[str], Any]] = None,
                 poll_timeout: int = 25) -> None:
        self.chat_id = chat_id
        self.download_dir = Path(download_dir)
        self.download_dir.mkdir(parents=True, exist_ok=True)
        self.poll_timeout = poll_timeout
        factory = bot_client_factory or TelegramBotClient
        self._bot_client = factory(token)
        self._inbound: "queue.Queue[InboundMessage]" = queue.Queue()
        self._offset: Optional[int] = None
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        self._loop = asyncio.new_event_loop()
        ready = threading.Event()

        def _run_loop() -> None:
            assert self._loop is not None
            asyncio.set_event_loop(self._loop)
            self._loop.call_soon(ready.set)
            self._loop.run_forever()

        self._thread = threading.Thread(target=_run_loop, daemon=True)
        self._thread.start()
        ready.wait(timeout=5)
        asyncio.run_coroutine_threadsafe(self._poll_loop(), self._loop)

    def stop(self) -> None:
        if self._loop is not None:
            self._loop.call_soon_threadsafe(self._loop.stop)
        if self._thread is not None:
            self._thread.join(timeout=2)

    async def _poll_loop(self) -> None:
        while True:
            try:
                updates = await self._bot_client.get_updates(offset=self._offset, timeout=self.poll_timeout)
            except Exception:
                await asyncio.sleep(0.1)
                continue
            for update in updates:
                self._offset = update.update_id + 1
                message = getattr(update, "message", None)
                if message is None:
                    continue
                if str(message.chat.id) != self.chat_id:
                    continue
                if getattr(message, "photo", None):
                    dest_path = self.download_dir / f"{uuid.uuid4().hex}.jpg"
                    await self._bot_client.download_photo(update, dest_path=str(dest_path))
                    self._inbound.put(InboundMessage(kind="photo", chat_id=self.chat_id, file_path=str(dest_path)))
                elif getattr(message, "text", None):
                    self._inbound.put(InboundMessage(kind="text", chat_id=self.chat_id, text=message.text))

    def receive(self) -> List[InboundMessage]:
        messages: List[InboundMessage] = []
        while True:
            try:
                messages.append(self._inbound.get_nowait())
            except queue.Empty:
                break
        return messages

    def send_text(self, chat_id: str, text: str) -> None:
        future = asyncio.run_coroutine_threadsafe(self._bot_client.send_text(chat_id, text), self._loop)
        future.result(timeout=30)

    def send_photo(self, chat_id: str, path: str) -> None:
        future = asyncio.run_coroutine_threadsafe(self._bot_client.send_photo_bytes(chat_id, path), self._loop)
        future.result(timeout=30)

    def send_file(self, chat_id: str, path: str) -> None:
        future = asyncio.run_coroutine_threadsafe(self._bot_client.send_document(chat_id, path), self._loop)
        future.result(timeout=30)
