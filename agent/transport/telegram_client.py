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
from typing import Any, List, Optional, Tuple

import telegram
from telegram import InlineKeyboardButton, InlineKeyboardMarkup
from telegram.request import HTTPXRequest


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
        # python-telegram-bot 的默认 HTTPXRequest 读/写/连接超时都只有
        # 5 秒：真机验证时复现过，发照片/文件在正常网络下经常超过 5 秒
        # 就被 httpx 自己掐断(telegram.error.TimedOut)，跟 Telegram 服
        # 务端是否响应无关。get_updates 走长轮询，读超时必须明显大于
        # 传给它的 timeout 参数，否则 httpx 会比 Telegram 服务端更早
        # 放弃这次长轮询。两类请求超时诉求不同，用 ptb 原生支持的
        # request/get_updates_request 分开配置。
        request = HTTPXRequest(connection_pool_size=8, read_timeout=30, write_timeout=30,
                                connect_timeout=15, pool_timeout=15)
        get_updates_request = HTTPXRequest(connection_pool_size=1, read_timeout=40, write_timeout=15,
                                            connect_timeout=15, pool_timeout=15)
        self._bot = telegram.Bot(token=token, request=request, get_updates_request=get_updates_request)

    async def get_updates(self, offset: Optional[int] = None, timeout: int = 25) -> List[Any]:
        return await self._bot.get_updates(offset=offset, timeout=timeout)

    async def send_text(self, chat_id: str, text: str) -> None:
        await self._bot.send_message(chat_id=chat_id, text=text)

    async def send_text_with_buttons(self, chat_id: str, text: str,
                                      buttons: List[Tuple[str, str]]) -> None:
        # buttons: [(label, callback_data), ...]，单行排布（一个闸门只有
        # 2-3 个选项，不需要多行）。callback_data 上限 64 字节，调用方保证
        # （"approve:tg-xxxxxxxx" 这类远低于上限）。
        keyboard = InlineKeyboardMarkup([[InlineKeyboardButton(label, callback_data=data)
                                          for label, data in buttons]])
        await self._bot.send_message(chat_id=chat_id, text=text, reply_markup=keyboard)

    async def answer_callback_query(self, callback_query_id: str) -> None:
        # 必须应答，否则用户端按钮一直转圈。不带文案，只是消掉 loading。
        await self._bot.answer_callback_query(callback_query_id)

    async def set_my_commands(self, commands: List[Tuple[str, str]]) -> None:
        # 让 Telegram 输入框弹出命令菜单（AG-16.2）。commands: [(name, desc), ...]。
        await self._bot.set_my_commands([telegram.BotCommand(name, desc) for name, desc in commands])

    async def send_photo_bytes(self, chat_id: str, path: str, caption: Optional[str] = None) -> None:
        with open(path, "rb") as f:
            await self._bot.send_photo(chat_id=chat_id, photo=f, caption=caption)

    async def send_document(self, chat_id: str, path: str) -> None:
        with open(path, "rb") as f:
            await self._bot.send_document(chat_id=chat_id, document=f)

    async def download_photo(self, update: Any, dest_path: str) -> None:
        photo_sizes = update.message.photo
        largest = max(photo_sizes, key=lambda p: p.file_size or 0)
        file = await self._bot.get_file(largest.file_id)
        await file.download_to_drive(custom_path=dest_path)

    async def download_document(self, update: Any, dest_path: str) -> None:
        # 手机相册"以文件方式发送"(不压缩)走的是 document，不是
        # photo：两者是 Telegram 里完全不同的消息字段，document 只有
        # 一份原图，不用像 photo 那样在多个尺寸里挑最大的。
        document = update.message.document
        file = await self._bot.get_file(document.file_id)
        await file.download_to_drive(custom_path=dest_path)
