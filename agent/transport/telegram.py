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
                try:
                    await self._handle_update(update)
                except Exception as e:
                    # 单条更新处理失败(典型是下载图片时网络抖了一下，或
                    # 者文件超过 Telegram 的下载体积上限)不该拖死整条常
                    # 驻轮询协程：offset 已经在上面推进过了，这条更新算
                    # 是"丢弃"，但轮询本身必须活下去。光在服务端终端打
                    # 印不够——真机验证时发现用户在 Telegram 那头完全不
                    # 知道有张照片"丢"了，必须回一句话过去。
                    print(f"[TelegramTransport] 处理消息失败，已跳过：{e!r}")
                    try:
                        await self._bot_client.send_text(
                            self.chat_id, f"收一条消息失败了(可能文件太大)，这条就跳过了：{e}"
                        )
                    except Exception:
                        pass  # 连报错消息都发不出去就算了，不要再往外抛

    async def _handle_update(self, update: Any) -> None:
        callback = getattr(update, "callback_query", None)
        if callback is not None:
            await self._handle_callback_query(callback)
            return
        message = getattr(update, "message", None)
        if message is None:
            return
        if str(message.chat.id) != self.chat_id:
            return
        document = getattr(message, "document", None)
        if getattr(message, "photo", None):
            dest_path = self.download_dir / f"{uuid.uuid4().hex}.jpg"
            await self._bot_client.download_photo(update, dest_path=str(dest_path))
            self._inbound.put(InboundMessage(kind="photo", chat_id=self.chat_id, file_path=str(dest_path)))
            self._enqueue_caption(message)
        elif document is not None and (document.mime_type or "").startswith("image/"):
            # 手机相册"以文件方式发送"(不压缩)的图片走 document，不是
            # photo，真机验证时发现之前完全没处理这种情况，见
            # transport/telegram_client.py::download_document。
            suffix = Path(document.file_name).suffix if document.file_name else ".jpg"
            dest_path = self.download_dir / f"{uuid.uuid4().hex}{suffix}"
            await self._bot_client.download_document(update, dest_path=str(dest_path))
            self._inbound.put(InboundMessage(kind="file", chat_id=self.chat_id, file_path=str(dest_path)))
            self._enqueue_caption(message)
        elif getattr(message, "text", None):
            self._inbound.put(InboundMessage(kind="text", chat_id=self.chat_id, text=message.text))
        else:
            # 诊断用：消息进来了但既没命中 photo 也没命中 text，先打印
            # 出来看它长什么样，不要悄无声息地把它吃掉。
            print(f"[TelegramTransport] 收到不认识的消息形状，已跳过：photo={getattr(message, 'photo', 'N/A')!r} "
                  f"text={getattr(message, 'text', 'N/A')!r} caption={getattr(message, 'caption', 'N/A')!r} "
                  f"document={getattr(message, 'document', 'N/A')!r}")

    def _enqueue_caption(self, message: Any) -> None:
        # 发图带的文字说明（caption）当一条普通文本入站消息接着投进队列，
        # 复用现有文本管线当意图处理（AG-09）。排在照片/文件消息之后：消费
        # 端先 mint/加照片、再处理意图。相册只有第一张带 caption，天然只触
        # 发一次。
        caption = (getattr(message, "caption", None) or "").strip()
        if caption:
            self._inbound.put(InboundMessage(kind="text", chat_id=self.chat_id, text=caption))

    async def _handle_callback_query(self, callback: Any) -> None:
        # inline 按钮点击：先应答消掉客户端 loading 转圈，再把 callback_data
        # 当作一条 kind="callback" 的入站消息投进队列，交给上层
        # SessionConsumer 解析（形如 "approve:tg-xxxxxxxx"）。
        message = getattr(callback, "message", None)
        chat_id = str(message.chat.id) if message is not None else None
        try:
            await self._bot_client.answer_callback_query(callback.id)
        except Exception as e:  # noqa: BLE001 应答失败只是转圈没消掉，不该拖垮轮询
            print(f"[TelegramTransport] answer_callback_query 失败：{e!r}")
        if chat_id != self.chat_id:
            return
        self._inbound.put(InboundMessage(kind="callback", chat_id=self.chat_id,
                                          data=getattr(callback, "data", None)))

    def receive(self) -> List[InboundMessage]:
        messages: List[InboundMessage] = []
        while True:
            try:
                messages.append(self._inbound.get_nowait())
            except queue.Empty:
                break
        return messages

    def register_commands(self, commands: List[Any]) -> None:
        # 注册 bot 命令菜单（AG-16.2）。best-effort：注册失败（网络等）不该拦
        # 启动，命令能不能用不依赖它（拦截在 consumer 侧）。
        try:
            future = asyncio.run_coroutine_threadsafe(
                self._bot_client.set_my_commands(commands), self._loop)
            future.result(timeout=30)
        except Exception as e:  # noqa: BLE001
            print(f"[TelegramTransport] set_my_commands 失败（忽略）：{e!r}")

    def send_text(self, chat_id: str, text: str) -> Optional[str]:
        future = asyncio.run_coroutine_threadsafe(self._bot_client.send_text(chat_id, text), self._loop)
        return future.result(timeout=30)

    def edit_text(self, chat_id: str, message_id: str, text: str) -> None:
        # 原地更新一条已发消息（进度播报，AG-16.3）。调用方对失败降级发新。
        future = asyncio.run_coroutine_threadsafe(
            self._bot_client.edit_message_text(chat_id, message_id, text), self._loop)
        future.result(timeout=30)

    def send_buttons(self, chat_id: str, text: str, options: List[Any]) -> None:
        # options: [(label, callback_data), ...]。SessionConsumer 用 getattr
        # 探测这个方法是否存在（见 consumer._send_buttons），所以它只在真机
        # TelegramTransport 上有，其它 transport 不实现、自动降级成纯文本。
        future = asyncio.run_coroutine_threadsafe(
            self._bot_client.send_text_with_buttons(chat_id, text, options), self._loop)
        future.result(timeout=30)

    def send_photo(self, chat_id: str, path: str, caption: Optional[str] = None) -> None:
        future = asyncio.run_coroutine_threadsafe(
            self._bot_client.send_photo_bytes(chat_id, path, caption), self._loop)
        future.result(timeout=30)

    def send_file(self, chat_id: str, path: str) -> None:
        future = asyncio.run_coroutine_threadsafe(self._bot_client.send_document(chat_id, path), self._loop)
        future.result(timeout=30)
