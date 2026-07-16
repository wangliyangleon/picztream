from orchestrator.types import RunStatus

from router_fakes import CHAT_ID, _fake_style_http_post, _make_router, _stage_source_photo, _text_msg
from transport.base import InboundMessage


def _run_to_style_gate(router):
    tmp_path = router.incoming_root.parent
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                          file_path=_stage_source_photo(tmp_path, "a.jpg", b"a")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID,
                                          file_path=_stage_source_photo(tmp_path, "b.jpg", b"b")))
    router.handle_message(_text_msg("筛一下留2张"))
    return router.handle_message(_text_msg("好的"))  # PLANNED -> RUNNING -> 停在 Style 闸门


def test_style_gate_asks_for_a_description(tmp_path):
    router, store, transport, client = _make_router(tmp_path)

    run = _run_to_style_gate(router)

    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "Style"
    assert any("风格" in text for _, text in transport.sent_texts)


def test_style_gate_blank_reply_does_not_advance(tmp_path):
    router, store, transport, client = _make_router(tmp_path)
    _run_to_style_gate(router)
    sent_before = list(transport.sent_texts)

    run = router.handle_message(_text_msg("   "))

    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "Style"
    assert len(transport.sent_texts) > len(sent_before)


def test_style_gate_reply_is_used_as_the_description_and_advances_to_style_apply_all_gate(tmp_path):
    router, store, transport, client = _make_router(tmp_path, style_http_post=_fake_style_http_post("Havana 1959"))
    _run_to_style_gate(router)

    run = router.handle_message(_text_msg("复古暖色调"))

    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "StyleApplyAll"
    assert run.outputs["Style"].data["chosen_recipe"] == "Havana 1959"
    assert any("Havana 1959" in text for _, text in transport.sent_texts)


def test_style_apply_all_gate_approve_advances_to_deliver_gate(tmp_path):
    router, store, transport, client = _make_router(tmp_path, style_http_post=_fake_style_http_post("Havana 1959"))
    _run_to_style_gate(router)
    router.handle_message(_text_msg("复古暖色调"))

    run = router.handle_message(_text_msg("好的"))

    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "Deliver"
    assert any("选好了 2 张" in text for _, text in transport.sent_texts)


def test_style_apply_all_gate_rejecting_with_new_description_reruns_style(tmp_path):
    calls = {"n": 0}

    def sequenced_http_post(url, headers, body):
        del url, headers, body
        calls["n"] += 1
        name = "Havana 1959" if calls["n"] == 1 else "Munich 1951"
        import json
        response = {"message": {"content": json.dumps({"recipe_name": name, "reasoning": "x"})}}
        return 200, json.dumps(response)

    router, store, transport, client = _make_router(tmp_path, style_http_post=sequenced_http_post)
    _run_to_style_gate(router)
    router.handle_message(_text_msg("复古暖色调"))

    run = router.handle_message(_text_msg("再暗一点、黑白的"))

    assert run.status == RunStatus.AWAITING_GATE
    assert run.gate_state.stage_name == "StyleApplyAll"
    assert run.outputs["Style"].data["chosen_recipe"] == "Munich 1951"
    assert any("Munich 1951" in text for _, text in transport.sent_texts)


def test_style_gate_reject_keyword_cancels(tmp_path):
    router, store, transport, client = _make_router(tmp_path)
    _run_to_style_gate(router)

    run = router.handle_message(_text_msg("取消"))

    assert run.status == RunStatus.CANCELLED
    assert any("已取消" in text for _, text in transport.sent_texts)


def test_style_apply_all_gate_reject_keyword_cancels(tmp_path):
    router, store, transport, client = _make_router(tmp_path, style_http_post=_fake_style_http_post("Havana 1959"))
    _run_to_style_gate(router)
    router.handle_message(_text_msg("复古暖色调"))

    run = router.handle_message(_text_msg("算了"))

    assert run.status == RunStatus.CANCELLED
    assert any("已取消" in text for _, text in transport.sent_texts)
