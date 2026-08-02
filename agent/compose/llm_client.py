"""意图/调整解析用的纯文本 LLM 客户端，是 core/ai/ai.cpp 的 Python 镜像：
同一套 provider/环境变量/instruction 拼接/markdown 围栏剥离逻辑，唯一
差别是这里从不带图片，只发文本(compose_plan/parse_adjustment 处理的是
聊天文本，不是照片评估)。HttpPostFn 注入点镜像 core::ai::HttpPostFn，
真实网络请求绝不出现在 pytest 快速套件里，只在真机手动验证时才用真的
http_post。
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Callable, Dict, Optional, Tuple

_CLAUDE_MODEL = "claude-sonnet-4-5-20250929"
_GEMINI_MODEL = "gemini-3.1-flash-lite"
_CLAUDE_URL = "https://api.anthropic.com/v1/messages"
# 本地 Ollama——跟 core/ai/ai.h::LocalModelConfig 的默认值保持一致，但这
# 里是纯文本的意图/调整解析，不带图片，不需要 core::ai 那套 provider 无
# 关的 request_json 通用层，是它在 Python 侧的镜像(见文件顶部说明)。不
# 需要 API key(_get_api_key 只在 claude/gemini 分支调用)。
_OLLAMA_BASE_URL = "http://localhost:11434"
_OLLAMA_MODEL = "gemma4:e2b"


def ollama_base_url() -> str:
    """这次真正会连的 Ollama 地址,可经 PZT_AGENT_OLLAMA_BASE_URL 覆盖(跟模
    型名的 PZT_AGENT_OLLAMA_MODEL 同一个套路):Ollama 跑在另一台机器上、或
    改过端口时,不该只能改代码。

    真实调用与启动预检都从这里取。两边各读一份的话,会出现"预检说连不上、
    实际调用好好的"(或反过来)这种最难查的错。末尾斜杠在这里剥掉,免得调用
    方拼出 `//api/chat`。"""
    return os.environ.get("PZT_AGENT_OLLAMA_BASE_URL", _OLLAMA_BASE_URL).rstrip("/")


def effective_ollama_model() -> str:
    """这次真正会发给 Ollama 的模型名。模型名可经 PZT_AGENT_OLLAMA_MODEL 覆
    盖(AG-13),所以"有效模型名"不等于 _OLLAMA_MODEL 常量 - 预检只看常量的话,
    用户覆盖过模型名时会报一个他根本没在用的模型。request_json 与 preflight
    都从这里取。"""
    return os.environ.get("PZT_AGENT_OLLAMA_MODEL", _OLLAMA_MODEL)


class LlmRequestError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


HttpPostFn = Callable[[str, Dict[str, str], str], Tuple[int, str]]


def _gemini_url(api_key: str) -> str:
    return (
        "https://generativelanguage.googleapis.com/v1beta/models/"
        f"{_GEMINI_MODEL}:generateContent?key={api_key}"
    )


def _get_api_key(provider: str) -> str:
    env_name = "ANTHROPIC_API_KEY" if provider == "claude" else "GEMINI_API_KEY"
    value = os.environ.get(env_name)
    if not value:
        raise LlmRequestError("missing_api_key", f"{env_name} is not set")
    return value


def _build_instruction_text(user_prompt: str, schema_instruction: str) -> str:
    return (
        schema_instruction + "\n\n" + user_prompt +
        "\n\nRespond with ONLY a single JSON object matching the shape described above. "
        "Do not include any other text, explanation, or markdown formatting."
    )


def _strip_markdown_json_fence(text: str) -> str:
    stripped = text.strip()
    if not stripped.startswith("```"):
        return stripped
    newline = stripped.find("\n")
    if newline == -1:
        return stripped
    body = stripped[newline + 1:]
    closing = body.rfind("```")
    if closing != -1:
        body = body[:closing]
    return body.strip()


def _parse_inner_json(text: str) -> dict:
    try:
        return json.loads(_strip_markdown_json_fence(text))
    except json.JSONDecodeError as e:
        raise LlmRequestError("parse_error", str(e)) from e


# 原来写死 60 秒，跟 core 那边一模一样的数字、也栽在一模一样的地方（core
# 的依据见 d8c18a3：本地 gemma4:e2b 一次调用墙钟 45-80 秒，模型自报的计算
# 只有几秒，差额是模型加载、排队和吐正文之前那段 thinking，中位数正好骑在
# 60 上，于是同一句话时好时坏）。票 08 给 compose 的提示词加了 selection_
# brief 一段之后实测复现：短意图 40 秒过，带寒暄和去重的长意图稳定卡满 60
# 秒报 timed out，用户那一侧看到的是意图没被解析。取跟 core 同一个值。
_REQUEST_TIMEOUT_SECONDS = 180


def _real_http_post(url: str, headers: Dict[str, str], body: str) -> Tuple[int, str]:
    request = urllib.request.Request(url, data=body.encode("utf-8"), headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=_REQUEST_TIMEOUT_SECONDS) as response:
            return response.status, response.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")
    except urllib.error.URLError as e:
        raise LlmRequestError("network_error", str(e.reason)) from e


def _parse_claude_response(body: str) -> dict:
    try:
        outer = json.loads(body)
    except json.JSONDecodeError as e:
        raise LlmRequestError("parse_error", str(e)) from e
    try:
        text = outer["content"][0]["text"]
    except (KeyError, IndexError, TypeError) as e:
        raise LlmRequestError("parse_error", "unexpected claude response shape") from e
    return _parse_inner_json(text)


def _parse_gemini_response(body: str) -> dict:
    try:
        outer = json.loads(body)
    except json.JSONDecodeError as e:
        raise LlmRequestError("parse_error", str(e)) from e
    try:
        text = outer["candidates"][0]["content"]["parts"][0]["text"]
    except (KeyError, IndexError, TypeError) as e:
        raise LlmRequestError("parse_error", "unexpected gemini response shape") from e
    return _parse_inner_json(text)


def _parse_local_response(body: str) -> dict:
    try:
        outer = json.loads(body)
    except json.JSONDecodeError as e:
        raise LlmRequestError("parse_error", str(e)) from e
    try:
        text = outer["message"]["content"]
    except (KeyError, TypeError) as e:
        raise LlmRequestError("parse_error", "unexpected local response shape") from e
    return _parse_inner_json(text)


def request_json(user_prompt: str, schema_instruction: str, provider: str,
                  http_post: Optional[HttpPostFn] = None) -> dict:
    post = http_post or _real_http_post
    instruction_text = _build_instruction_text(user_prompt, schema_instruction)

    if provider == "claude":
        api_key = _get_api_key(provider)
        url = _CLAUDE_URL
        headers = {
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        }
        request_body = json.dumps({
            "model": _CLAUDE_MODEL,
            "max_tokens": 1024,
            "messages": [{"role": "user", "content": instruction_text}],
        })
    elif provider == "gemini":
        api_key = _get_api_key(provider)
        url = _gemini_url(api_key)
        headers = {"content-type": "application/json"}
        request_body = json.dumps({"contents": [{"parts": [{"text": instruction_text}]}]})
    elif provider == "local":
        url = f"{ollama_base_url()}/api/chat"
        headers = {"content-type": "application/json"}
        request_body = json.dumps({
            # 本地模型名可经 PZT_AGENT_OLLAMA_MODEL 覆盖，不必改代码（AG-13）。
            "model": effective_ollama_model(),
            "format": "json",
            "stream": False,
            "messages": [{"role": "user", "content": instruction_text}],
        })
    else:
        raise LlmRequestError("unknown_provider", f"unknown provider {provider!r}")

    status_code, response_body = post(url, headers, request_body)
    if status_code < 200 or status_code >= 300:
        raise LlmRequestError("http_error", f"status={status_code} body={response_body[:200]}")

    if provider == "claude":
        return _parse_claude_response(response_body)
    if provider == "gemini":
        return _parse_gemini_response(response_body)
    return _parse_local_response(response_body)
