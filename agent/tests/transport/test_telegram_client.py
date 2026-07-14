import pytest

from transport.telegram_client import TelegramConfigError, chat_id_from_env, token_from_env


def test_token_from_env_reads_value(monkeypatch):
    monkeypatch.setenv("TELEGRAM_BOT_TOKEN", "abc123")

    assert token_from_env() == "abc123"


def test_token_from_env_raises_when_unset(monkeypatch):
    monkeypatch.delenv("TELEGRAM_BOT_TOKEN", raising=False)

    with pytest.raises(TelegramConfigError) as exc_info:
        token_from_env()

    assert exc_info.value.code == "missing_token"


def test_token_from_env_raises_when_empty(monkeypatch):
    monkeypatch.setenv("TELEGRAM_BOT_TOKEN", "")

    with pytest.raises(TelegramConfigError) as exc_info:
        token_from_env()

    assert exc_info.value.code == "missing_token"


def test_chat_id_from_env_reads_value(monkeypatch):
    monkeypatch.setenv("TELEGRAM_CHAT_ID", "999")

    assert chat_id_from_env() == "999"


def test_chat_id_from_env_raises_when_unset(monkeypatch):
    monkeypatch.delenv("TELEGRAM_CHAT_ID", raising=False)

    with pytest.raises(TelegramConfigError) as exc_info:
        chat_id_from_env()

    assert exc_info.value.code == "missing_chat_id"


def test_chat_id_from_env_raises_when_empty(monkeypatch):
    monkeypatch.setenv("TELEGRAM_CHAT_ID", "")

    with pytest.raises(TelegramConfigError) as exc_info:
        chat_id_from_env()

    assert exc_info.value.code == "missing_chat_id"
