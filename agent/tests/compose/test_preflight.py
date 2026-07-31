"""启动期的环境预检（T-10 (c) / (c-2)）。

判定与展示分开：这里断言 code，hint 只断言"有没有把该说的关键词说出来"，
不锁死措辞。真实网络请求跟 llm_client 一样绝不进 pytest 快速套件，全部走
注入的 http_get。
"""
import json

import pytest

from compose.llm_client import LlmRequestError, effective_ollama_model, ollama_base_url
from compose.preflight import check_meta_provider_key, check_ollama

BASE = "http://localhost:11434"


def _tags_body(*names):
    return json.dumps({"models": [{"name": n} for n in names]})


def _get_returning(status, body):
    def fake_get(url):
        assert url.startswith(BASE)
        return status, body
    return fake_get


def _get_raising_network_error(url):
    # _real_http_get 遇 URLError 抛的就是这个,跟 llm_client 的既有约定一致。
    raise LlmRequestError("network_error", "Connection refused")


def test_service_reachable_and_model_present_is_silent():
    assert check_ollama(BASE, "gemma4:e2b",
                         http_get=_get_returning(200, _tags_body("gemma4:e2b", "llama3"))) is None


def test_unreachable_service_points_at_ollama_serve():
    result = check_ollama(BASE, "gemma4:e2b", http_get=_get_raising_network_error)

    assert result is not None
    code, hint = result
    assert code == "ollama_unreachable"
    assert "ollama serve" in hint
    # 带上实际 URL,不让用户猜我们在连哪。
    assert BASE in hint


def test_model_not_pulled_points_at_ollama_pull_with_the_real_model_name():
    result = check_ollama(BASE, "gemma4:e2b", http_get=_get_returning(200, _tags_body("llama3")))

    assert result is not None
    code, hint = result
    assert code == "ollama_model_missing"
    assert "ollama pull" in hint
    assert "gemma4:e2b" in hint
    # 这条跟"服务连不上"必须可区分,否则用户会去重启一个本来就在跑的服务。
    assert "ollama serve" not in hint


def test_model_matches_when_registry_reports_the_implicit_latest_tag():
    # 用户配 "gemma4",ollama 列出来的是 "gemma4:latest",这是同一个模型,
    # 不该报"没 pull"。
    assert check_ollama(BASE, "gemma4",
                         http_get=_get_returning(200, _tags_body("gemma4:latest"))) is None


def test_unparseable_body_does_not_warn():
    # 拿到 200 说明服务是活的。模型清单解析不出来是我们对 ollama 响应格式的
    # 假设过期,不是用户的环境问题 - 为此告警等于把自己的 bug 报成用户的错。
    assert check_ollama(BASE, "gemma4:e2b", http_get=_get_returning(200, "not json at all")) is None
    assert check_ollama(BASE, "gemma4:e2b", http_get=_get_returning(200, "{}")) is None


def test_non_2xx_is_reported_as_unreachable():
    result = check_ollama(BASE, "gemma4:e2b", http_get=_get_returning(500, "boom"))

    assert result is not None
    assert result[0] == "ollama_unreachable"


@pytest.mark.parametrize("provider", ["claude", "gemini"])
def test_missing_cloud_key_is_reported_with_the_variable_name(monkeypatch, provider):
    env_name = "ANTHROPIC_API_KEY" if provider == "claude" else "GEMINI_API_KEY"
    monkeypatch.delenv(env_name, raising=False)

    result = check_meta_provider_key(provider)

    assert result is not None
    code, hint = result
    assert code == "missing_api_key"
    assert env_name in hint


@pytest.mark.parametrize("provider", ["claude", "gemini"])
def test_present_cloud_key_is_silent(monkeypatch, provider):
    env_name = "ANTHROPIC_API_KEY" if provider == "claude" else "GEMINI_API_KEY"
    monkeypatch.setenv(env_name, "sk-whatever")

    assert check_meta_provider_key(provider) is None


def test_local_provider_needs_no_key(monkeypatch):
    monkeypatch.delenv("ANTHROPIC_API_KEY", raising=False)
    monkeypatch.delenv("GEMINI_API_KEY", raising=False)

    assert check_meta_provider_key("local") is None


def test_effective_ollama_model_honours_the_env_override(monkeypatch):
    # 预检必须跟真实调用看同一个模型名:llm_client 发请求时读的就是这个环境
    # 变量,预检只看常量的话会报一个用户根本没在用的模型。
    monkeypatch.setenv("PZT_AGENT_OLLAMA_MODEL", "custom-model")
    assert effective_ollama_model() == "custom-model"

    monkeypatch.delenv("PZT_AGENT_OLLAMA_MODEL", raising=False)
    assert effective_ollama_model() == "gemma4:e2b"


def test_ollama_base_url_honours_the_env_override(monkeypatch):
    # 同上:预检探的地址必须跟真实调用发的地址一致,否则会出现"预检说连不
    # 上、实际调用好好的"(或反过来)这种最难查的错。
    monkeypatch.setenv("PZT_AGENT_OLLAMA_BASE_URL", "http://box.local:11500")
    assert ollama_base_url() == "http://box.local:11500"

    monkeypatch.delenv("PZT_AGENT_OLLAMA_BASE_URL", raising=False)
    assert ollama_base_url() == "http://localhost:11434"
