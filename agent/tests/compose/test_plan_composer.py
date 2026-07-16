import json

from compose.plan_composer import compose_plan
from orchestrator.types import Plan


def _fake_gemini_http_post(decision_json):
    def fake(url, headers, body):
        response = {"candidates": [{"content": {"parts": [{"text": json.dumps(decision_json)}]}}]}
        return 200, json.dumps(response)
    return fake


def test_compose_plan_builds_five_stage_plan_from_llm_decision(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post(
        {"provider": "claude", "auto_reject": False, "count": 12, "apply_tag": "朋友圈"}
    )

    plan = compose_plan("出去玩拍了40张，挑12张发朋友圈", None, None, http_post=fake_http_post,
                         meta_provider="gemini")

    assert [s.name for s in plan.stages] == [
        "Ingest", "Evaluate", "Dedup", "Curate", "Style", "StyleApplyAll", "Deliver"]
    # Style/StyleApplyAll 是两段式的对话闸门(先问描述、再确认单张预
    # 览)，其它 stage 保持全自动不打断。
    gates = {s.name: s.gate for s in plan.stages}
    assert gates["Style"] == "required"
    assert gates["StyleApplyAll"] == "required"
    assert all(gate == "off" for name, gate in gates.items() if name not in ("Style", "StyleApplyAll"))
    assert plan.stages[0].params == {}
    assert plan.stages[1].params == {"provider": "claude", "auto_reject": False}
    assert plan.stages[2].params == {}
    assert plan.stages[3].params == {"count": 12, "apply_tag": "朋友圈"}
    assert plan.stages[4].params == {"provider": "claude"}
    assert plan.stages[5].params == {}
    assert plan.stages[6].params == {}


def test_compose_plan_applies_defaults_when_llm_omits_fields(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({})

    plan = compose_plan("随便挑几张", None, None, http_post=fake_http_post, meta_provider="gemini")

    evaluate = next(s for s in plan.stages if s.name == "Evaluate")
    curate = next(s for s in plan.stages if s.name == "Curate")
    style = next(s for s in plan.stages if s.name == "Style")
    # provider 的默认值(LLM 没提时的兜底)是 "local"，不是 meta_provider
    # (那是跑这次解析调用本身用的 provider，两者是独立的概念)。
    assert evaluate.params == {"provider": "local", "auto_reject": True}
    assert style.params == {"provider": "local"}
    assert curate.params == {"count": 9, "apply_tag": "精选"}


def test_compose_plan_ignores_profile_and_last_config(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({"count": 5, "apply_tag": "精选"})

    plan = compose_plan("留5张", "friend-circle-profile", Plan(stages=[]), http_post=fake_http_post,
                         meta_provider="gemini")

    curate = next(s for s in plan.stages if s.name == "Curate")
    assert curate.params["count"] == 5


def test_compose_plan_uses_meta_provider_for_its_own_llm_call(monkeypatch):
    monkeypatch.setenv("ANTHROPIC_API_KEY", "fake-key")
    captured = {}

    def fake_http_post(url, headers, body):
        captured["url"] = url
        response = {"content": [{"text": '{"count": 9, "apply_tag": "精选"}'}]}
        return 200, json.dumps(response)

    compose_plan("留9张", None, None, http_post=fake_http_post, meta_provider="claude")

    assert "anthropic" in captured["url"]


def test_compose_plan_defaults_meta_provider_to_local(monkeypatch):
    # quota 考虑：意图解析这一步默认打本地 Ollama，不打云端 API，见
    # docs/W2026-07-15_AgentStyle_Eng_Design.md。不传 meta_provider 时应
    # 该命中 compose/llm_client.py 的 local 分支，不需要任何 API key。
    captured = {}

    def fake_http_post(url, headers, body):
        captured["url"] = url
        response = {"message": {"role": "assistant", "content": '{"count": 9, "apply_tag": "精选"}'}}
        return 200, json.dumps(response)

    compose_plan("留9张", None, None, http_post=fake_http_post)

    assert captured["url"].endswith("/api/chat")
