from __future__ import annotations

import asyncio
import time
from pathlib import Path

from transport.telegram import TelegramTransport, _next_backoff


class FakeChat:
    def __init__(self, chat_id: str) -> None:
        self.id = chat_id


class FakeDocument:
    def __init__(self, file_id: str, file_name: str, mime_type: str) -> None:
        self.file_id = file_id
        self.file_name = file_name
        self.mime_type = mime_type


class FakeMessage:
    def __init__(self, chat_id: str, text=None, photo=None, document=None, caption=None) -> None:
        self.chat = FakeChat(chat_id)
        self.text = text
        self.photo = photo or []
        self.document = document
        self.caption = caption


class FakeUpdate:
    def __init__(self, update_id: int, message: FakeMessage) -> None:
        self.update_id = update_id
        self.message = message


class FakeBotClient:
    """No queued updates; used for send_text/stop tests."""

    def __init__(self) -> None:
        self.sent = []
        self.download_calls = []
        self.sent_photos = []  # (path, caption)
        self.registered_commands = None
        self.edits = []  # (message_id, text)

    async def set_my_commands(self, commands):
        await asyncio.sleep(0)
        self.registered_commands = list(commands)

    async def get_updates(self, offset=None, timeout=25):
        await asyncio.sleep(0.01)
        return []

    async def send_text(self, chat_id, text):
        await asyncio.sleep(0)
        self.sent.append((chat_id, text))
        return "42"

    async def edit_message_text(self, chat_id, message_id, text):
        await asyncio.sleep(0)
        self.edits.append((message_id, text))

    async def send_photo_bytes(self, chat_id, path, caption=None):
        await asyncio.sleep(0)
        self.sent_photos.append((path, caption))

    async def send_document(self, chat_id, path):
        await asyncio.sleep(0)

    async def download_photo(self, update, dest_path):
        await asyncio.sleep(0)
        self.download_calls.append(dest_path)
        Path(dest_path).write_bytes(b"fake-photo-bytes")

    async def download_document(self, update, dest_path):
        await asyncio.sleep(0)
        self.download_calls.append(dest_path)
        Path(dest_path).write_bytes(b"fake-document-bytes")


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


def test_image_sent_as_document_is_downloaded_as_a_file_message(tmp_path):
    # Telegram 里"以文件方式发送"(不压缩)的图片走 document 字段，不
    # 是 photo；真机验证时发现的真实 bug：手机相册选"发送文件"发出来
    # 的图，之前完全没被识别，静默丢弃。
    doc = FakeDocument(file_id="doc1", file_name="IMG_0001.jpg", mime_type="image/jpeg")
    updates = [FakeUpdate(1, FakeMessage("123", document=doc))]
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
            if messages:
                break
            time.sleep(0.05)
        assert len(messages) == 1
        assert messages[0].kind == "file"
        assert messages[0].chat_id == "123"
        assert Path(messages[0].file_path).parent == tmp_path
        assert Path(messages[0].file_path).suffix == ".jpg"
        assert fake.download_calls == [messages[0].file_path]
    finally:
        transport.stop()


def _drain(transport, want, timeout=2.0):
    deadline = time.monotonic() + timeout
    messages = []
    while time.monotonic() < deadline:
        messages += transport.receive()
        if len(messages) >= want:
            break
        time.sleep(0.05)
    return messages


def test_photo_with_caption_also_emits_a_text_message(tmp_path):
    # AG-09：发图时把意图写在 caption 里 -> 图之后追加一条 text 消息，复用文本管线。
    updates = [FakeUpdate(1, FakeMessage("123", photo=[{"file_id": "abc", "file_size": 100}],
                                          caption="帮我选3张发朋友圈"))]
    fake = FakeBotClientWithUpdates(updates)
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        messages = _drain(transport, 2)
        assert len(messages) == 2
        assert messages[0].kind == "photo"
        assert messages[1].kind == "text"
        assert messages[1].text == "帮我选3张发朋友圈"
        assert messages[1].chat_id == "123"
    finally:
        transport.stop()


def test_document_image_with_caption_also_emits_a_text_message(tmp_path):
    doc = FakeDocument(file_id="doc1", file_name="IMG_0001.jpg", mime_type="image/jpeg")
    updates = [FakeUpdate(1, FakeMessage("123", document=doc, caption="留5张就行"))]
    fake = FakeBotClientWithUpdates(updates)
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        messages = _drain(transport, 2)
        assert len(messages) == 2
        assert messages[0].kind == "file"
        assert messages[1].kind == "text"
        assert messages[1].text == "留5张就行"
    finally:
        transport.stop()


def test_send_text_returns_id_and_edit_text_passes_through(tmp_path):
    # AG-16.3：send_text 返回 message_id，edit_text 透传到 bot.edit_message_text。
    fake = FakeBotClient()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        mid = transport.send_text("123", "进度 1")
        assert mid == "42"
        transport.edit_text("123", mid, "进度 2")
        assert fake.edits == [("42", "进度 2")]
    finally:
        transport.stop()


def test_register_commands_calls_set_my_commands(tmp_path):
    # AG-16.2：register_commands 透传到 bot.set_my_commands。
    fake = FakeBotClient()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        transport.register_commands([("status", "看进度"), ("help", "帮助")])
        assert fake.registered_commands == [("status", "看进度"), ("help", "帮助")]
    finally:
        transport.stop()


def test_send_photo_passes_caption_through(tmp_path):
    # AG-15：send_photo 的 caption 透传到 bot 层。
    fake = FakeBotClient()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        transport.send_photo("123", "/tmp/x.jpg", caption="第 2 张")
        assert fake.sent_photos == [("/tmp/x.jpg", "第 2 张")]
    finally:
        transport.stop()


def test_non_image_document_is_ignored(tmp_path):
    doc = FakeDocument(file_id="doc2", file_name="notes.pdf", mime_type="application/pdf")
    updates = [FakeUpdate(1, FakeMessage("123", document=doc))]
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
        assert fake.download_calls == []
    finally:
        transport.stop()


class FakeBotClientPhotoDownloadFailsOnce(FakeBotClient):
    """First batch: a photo whose download blows up. Second batch: a text
    message. Reproduces the real-world bug where one bad update (e.g. a
    flaky download) must not permanently kill the whole poll loop.
    """

    def __init__(self) -> None:
        super().__init__()
        self._call_count = 0

    async def get_updates(self, offset=None, timeout=25):
        await asyncio.sleep(0.01)
        self._call_count += 1
        if self._call_count == 1:
            return [FakeUpdate(1, FakeMessage("123", photo=[{"file_id": "abc", "file_size": 100}]))]
        if self._call_count == 2:
            return [FakeUpdate(2, FakeMessage("123", text="still alive"))]
        return []

    async def download_photo(self, update, dest_path):
        raise RuntimeError("network blew up mid-download")


def test_poll_loop_survives_a_single_update_processing_error(tmp_path):
    fake = FakeBotClientPhotoDownloadFailsOnce()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        deadline = time.monotonic() + 2.0
        messages: list = []
        while time.monotonic() < deadline:
            messages = transport.receive()
            if messages:
                break
            time.sleep(0.05)
        assert len(messages) == 1
        assert messages[0].kind == "text"
        assert messages[0].text == "still alive"
    finally:
        transport.stop()


def test_poll_loop_notifies_the_chat_when_a_download_fails(tmp_path):
    # 真机验证时发现的真实 bug：下载失败之前只打印在服务端终端日志里，
    # 用户在 Telegram 那头完全不知道有张照片"丢"了(10 张发过去，agent
    # 只认到 7 张，用户毫无察觉是哪 3 张、为什么)。下载失败必须回一句
    # 话给用户，不能只默默跳过。
    fake = FakeBotClientPhotoDownloadFailsOnce()
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if any("失败" in text for _, text in fake.sent):
                break
            time.sleep(0.05)
        assert any(chat_id == "123" and "失败" in text for chat_id, text in fake.sent)
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


class FakeCallbackQuery:
    def __init__(self, query_id, chat_id, data) -> None:
        self.id = query_id
        self.message = FakeMessage(chat_id)
        self.data = data


class FakeCallbackUpdate:
    def __init__(self, update_id, callback_query) -> None:
        self.update_id = update_id
        self.message = None
        self.callback_query = callback_query


class FakeBotClientWithCallbacks(FakeBotClientWithUpdates):
    def __init__(self, updates) -> None:
        super().__init__(updates)
        self.answered = []

    async def answer_callback_query(self, callback_query_id):
        await asyncio.sleep(0)
        self.answered.append(callback_query_id)


def test_callback_query_becomes_callback_inbound_and_is_answered(tmp_path):
    updates = [FakeCallbackUpdate(1, FakeCallbackQuery("q1", "123", "approve:tg-abc"))]
    fake = FakeBotClientWithCallbacks(updates)
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
            if messages:
                break
            time.sleep(0.05)
        assert len(messages) == 1
        assert messages[0].kind == "callback"
        assert messages[0].chat_id == "123"
        assert messages[0].data == "approve:tg-abc"
        assert fake.answered == ["q1"]  # loading 转圈被消掉
    finally:
        transport.stop()


def test_callback_query_from_other_chat_is_dropped_but_still_answered(tmp_path):
    updates = [FakeCallbackUpdate(1, FakeCallbackQuery("q9", "999", "approve:tg-x"))]
    fake = FakeBotClientWithCallbacks(updates)
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            if fake.answered:
                break
            time.sleep(0.05)
        assert transport.receive() == []      # 非白名单 chat 不产生入站
        assert fake.answered == ["q9"]         # 但仍要应答，别让对方一直转圈
    finally:
        transport.stop()


def test_next_backoff_doubles_and_caps_at_30():
    # AG-17：get_updates 失败退避 0.1->0.2->0.4...，封顶 30。
    b = 0.1
    seen = []
    for _ in range(12):
        seen.append(b)
        b = _next_backoff(b)
    assert seen[0] == 0.1
    assert abs(seen[1] - 0.2) < 1e-9
    assert abs(seen[2] - 0.4) < 1e-9
    assert seen[-1] == 30.0  # 已封顶
    assert _next_backoff(30.0) == 30.0


class FakeBotClientGetUpdatesFailsOnce(FakeBotClient):
    """首次 get_updates 抛异常，之后正常服务一批 update 一次。"""

    def __init__(self, updates):
        super().__init__()
        self._updates = updates
        self._failed = False
        self._served = False

    async def get_updates(self, offset=None, timeout=25):
        await asyncio.sleep(0.01)
        if not self._failed:
            self._failed = True
            raise RuntimeError("network down")
        if not self._served:
            self._served = True
            return self._updates
        return []


def test_poll_loop_recovers_after_get_updates_failure(tmp_path):
    # AG-17：get_updates 故障不拖死轮询，退避后恢复，update 最终到达。
    updates = [FakeUpdate(1, FakeMessage("123", text="hello"))]
    fake = FakeBotClientGetUpdatesFailsOnce(updates)
    transport = TelegramTransport(
        token="t", chat_id="123", download_dir=tmp_path,
        bot_client_factory=lambda token: fake,
    )
    transport.start()
    try:
        messages = _drain(transport, 1)
        assert len(messages) == 1
        assert messages[0].text == "hello"
    finally:
        transport.stop()
