"""开发/测试期跟 Telegram 等价的传输：一次性批处理，不常驻轮询（子增
量 D 范围，见本计划 Context 第 1 点）。receive() 把 in_dir 里现有的图
片列成一批 InboundMessage；send_* 把文件拷贝进 out_dir，模拟"发给用
户"。
"""
from __future__ import annotations

import shutil
from pathlib import Path
from typing import Iterator, Optional

from .base import InboundMessage

_IMAGE_EXTENSIONS = {".jpg", ".jpeg"}


class WatchFolderTransport:
    CHAT_ID = "watchfolder"

    def __init__(self, in_dir: Path, out_dir: Path) -> None:
        self.in_dir = Path(in_dir)
        self.out_dir = Path(out_dir)
        self.out_dir.mkdir(parents=True, exist_ok=True)

    def receive(self) -> Iterator[InboundMessage]:
        for p in sorted(self.in_dir.iterdir()):
            if p.is_file() and p.suffix.lower() in _IMAGE_EXTENSIONS:
                yield InboundMessage(kind="file", chat_id=self.CHAT_ID, file_path=str(p))

    def send_text(self, chat_id: str, text: str) -> None:
        with (self.out_dir / "messages.log").open("a") as f:
            f.write(text + "\n")

    def send_photo(self, chat_id: str, path: str, caption: Optional[str] = None) -> None:
        del caption  # 落盘无 caption 概念
        self.send_file(chat_id, path)

    def send_file(self, chat_id: str, path: str) -> None:
        shutil.copy2(path, self.out_dir / Path(path).name)
