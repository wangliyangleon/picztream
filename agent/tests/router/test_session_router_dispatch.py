from orchestrator.types import RunStatus
from transport.base import InboundMessage

from router_fakes import CHAT_ID, _make_router, _run_to_gate, _text_msg


def test_photo_arriving_at_gate_is_staged_but_does_not_disturb_the_pending_gate(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    run = _run_to_gate(router)
    src = tmp_path / "downloaded" / "extra.jpg"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_bytes(b"z")

    result = router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    assert result.status == RunStatus.AWAITING_GATE
    assert (tmp_path / "incoming" / run.run_id / "extra.jpg").read_bytes() == b"z"
    assert any("收到新照片" in text for _, text in transport.sent_texts)


def test_stale_awaiting_review_run_self_heals_to_done_then_starts_a_fresh_collecting_run(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    run = _run_to_gate(router)
    router.driver.resolve_gate(run, "proceed")  # Deliver 跑完了，但驱动循环还没来得及再 advance() 一次就"崩了"
    router.driver.advance(run)  # advance() 才是真正把"没有下一个 pending"翻译成 AWAITING_REVIEW 的那一步
    assert run.status == RunStatus.AWAITING_REVIEW

    new_run = router.handle_message(_text_msg("下一批的意图"))

    stale = store.load(run.run_id)
    assert stale.status == RunStatus.DONE
    assert new_run.run_id != run.run_id


def test_approve_keyword_at_gate_resolves_deliver_and_auto_approves_to_done(tmp_path):
    # 设计决定：Deliver 跑完落在 AWAITING_REVIEW 之后不二次追问用户，
    # _handle_gate 自己直接 approve() 到底，见 session_router.py 的
    # _handle_gate 实现和本计划 Self-review 第 1/2 条。
    router, store, transport, _ = _make_router(tmp_path)
    _run_to_gate(router)

    result = router.handle_message(_text_msg("好的"))

    assert result.status == RunStatus.DONE
    assert len(transport.sent_files) == 2  # DeliverStage 自己把两张选片发出去了


def test_reject_keyword_at_gate_cancels_the_run(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    _run_to_gate(router)

    result = router.handle_message(_text_msg("取消"))

    assert result.status == RunStatus.CANCELLED
    assert any("已取消" in text for _, text in transport.sent_texts)


def test_unparseable_adjustment_at_gate_keeps_the_gate_and_apologizes(tmp_path):
    from compose.adjustment_parser import AdjustmentError

    def _raising_parse_adjustment(text, run):
        raise AdjustmentError("unknown_action", "boom")

    router, store, transport, _ = _make_router(tmp_path)
    router.parse_adjustment_fn = _raising_parse_adjustment
    _run_to_gate(router)

    result = router.handle_message(_text_msg("胡乱说一句"))

    assert result.status == RunStatus.AWAITING_GATE
    assert any("没听懂这句调整" in text for _, text in transport.sent_texts)
