import json

import pytest

from compose.adjustment_parser import (
    AdjustmentError,
    classify_collecting_message,
    classify_dedup_followup,
    classify_gate_reply,
    classify_style_describe,
    parse_adjustment,
    refine_plan_confirmation,
)
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus


def _fake_http_post(decision_json):
    # 目标三：meta_provider 默认值改成 "local"（Ollama），不再需要
    # GEMINI_API_KEY，响应形状也跟着换成 message.content（见
    # compose/llm_client.py::_parse_local_response）。
    def fake(url, headers, body):
        response = {"message": {"role": "assistant", "content": json.dumps(decision_json)}}
        return 200, json.dumps(response)
    return fake


def _make_run(selected, exclude=None):
    curate_spec = StageSpec(name="Curate", params={"count": len(selected), "apply_tag": "精选"})
    if exclude is not None:
        curate_spec.params["exclude"] = exclude
    plan = Plan(stages=[curate_spec])
    return RunState(
        run_id="run-1", project_id="proj-1", plan=plan,
        stage_states={"Curate": StageStatus.DONE},
        outputs={"Curate": StageOutput(ok=True, data={"selected": selected})},
        status=RunStatus.AWAITING_REVIEW,
    )


def test_parse_adjustment_set_count(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg"])

    delta = parse_adjustment("留12张", run, http_post=_fake_http_post({"action": "set_count", "count": 12}))

    assert delta.stage_name == "Curate"
    assert delta.params == {"count": 12}


def test_parse_adjustment_set_apply_tag(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg"])

    delta = parse_adjustment(
        "标签换成 朋友圈投稿", run,
        http_post=_fake_http_post({"action": "set_apply_tag", "apply_tag": "朋友圈投稿"}),
    )

    assert delta.stage_name == "Curate"
    assert delta.params == {"apply_tag": "朋友圈投稿"}


def test_parse_adjustment_swap_out_resolves_index_against_selected_order(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    delta = parse_adjustment("换掉第3张", run, http_post=_fake_http_post({"action": "swap_out", "index": 3}))

    assert delta.stage_name == "Curate"
    assert delta.params == {"exclude": ["c.jpg"]}


def test_parse_adjustment_swap_out_merges_with_existing_exclude(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"], exclude=["x.jpg"])

    delta = parse_adjustment("换掉第2张", run, http_post=_fake_http_post({"action": "swap_out", "index": 2}))

    assert delta.params == {"exclude": ["x.jpg", "b.jpg"]}


def test_parse_adjustment_swap_out_out_of_range_raises(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        parse_adjustment("换掉第5张", run, http_post=_fake_http_post({"action": "swap_out", "index": 5}))

    assert exc_info.value.code == "index_out_of_range"


def test_parse_adjustment_unknown_action_raises(monkeypatch):
    run = _make_run(["a.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        parse_adjustment("随便你", run, http_post=_fake_http_post({"action": "do_something_else"}))

    assert exc_info.value.code == "unknown_action"


def test_classify_gate_reply_recognizes_casual_approval(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    reply = classify_gate_reply("挺好的，就这三张吧", run, http_post=_fake_http_post({"action": "approve"}))

    assert reply.action == "approve"
    assert reply.delta is None


def test_classify_gate_reply_recognizes_reject(monkeypatch):
    run = _make_run(["a.jpg"])

    reply = classify_gate_reply("算了不要了", run, http_post=_fake_http_post({"action": "reject"}))

    assert reply.action == "reject"
    assert reply.delta is None


def test_classify_gate_reply_still_resolves_adjustments_with_a_delta(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    reply = classify_gate_reply("换掉第3张", run, http_post=_fake_http_post({"action": "swap_out", "index": 3}))

    assert reply.action == "adjust"
    assert reply.delta.stage_name == "Curate"
    assert reply.delta.params == {"exclude": ["c.jpg"]}


def test_classify_gate_reply_unknown_action_raises(monkeypatch):
    run = _make_run(["a.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        classify_gate_reply("随便你", run, http_post=_fake_http_post({"action": "do_something_else"}))

    assert exc_info.value.code == "unknown_action"


def test_refine_plan_confirmation_returns_clarify_question_for_vague_reply(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    reply = refine_plan_confirmation(
        "帮我选几张发朋友圈", current_params, "不对",
        http_post=_fake_http_post({"action": "clarify", "question": "具体想改哪一项？"}),
    )

    assert reply.action == "clarify"
    assert reply.question == "具体想改哪一项？"


def test_refine_plan_confirmation_merges_specific_correction_and_keeps_the_rest(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    reply = refine_plan_confirmation(
        "帮我选几张发朋友圈", current_params, "改成6张",
        http_post=_fake_http_post({"action": "confirmed", "count": 6}),
    )

    assert reply.action == "confirmed"
    assert reply.count == 6
    assert reply.apply_tag == "精选"          # 用户没提，保留原值
    assert reply.ai_enabled is False          # 用户没提，保留原值
    assert reply.provider == "local"          # 用户没提，保留原值


def test_refine_plan_confirmation_merges_ai_enabled_and_provider_and_keeps_the_rest(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    reply = refine_plan_confirmation(
        "帮我选几张发朋友圈", current_params, "AI帮我选，换成gemini",
        http_post=_fake_http_post({"action": "confirmed", "ai_enabled": True, "provider": "gemini"}),
    )

    assert reply.action == "confirmed"
    assert reply.ai_enabled is True
    assert reply.provider == "gemini"
    assert reply.count == 9                   # 用户没提，保留原值
    assert reply.apply_tag == "精选"          # 用户没提，保留原值


def test_refine_plan_confirmation_unknown_action_raises(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    with pytest.raises(AdjustmentError) as exc_info:
        refine_plan_confirmation(
            "帮我选几张发朋友圈", current_params, "随便",
            http_post=_fake_http_post({"action": "do_something_else"}),
        )

    assert exc_info.value.code == "unknown_action"


def test_classify_gate_reply_recognizes_a_status_query(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    reply = classify_gate_reply("选了几张呀？", run, http_post=_fake_http_post({"action": "query"}))

    assert reply.action == "query"


def test_refine_plan_confirmation_recognizes_a_status_query(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    reply = refine_plan_confirmation(
        "帮我选几张发朋友圈", current_params, "你收到几张图片了？",
        http_post=_fake_http_post({"action": "query"}),
    )

    assert reply.action == "query"


def test_refine_plan_confirmation_recognizes_natural_language_approval(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    reply = refine_plan_confirmation(
        "帮我选几张发朋友圈", current_params, "好的，处理吧",
        http_post=_fake_http_post({"action": "approve"}),
    )

    assert reply.action == "approve"


def test_refine_plan_confirmation_recognizes_natural_language_rejection(monkeypatch):
    current_params = {"count": 9, "apply_tag": "精选", "ai_enabled": False, "provider": "local"}

    reply = refine_plan_confirmation(
        "帮我选几张发朋友圈", current_params, "算了不用了",
        http_post=_fake_http_post({"action": "reject"}),
    )

    assert reply.action == "reject"


def test_classify_collecting_message_recognizes_a_status_query(monkeypatch):

    reply = classify_collecting_message("你收到几张图片了？", 7, http_post=_fake_http_post({"action": "query"}))

    assert reply.action == "query"


def test_classify_collecting_message_recognizes_a_real_intent(monkeypatch):

    reply = classify_collecting_message("帮我选几张发朋友圈", 7, http_post=_fake_http_post({"action": "intent"}))

    assert reply.action == "intent"


def test_classify_collecting_message_unknown_action_raises(monkeypatch):

    with pytest.raises(AdjustmentError) as exc_info:
        classify_collecting_message("随便说点什么", 7, http_post=_fake_http_post({"action": "do_something_else"}))

    assert exc_info.value.code == "unknown_action"


def test_classify_style_describe_recognizes_all_four_actions():
    for action in ("describe", "skip", "cancel", "query"):
        reply = classify_style_describe("随便一句", http_post=_fake_http_post({"action": action}))
        assert reply.action == action


def test_classify_style_describe_unknown_action_raises():
    with pytest.raises(AdjustmentError) as exc_info:
        classify_style_describe("随便你", http_post=_fake_http_post({"action": "do_something_else"}))

    assert exc_info.value.code == "unknown_action"


def test_classify_dedup_followup_recognizes_narrow_with_count():
    reply = classify_dedup_followup(
        "留5张", 8, http_post=_fake_http_post({"action": "narrow", "count": 5}))

    assert reply.action == "narrow"
    assert reply.count == 5
    assert reply.apply_tag is None  # 没提标签/目的地


def test_classify_dedup_followup_recognizes_narrow_with_apply_tag():
    reply = classify_dedup_followup(
        "选一张发朋友圈", 8,
        http_post=_fake_http_post({"action": "narrow", "count": 1, "apply_tag": "朋友圈"}))

    assert reply.action == "narrow"
    assert reply.count == 1
    assert reply.apply_tag == "朋友圈"


def test_classify_dedup_followup_recognizes_approve():
    reply = classify_dedup_followup(
        "对，好的", 8, http_post=_fake_http_post({"action": "approve"}))

    assert reply.action == "approve"


def test_classify_dedup_followup_recognizes_skip():
    reply = classify_dedup_followup(
        "不用了，都留着", 8, http_post=_fake_http_post({"action": "skip"}))

    assert reply.action == "skip"
    assert reply.count is None


def test_classify_dedup_followup_recognizes_query():
    reply = classify_dedup_followup(
        "现在还剩几张？", 8, http_post=_fake_http_post({"action": "query"}))

    assert reply.action == "query"


def test_classify_dedup_followup_recognizes_cancel():
    reply = classify_dedup_followup(
        "算了不要了", 8, http_post=_fake_http_post({"action": "cancel"}))

    assert reply.action == "cancel"


def test_classify_dedup_followup_unknown_action_raises():
    with pytest.raises(AdjustmentError) as exc_info:
        classify_dedup_followup("随便你", 8, http_post=_fake_http_post({"action": "do_something_else"}))

    assert exc_info.value.code == "unknown_action"
