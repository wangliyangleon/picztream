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

    plan = compose_plan("出去玩拍了40张，挑12张发朋友圈", None, None, http_post=fake_http_post)

    assert [s.name for s in plan.stages] == ["Ingest", "Evaluate", "Dedup", "Curate", "Style", "Deliver"]
    assert all(s.gate == "off" for s in plan.stages)
    assert plan.stages[0].params == {}
    assert plan.stages[1].params == {"provider": "claude", "auto_reject": False}
    assert plan.stages[2].params == {}
    assert plan.stages[3].params == {"count": 12, "apply_tag": "朋友圈"}
    assert plan.stages[4].params == {"provider": "claude"}
    assert plan.stages[5].params == {}


def test_compose_plan_applies_defaults_when_llm_omits_fields(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({})

    plan = compose_plan("随便挑几张", None, None, http_post=fake_http_post)

    evaluate = next(s for s in plan.stages if s.name == "Evaluate")
    curate = next(s for s in plan.stages if s.name == "Curate")
    assert evaluate.params == {"provider": "gemini", "auto_reject": True}
    assert curate.params == {"count": 9, "apply_tag": "精选"}


def test_compose_plan_ignores_profile_and_last_config(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({"count": 5, "apply_tag": "精选"})

    plan = compose_plan("留5张", "friend-circle-profile", Plan(stages=[]), http_post=fake_http_post)

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
