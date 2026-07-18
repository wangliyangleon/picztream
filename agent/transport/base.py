from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator, Literal, Optional, Protocol


@dataclass
class InboundMessage:
    kind: Literal["text", "file", "photo", "callback"]
    chat_id: str
    text: Optional[str] = None
    file_path: Optional[str] = None
    # kind=="callback"（inline 按钮点击）时携带按钮的 callback_data，形如
    # "approve:tg-xxxxxxxx"（动作:run_id，见 session/consumer.py）。
    data: Optional[str] = None


class Transport(Protocol):
    def receive(self) -> Iterator[InboundMessage]: ...
    # send_text 返回消息标识（Telegram 的 message_id，供 edit_text 原地更新
    # 用），不支持的 transport 返回 None（见 AG-16.3）。
    def send_text(self, chat_id: str, text: str) -> Optional[str]: ...
    def send_photo(self, chat_id: str, path: str, caption: Optional[str] = None) -> None: ...
    def send_file(self, chat_id: str, path: str) -> None: ...
    # send_buttons / edit_text 是可选能力：只有真机 TelegramTransport 实现，
    # WatchFolderTransport 等不实现。调用方（SessionConsumer）用 getattr
    # 探测，缺省时降级（纯文本 / 发新消息），因此不写进 Protocol 的必需方法集。
    # def send_buttons(self, chat_id, text, options: list[tuple[str, str]]) -> None: ...
    # def edit_text(self, chat_id, message_id: str, text: str) -> None: ...
