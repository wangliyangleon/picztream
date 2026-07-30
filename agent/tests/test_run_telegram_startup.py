"""run_telegram.py 启动期的凭证预检（T-10 (b)）。

只覆盖"凭证不齐时不抛 traceback、给一句指名环境变量的人话、非零退出"这
一段。main() 后面那半（transport/线程/常驻循环）碰真网络与真线程，不进
pytest 快速套件，由真机验证覆盖 - 凭证缺失是在构造 TelegramTransport 之
前就返回的，所以这里调 main() 不会走到那一段。
"""
import sys

import pytest

from run_telegram import config_error_hint, main
from transport.telegram_client import TelegramConfigError


def test_hint_names_the_missing_token_variable():
    hint = config_error_hint(TelegramConfigError("missing_token", "TELEGRAM_BOT_TOKEN is not set"))

    assert "TELEGRAM_BOT_TOKEN" in hint


def test_hint_names_the_missing_chat_id_variable_and_not_the_token():
    hint = config_error_hint(TelegramConfigError("missing_chat_id", "TELEGRAM_CHAT_ID is not set"))

    assert "TELEGRAM_CHAT_ID" in hint
    # 两条提示必须可区分：缺 chat id 时提 token 会把人指向错误的方向。
    assert "TELEGRAM_BOT_TOKEN" not in hint


def test_hint_falls_back_to_the_raw_message_for_an_unknown_code():
    # 防御未来新增的 code：宁可回落到原始英文 message，也不能静默给出空
    # 提示（那等于回到"失败不告知原因"这个本增量要修的缺陷本身）。
    hint = config_error_hint(TelegramConfigError("something_new", "went sideways"))

    assert "went sideways" in hint


def _run_main_expecting_exit(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(sys, "argv", ["run_telegram", "--state-dir", str(tmp_path)])
    with pytest.raises(SystemExit) as excinfo:
        main()
    return excinfo.value, capsys.readouterr()


def test_missing_token_exits_nonzero_without_traceback(monkeypatch, tmp_path, capsys):
    monkeypatch.delenv("TELEGRAM_BOT_TOKEN", raising=False)
    monkeypatch.delenv("TELEGRAM_CHAT_ID", raising=False)

    exc, captured = _run_main_expecting_exit(monkeypatch, tmp_path, capsys)

    assert exc.code != 0
    assert "TELEGRAM_BOT_TOKEN" in captured.err + captured.out


def test_missing_chat_id_alone_exits_nonzero_and_points_at_chat_id(monkeypatch, tmp_path, capsys):
    monkeypatch.setenv("TELEGRAM_BOT_TOKEN", "123:fake-token")
    monkeypatch.delenv("TELEGRAM_CHAT_ID", raising=False)

    exc, captured = _run_main_expecting_exit(monkeypatch, tmp_path, capsys)

    assert exc.code != 0
    assert "TELEGRAM_CHAT_ID" in captured.err + captured.out


def test_config_error_does_not_escape_as_telegram_config_error(monkeypatch, tmp_path):
    # 语义锁：main() 必须把类型化配置错误翻译掉，不能让它逃到调用方
    # （pzt-agent 的 console_script 入口直接调 main，逃出去就是 traceback）。
    monkeypatch.setattr(sys, "argv", ["run_telegram", "--state-dir", str(tmp_path)])
    monkeypatch.delenv("TELEGRAM_BOT_TOKEN", raising=False)

    with pytest.raises(SystemExit):
        main()
