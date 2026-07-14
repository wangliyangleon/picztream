"""Telegram Bot API 的薄封装：env 读取 + 4 个 F1 需要的操作，镜像
compose/llm_client.py 的"typed error + 可注入 callable，默认走真实实
现"套路。TelegramBotClient 本身仍是 async(不是同步桥，那是
transport/telegram.py::TelegramTransport 的活)，只是给 Task 2 一个小
的、可 mock 的 async 接口，不用到处直接调 telegram.Bot。TelegramBotClient
的正确性不进 pytest 快速套件(没有真 bot/没有对三方库内部做大量 mock
就测不出什么)，由手动真机验证覆盖，见 docs/M4_Eng_Design.md 第五节。
"""
from __future__ import annotations

import os
from typing import Any, List, Optional

import telegram


class TelegramConfigError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def token_from_env() -> str:
    value = os.environ.get("TELEGRAM_BOT_TOKEN")
    if not value:
        raise TelegramConfigError("missing_token", "TELEGRAM_BOT_TOKEN is not set")
    return value


def chat_id_from_env() -> str:
    value = os.environ.get("TELEGRAM_CHAT_ID")
    if not value:
        raise TelegramConfigError("missing_chat_id", "TELEGRAM_CHAT_ID is not set")
    return value


class TelegramBotClient:
    def __init__(self, token: str) -> None:
        self._bot = telegram.Bot(token=token)

    async def get_updates(self, offset: Optional[int] = None, timeout: int = 25) -> List[Any]:
        return await self._bot.get_updates(offset=offset, timeout=timeout)

    async def send_text(self, chat_id: str, text: str) -> None:
        await self._bot.send_message(chat_id=chat_id, text=text)

    async def send_photo_bytes(self, chat_id: str, path: str) -> None:
        with open(path, "rb") as f:
            await self._bot.send_photo(chat_id=chat_id, photo=f)

    async def send_document(self, chat_id: str, path: str) -> None:
        with open(path, "rb") as f:
            await self._bot.send_document(chat_id=chat_id, document=f)

    async def download_photo(self, update: Any, dest_path: str) -> None:
        photo_sizes = update.message.photo
        largest = max(photo_sizes, key=lambda p: p.file_size or 0)
        file = await self._bot.get_file(largest.file_id)
        await file.download_to_drive(custom_path=dest_path)
