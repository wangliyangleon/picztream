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


# -- 票 11：选片确认阶段改题材要求 --


def test_classify_gate_reply_set_selection_brief_becomes_a_curate_delta(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    reply = classify_gate_reply(
        "要活泼一点的", run,
        http_post=_fake_http_post({"action": "set_selection_brief",
                                    "selection_brief": "表情活泼、有人的"}),
    )

    assert reply.action == "adjust"
    assert reply.delta.stage_name == "Curate"
    assert reply.delta.params == {"selection_brief": "表情活泼、有人的"}


def test_selection_brief_change_leaves_existing_exclude_alone(monkeypatch):
    # 票 11 决策一：exclude 是对具体某张照片的否定，换题材要求不撤销它。
    # 这里断言的是 delta 里**没有** exclude 这个 key - PlanDelta 走
    # params.update()，不放 key 就是"不动它"，放个空列表才是清空。
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"], exclude=["x.jpg"])

    reply = classify_gate_reply(
        "别都是风景", run,
        http_post=_fake_http_post({"action": "set_selection_brief",
                                    "selection_brief": "别都是风景"}),
    )

    assert "exclude" not in reply.delta.params
    assert reply.delta.params == {"selection_brief": "别都是风景"}


def test_swap_out_and_brief_in_one_sentence_both_take_effect(monkeypatch):
    # 票 11 验收：一句话里两件事都给了，两件事都生效，且互不擦除。这正是
    # 决策一选"保留"的理由之一 - 清空的话，同一句话里产生的 exclude 会
    # 被同一句话里的 brief 变更擦掉，结果取决于两个字段的应用先后。
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"], exclude=["x.jpg"])

    reply = classify_gate_reply(
        "把第3张换掉，要活泼点的", run,
        http_post=_fake_http_post({"action": "swap_out", "index": 3,
                                    "selection_brief": "表情活泼"}),
    )

    assert reply.delta.params == {"exclude": ["x.jpg", "c.jpg"],
                                   "selection_brief": "表情活泼"}


def test_set_count_and_brief_in_one_sentence_both_take_effect(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    reply = classify_gate_reply(
        "留5张，要有人的", run,
        http_post=_fake_http_post({"action": "set_count", "count": 5,
                                    "selection_brief": "要有人的"}),
    )

    assert reply.delta.params == {"count": 5, "selection_brief": "要有人的"}


def test_set_apply_tag_and_brief_in_one_sentence_both_take_effect(monkeypatch):
    run = _make_run(["a.jpg", "b.jpg"])

    reply = classify_gate_reply(
        "标签叫ins，要活泼的", run,
        http_post=_fake_http_post({"action": "set_apply_tag", "apply_tag": "ins",
                                    "selection_brief": "表情活泼"}),
    )

    assert reply.delta.params == {"apply_tag": "ins", "selection_brief": "表情活泼"}


def test_adjustment_without_a_brief_does_not_touch_the_existing_one(monkeypatch):
    # 票 11 决策二的边界：没提题材要求的轮次不能把旧简述清空。缺席与
    # 显式 null 都是"这次没提"，key 整个不放。
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    omitted = classify_gate_reply(
        "留5张", run, http_post=_fake_http_post({"action": "set_count", "count": 5}))
    explicit_null = classify_gate_reply(
        "留5张", run,
        http_post=_fake_http_post({"action": "set_count", "count": 5, "selection_brief": None}))

    assert omitted.delta.params == {"count": 5}
    assert explicit_null.delta.params == {"count": 5}


def test_empty_brief_is_an_explicit_clear_not_an_omission(monkeypatch):
    # 空串 = 用户明确要求去掉题材限制（"不用管题材了，随便选"），是一次
    # 有意的覆盖，key 必须放进去。沿用票 08 在 DedupFollowupReply 上立的
    # 同一套 None/"" 约定。
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    reply = classify_gate_reply(
        "不用管题材了，随便选", run,
        http_post=_fake_http_post({"action": "set_selection_brief", "selection_brief": ""}),
    )

    assert reply.delta.params == {"selection_brief": ""}


def test_set_selection_brief_without_a_brief_raises(monkeypatch):
    # 模型挑了这个 action 却没填自己的必填字段。静默产一个空 delta 会把
    # 旧简述清掉（用户没要求过），宁可报"没听懂"。
    run = _make_run(["a.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        classify_gate_reply("要活泼点的", run,
                            http_post=_fake_http_post({"action": "set_selection_brief"}))

    assert exc_info.value.code == "missing_selection_brief"


def test_parse_adjustment_does_not_learn_the_new_gate_action(monkeypatch):
    # 票 11 只扩 gate 那条路径。run_intent 的 parse_adjustment 提示词里
    # 从没有这个 action，它出现就说明模型在幻觉，仍按未知动作拒掉。
    run = _make_run(["a.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        parse_adjustment("要活泼点的", run,
                         http_post=_fake_http_post({"action": "set_selection_brief",
                                                     "selection_brief": "活泼"}))

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


def test_classify_dedup_followup_narrow_carries_a_selection_brief():
    # 票 08：去重后追问那一处的引导语现在也带题材偏好的例子（五处共用同一
    # 个示例函数），用户照着说了就必须接得住，否则那句引导是死的。
    reply = classify_dedup_followup(
        "留三张有景有人、表情活泼的照片发朋友圈", 8,
        http_post=_fake_http_post({"action": "narrow", "count": 3, "apply_tag": "朋友圈",
                                    "selection_brief": "发朋友圈用，要有景有人、表情活泼的"}))

    assert reply.action == "narrow"
    assert reply.count == 3
    assert reply.selection_brief == "发朋友圈用，要有景有人、表情活泼的"


def test_classify_dedup_followup_narrow_without_a_brief_leaves_it_none():
    # 只给了数量时 selection_brief 是 None 而不是空串：None 的意思是"这次
    # 没说"，交给 consumer 保留组装意图时抽出来的那一份；空串会把它冲掉。
    reply = classify_dedup_followup(
        "留5张", 8, http_post=_fake_http_post({"action": "narrow", "count": 5}))

    assert reply.selection_brief is None
