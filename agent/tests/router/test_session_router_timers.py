from transport.base import InboundMessage

from router_fakes import CHAT_ID, FakeClock, _make_router, _run_to_gate, _stage_source_photo


def test_check_idle_timers_nudges_collecting_run_with_no_intent_after_threshold(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, idle_reminder_seconds=300)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(301)

    router.check_idle_timers()

    assert any("看到你发了 1 张，想怎么处理？" in text for _, text in transport.sent_texts)
    assert store.list_active()[0].reminder_sent is True


def test_check_idle_timers_does_nothing_before_threshold(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, idle_reminder_seconds=300)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(299)

    router.check_idle_timers()

    assert transport.sent_texts == []


def test_check_idle_timers_only_fires_once_per_idle_stretch(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, idle_reminder_seconds=300)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(301)
    router.check_idle_timers()
    clock.advance(301)

    router.check_idle_timers()

    assert len(transport.sent_texts) == 1


def test_check_idle_timers_nudges_awaiting_gate_run_after_threshold(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, idle_reminder_seconds=300)
    _run_to_gate(router)
    clock.advance(301)

    router.check_idle_timers()

    assert any("还在等你的回复" in text for _, text in transport.sent_texts)


def test_check_idle_timers_does_nothing_when_no_active_run(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, idle_reminder_seconds=300)

    router.check_idle_timers()  # 不该抛异常，也不该发任何东西

    assert transport.sent_texts == []
