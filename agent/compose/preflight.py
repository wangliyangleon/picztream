"""启动期的环境预检（T-10 (c) / (c-2)）。

在 agent 真正开始干活之前，先看一眼它依赖的东西在不在：本地 Ollama 起没
起、要用的模型 pull 没 pull、云端 provider 的 API key 配没配。以前这些全
是"用到了才连"，用户要把照片传完、意图打完，才在编排失败时收到一句"AI 服
务好像连不上"，而且不告诉他该去做什么。

**只告警，不拒绝启动**（PRD 决策 2）：常驻会话不该因为一个稍后可能被起来
的服务而拒绝拉起，用户完全可能先起 agent 再起 Ollama。所以这里的函数一律
返回"发现了什么"，由调用方决定怎么呈现，自己不打日志、不退出。

放在 compose/ 而不是新起一个顶层模块，有两个原因：pyproject 的 py-modules
是显式白名单，新增顶层模块必须同时改那一行才会进 wheel，漏了就是"本地跑
得通、brew install 之后 ImportError"；而 Ollama 地址、模型名、API key 的
环境变量名本来就是 llm_client 的东西，放旁边能直接复用，不用把它们再抄一
份。判定与展示分开的写法跟 run_telegram.config_error_hint 一致。
"""
from __future__ import annotations

import json
import os
import urllib.error
import urllib.request
from typing import Callable, Optional, Tuple

from compose.llm_client import LlmRequestError

# url -> (status, body)。POST 那条 HttpPostFn 签名带 headers 和 body，
# /api/tags 是 GET，用不上，另立一个而不是硬塞。
HttpGetFn = Callable[[str], Tuple[int, str]]

# 预检的超时必须短：这是启动路径上的探活，不是业务调用。llm_client 那边
# 60 秒是给真实推理留的，照抄过来会把"Ollama 没起"变成"agent 卡半分钟才
# 启动"。
_PREFLIGHT_TIMEOUT_SECONDS = 2

_API_KEY_ENV = {"claude": "ANTHROPIC_API_KEY", "gemini": "GEMINI_API_KEY"}


def _real_http_get(url: str) -> Tuple[int, str]:
    request = urllib.request.Request(url, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=_PREFLIGHT_TIMEOUT_SECONDS) as response:
            return response.status, response.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8")
    except urllib.error.URLError as e:
        # 跟 llm_client._real_http_post 同一个约定：连接层失败抛 typed error，
        # 不返回状态码。
        raise LlmRequestError("network_error", str(e.reason)) from e


def _model_is_present(body: str, model: str) -> Optional[bool]:
    """模型在不在 ollama 的清单里。返回 None 表示"看不出来"（响应格式跟我们
    的假设对不上），调用方据此保持沉默。"""
    try:
        payload = json.loads(body)
        models = payload["models"]
        names = {m["name"] for m in models}
    except (json.JSONDecodeError, KeyError, TypeError):
        return None
    if not names:
        return None
    # 用户配 "gemma4"、ollama 列 "gemma4:latest" 是同一个模型，不该报没 pull。
    return model in names or f"{model}:latest" in names


def check_ollama(base_url: str, model: str,
                  http_get: Optional[HttpGetFn] = None) -> Optional[Tuple[str, str]]:
    """一次 GET /api/tags 就能把两种失败分开：连不上服务，和服务在、模型没
    pull。返回 None 表示没发现问题，否则返回 (code, 给人看的一句话)。

    不用 POST /api/chat 探活：那会真的把模型加载起来，秒级甚至十几秒，而我
    们只需要知道服务在不在、模型有没有。"""
    get = http_get or _real_http_get
    url = f"{base_url.rstrip('/')}/api/tags"

    try:
        status, body = get(url)
    except LlmRequestError as e:
        return ("ollama_unreachable",
                f"连不上本地 AI 服务（{base_url}，{e.message}）。"
                f"跑一下 `ollama serve` 再试；没起也能继续用，只是需要 AI 的那几步会失败。")

    if status < 200 or status >= 300:
        # 服务应答了但不好使。可做的动作跟连不上是同一个，归到一类。
        return ("ollama_unreachable",
                f"本地 AI 服务返回了 {status}（{base_url}）。"
                f"确认一下 `ollama serve` 的状态。")

    present = _model_is_present(body, model)
    if present is None:
        # 拿到 200 说明服务活着。清单解析不出来是我们对 ollama 响应格式的假
        # 设过期，是 PZT 自己的问题，不该报成用户的环境错。
        return None
    if not present:
        return ("ollama_model_missing",
                f"本地 AI 服务在跑，但模型 {model} 还没下载。"
                f"跑一下 `ollama pull {model}`。")
    return None


def check_meta_provider_key(provider: str) -> Optional[Tuple[str, str]]:
    """云端 provider 的 API key 在不在。provider 在启动时就定了（
    PZT_AGENT_META_PROVIDER），却要等到第一次真实调用才发现 key 没配，属于
    跟 Ollama 一样的延迟失败，一并提前。"""
    env_name = _API_KEY_ENV.get(provider)
    if env_name is None:
        return None  # local：不需要 key
    if os.environ.get(env_name):
        return None
    return ("missing_api_key",
            f"语言推理用的是 {provider}，但环境变量 {env_name} 没设，"
            f"到了要解析意图那一步会失败。")
