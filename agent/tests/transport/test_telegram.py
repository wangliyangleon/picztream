from __future__ import annotations

import asyncio
import time
from pathlib import Path

from transport.telegram import TelegramTransport


class FakeChat:
    def __init__(self, chat_id: str) -> None:
        self.id = chat_id


class FakeMessage:
    def __init__(self, chat_id: str, text=None, photo=None) -> None:
        self.chat = FakeChat(chat_id)
        self.text = text
        self.photo = photo or []


class FakeUpdate:
    def __init__(self, update_id: int, message: FakeMessage) -> None:
        self.update_id = update_id
        self.message = message


class FakeBotClient:
    """No queued updates; used for send_text/stop tests."""

    def __init__(self) -> None:
        self.sent = []
        self.download_calls = []

    async def get_updates(self, offset=None, timeout=25):
        await asyncio.sleep(0.01)
        return []

    async def send_text(self, chat_id, text):
        await asyncio.sleep(0)
        self.sent.append((chat_id, text))

    async def send_photo_bytes(self, chat_id, path):
        await asyncio.sleep(0)

    async def send_document(self, chat_id, path):
        await asyncio.sleep(0)

    async def download_photo(self, update, dest_path):
        await asyncio.sleep(0)
        self.download_calls.append(dest_path)
        Path(dest_path).write_bytes(b"fake-photo-bytes")


class FakeBotClientWithUpdates(FakeBotClient):
    """Serves a canned batch of updates exactly once, then empties out."""

    def __init__(self, updates) -> None:
        super().__init__()
        self._updates = updates
        self._served = False

    async def get_updates(self, offset=None, timeout=25):
        await asyncio.sleep(0.01)
        if not self._served:
            self._served = True
            return self._updates
        return []


def test_send_text_blocks_until_fake_coroutine_completes(tmp_path):
    fake = FakeBotClient()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        transport.send_text("123", "hello")
        assert fake.sent == [("123", "hello")]
    finally:
        transport.stop()


def test_receive_returns_text_and_photo_messages_from_poll_loop(tmp_path):
    updates = [
        FakeUpdate(1, FakeMessage("123", text="hello")),
        FakeUpdate(2, FakeMessage("123", photo=[{"file_id": "abc", "file_size": 100}])),
    ]
    fake = FakeBotClientWithUpdates(updates)
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        deadline = time.monotonic() + 2.0
        messages = []
        while time.monotonic() < deadline:
            messages = transport.receive()
            if len(messages) >= 2:
                break
            time.sleep(0.05)
        assert len(messages) == 2
        assert messages[0].kind == "text"
        assert messages[0].chat_id == "123"
        assert messages[0].text == "hello"
        assert messages[1].kind == "photo"
        assert messages[1].chat_id == "123"
        assert Path(messages[1].file_path).parent == tmp_path
        assert fake.download_calls == [messages[1].file_path]
    finally:
        transport.stop()


def test_updates_from_non_matching_chat_id_are_dropped(tmp_path):
    updates = [FakeUpdate(1, FakeMessage("999", text="not for us"))]
    fake = FakeBotClientWithUpdates(updates)
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            time.sleep(0.05)
        assert transport.receive() == []
    finally:
        transport.stop()


def test_stop_joins_background_thread(tmp_path):
    fake = FakeBotClient()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()

    transport.stop()

    assert transport._thread.is_alive() is False
