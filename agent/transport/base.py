from __future__ import annotations

from dataclasses import dataclass
from typing import Iterator, Literal, Optional, Protocol


@dataclass
class InboundMessage:
    kind: Literal["text", "file", "photo"]
    chat_id: str
    text: Optional[str] = None
    file_path: Optional[str] = None


class Transport(Protocol):
    def receive(self) -> Iterator[InboundMessage]: ...
    def send_text(self, chat_id: str, text: str) -> None: ...
    def send_photo(self, chat_id: str, path: str) -> None: ...
    def send_file(self, chat_id: str, path: str) -> None: ...
