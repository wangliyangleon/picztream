import json

import pytest

from compose.llm_client import LlmRequestError, request_json


def test_missing_api_key_raises_with_code(monkeypatch):
    monkeypatch.delenv("GEMINI_API_KEY", raising=False)

    with pytest.raises(LlmRequestError) as exc_info:
        request_json("do the thing", "schema", "gemini", http_post=lambda *a: (200, "{}"))

    assert exc_info.value.code == "missing_api_key"


def test_gemini_happy_path_parses_inner_json(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    captured = {}

    def fake_http_post(url, headers, body):
        captured["url"] = url
        captured["headers"] = headers
        captured["body"] = json.loads(body)
        response = {"candidates": [{"content": {"parts": [{"text": '{"count": 9}'}]}}]}
        return 200, json.dumps(response)

    result = request_json("留9张", "schema instruction", "gemini", http_post=fake_http_post)

    assert result == {"count": 9}
    assert "fake-key" in captured["url"]
    assert captured["body"]["contents"][0]["parts"][0]["text"].startswith("schema instruction")


def test_claude_happy_path_parses_inner_json(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "fake-key")

    def fake_http_post(url, headers, body):
        assert headers["x-api-key"] == "fake-key"
        response = {"content": [{"text": '{"count": 6}'}]}
        return 200, json.dumps(response)

    result = request_json("留6张", "schema instruction", "claude", http_post=fake_http_post)

    assert result == {"count": 6}


def test_strips_markdown_json_fence_before_parsing(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")

    def fake_http_post(url, headers, body):
        text = "```json\n{\"count\": 3}\n```"
        response = {"candidates": [{"content": {"parts": [{"text": text}]}}]}
        return 200, json.dumps(response)

    result = request_json("留3张", "schema", "gemini", http_post=fake_http_post)

    assert result == {"count": 3}


def test_non_2xx_status_raises_http_error(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")

    def fake_http_post(url, headers, body):
        return 500, "internal error"

    with pytest.raises(LlmRequestError) as exc_info:
        request_json("留9张", "schema", "gemini", http_post=fake_http_post)

    assert exc_info.value.code == "http_error"


def test_malformed_inner_json_raises_parse_error(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")

    def fake_http_post(url, headers, body):
        response = {"candidates": [{"content": {"parts": [{"text": "not json"}]}}]}
        return 200, json.dumps(response)

    with pytest.raises(LlmRequestError) as exc_info:
        request_json("留9张", "schema", "gemini", http_post=fake_http_post)

    assert exc_info.value.code == "parse_error"


def test_local_happy_path_does_not_require_an_api_key(monkeypatch):
    monkeypatch.delenv("GEMINI_API_KEY", raising=False)
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    captured = {}

    def fake_http_post(url, headers, body):
        captured["url"] = url
        captured["body"] = json.loads(body)
        response = {"message": {"role": "assistant", "content": '{"count": 9}'}}
        return 200, json.dumps(response)

    result = request_json("留9张", "schema instruction", "local", http_post=fake_http_post)

    assert result == {"count": 9}
    assert captured["url"].endswith("/api/chat")
    assert captured["body"]["format"] == "json"
    assert captured["body"]["messages"][0]["content"].startswith("schema instruction")


def test_local_malformed_response_shape_raises_parse_error():
    def fake_http_post(url, headers, body):
        return 200, json.dumps({"unexpected": "shape"})

    with pytest.raises(LlmRequestError) as exc_info:
        request_json("留9张", "schema", "local", http_post=fake_http_post)

    assert exc_info.value.code == "parse_error"


def test_injected_http_post_network_error_propagates(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")

    def fake_http_post(url, headers, body):
        raise LlmRequestError("network_error", "boom")

    with pytest.raises(LlmRequestError) as exc_info:
        request_json("留9张", "schema", "gemini", http_post=fake_http_post)

    assert exc_info.value.code == "network_error"
