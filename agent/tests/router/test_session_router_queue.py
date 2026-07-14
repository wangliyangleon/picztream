from orchestrator.types import RunStatus
from transport.base import InboundMessage

from router_fakes import CHAT_ID, FakeClock, _make_router, _run_to_gate, _stage_source_photo, _text_msg


def test_photo_at_gate_is_queued_not_merged_into_the_current_run(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    run = _run_to_gate(router)
    src = tmp_path / "downloaded" / "extra.jpg"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_bytes(b"z")

    result = router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    assert result.status == RunStatus.AWAITING_GATE
    assert not (tmp_path / "incoming" / run.run_id / "extra.jpg").exists()
    assert (tmp_path / "incoming" / "_pending" / "extra.jpg").read_bytes() == b"z"
    assert any("先帮你收着" in text for _, text in transport.sent_texts)


def test_queued_photos_are_drained_into_the_next_run_after_approve(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    first_run = _run_to_gate(router)
    src = tmp_path / "downloaded" / "extra.jpg"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_bytes(b"z")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    router.handle_message(_text_msg("好的"))  # approve -> Done
    new_run = router.handle_message(_text_msg("下一批意图"))  # 自愈成新 Collecting run，顺带排空队列

    assert new_run.run_id != first_run.run_id
    assert (tmp_path / "incoming" / new_run.run_id / "extra.jpg").read_bytes() == b"z"
    assert not (tmp_path / "incoming" / "_pending").exists()
    assert any("之前排队的 1 张已经并进这一批了" in text for _, text in transport.sent_texts)


def test_every_dispatched_message_touches_last_activity_and_clears_reminder_sent(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock)
    run = _run_to_gate(router)
    assert run.last_activity_at == clock.now

    run.reminder_sent = True
    store.save(run)
    clock.advance(10)

    updated = router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "extra.jpg", b"z"),
    ))

    assert updated.last_activity_at == clock.now
    assert updated.reminder_sent is False
