import json

from compose.plan_composer import compose_plan
from orchestrator.types import Plan


def _fake_gemini_http_post(decision_json):
    def fake(url, headers, body):
        response = {"candidates": [{"content": {"parts": [{"text": json.dumps(decision_json)}]}}]}
        return 200, json.dumps(response)
    return fake


def test_compose_plan_builds_five_stage_plan_when_count_is_given(monkeypatch):
    # W2026-07-21 目标三：给了目标张数就不单独跑 Dedup，curate 的粗聚类已
    # 经隐含去重效果（决策一）。
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post(
        {"provider": "claude", "ai_enabled": True, "count": 12, "apply_tag": "朋友圈"}
    )

    plan = compose_plan("出去玩拍了40张，用AI帮我挑12张发朋友圈", None, None, http_post=fake_http_post,
                         meta_provider="gemini")

    assert [s.name for s in plan.stages] == ["Ingest", "Curate", "Style", "StyleApplyAll", "Deliver"]
    # Style/StyleApplyAll 是两段式的对话闸门(先问描述、再确认单张预
    # 览)，其它 stage 保持全自动不打断。
    gates = {s.name: s.gate for s in plan.stages}
    assert gates["Style"] == "required"
    assert gates["StyleApplyAll"] == "required"
    assert all(gate == "off" for name, gate in gates.items() if name not in ("Style", "StyleApplyAll"))
    assert plan.stages[0].params == {}
    assert plan.stages[1].params == {"count": 12, "apply_tag": "朋友圈",
                                      "ai_enabled": True, "provider": "claude"}
    assert plan.stages[2].params == {"provider": "claude"}
    assert plan.stages[3].params == {}
    assert plan.stages[4].params == {}


def test_compose_plan_skips_dedup_and_defaults_count_when_llm_omits_fields(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({})

    plan = compose_plan("随便挑几张", None, None, http_post=fake_http_post, meta_provider="gemini")

    assert [s.name for s in plan.stages] == ["Ingest", "Curate", "Style", "StyleApplyAll", "Deliver"]
    curate = next(s for s in plan.stages if s.name == "Curate")
    style = next(s for s in plan.stages if s.name == "Style")
    # provider 的默认值(LLM 没提时的兜底)是 "local"，不是 meta_provider
    # (那是跑这次解析调用本身用的 provider，两者是独立的概念)。ai_enabled
    # 没提时默认 false——对齐"opt-in 重路径，不默认全量"。
    assert style.params == {"provider": "local"}
    assert curate.params == {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}


def test_compose_plan_defers_curate_when_dedup_requested_without_count(monkeypatch):
    # W2026-07-21 目标三案例二：只说去重没给数量，Curate 的决定推迟到
    # Dedup 跑完之后（Commit 8 接的闸门）。
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({"dedup_requested": True})

    plan = compose_plan("帮我去一下重", None, None, http_post=fake_http_post, meta_provider="gemini")

    assert [s.name for s in plan.stages] == [
        "Ingest", "Dedup", "Curate", "Style", "StyleApplyAll", "Deliver"]
    dedup = next(s for s in plan.stages if s.name == "Dedup")
    curate = next(s for s in plan.stages if s.name == "Curate")
    assert dedup.params == {"ai_enabled": False, "provider": "local"}
    assert curate.params["count"] is None
    assert curate.gate == "required"


def test_compose_plan_skips_dedup_even_when_requested_if_count_given(monkeypatch):
    # 决策一容易写反的一条：dedup_requested=True 但也给了 count 时，仍然
    # 走"不单独跑 Dedup"分支（count 存在优先于 dedup_requested）。
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    fake_http_post = _fake_gemini_http_post({"dedup_requested": True, "count": 5})

    plan = compose_plan("去重之后留5张", None, None, http_post=fake_http_post, meta_provider="gemini")

    assert [s.name for s in plan.stages] == ["Ingest", "Curate", "Style", "StyleApplyAll", "Deliver"]
    curate = next(s for s in plan.stages if s.name == "Curate")
    assert curate.params["count"] == 5
    assert curate.gate == "off"


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
