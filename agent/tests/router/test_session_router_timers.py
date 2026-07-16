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


def test_check_progress_updates_reports_photo_count_after_first_interval(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, progress_interval_seconds=60)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(60)

    router.check_progress_updates()

    assert any("已收到 1 张图片" in text for _, text in transport.sent_texts)


def test_check_progress_updates_does_nothing_before_the_interval(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, progress_interval_seconds=60)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(59)

    router.check_progress_updates()

    assert transport.sent_texts == []


def test_check_progress_updates_fires_on_a_fixed_cadence_even_with_continuous_activity(tmp_path):
    # 跟 check_idle_timers 唯一但关键的语义差异：不受 last_activity_at
    # 影响，即使用户(这里是持续发照片)一直在活动，也照样按固定间隔触
    # 发——这正是"收图期间"最想要反馈的场景。
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, progress_interval_seconds=60)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(30)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "b.jpg", b"b"),
    ))  # 这条消息会刷新 last_activity_at，但不该影响 check_progress_updates
    clock.advance(30)

    router.check_progress_updates()

    assert any("已收到 2 张图片" in text for _, text in transport.sent_texts)


def test_check_progress_updates_ignores_non_collecting_status(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, progress_interval_seconds=60)
    _run_to_gate(router)  # 走到 AWAITING_GATE 本身已经会产生一堆消息(见 Task 2)
    sent_before = list(transport.sent_texts)
    clock.advance(60)

    router.check_progress_updates()

    assert transport.sent_texts == sent_before  # 没有因为这个检查多发任何东西


def test_check_progress_updates_does_not_repeat_within_the_same_interval(tmp_path):
    clock = FakeClock()
    router, store, transport, _ = _make_router(tmp_path, now_fn=clock, progress_interval_seconds=60)
    router.handle_message(InboundMessage(
        kind="photo", chat_id=CHAT_ID, file_path=_stage_source_photo(tmp_path, "a.jpg", b"a"),
    ))
    clock.advance(60)
    router.check_progress_updates()
    clock.advance(1)

    router.check_progress_updates()

    assert len(transport.sent_texts) == 1
