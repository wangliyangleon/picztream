from orchestrator.types import RunStatus

from router_fakes import _make_router, _run_to_gate, _text_msg

# 注：photo-at-gate 的行为(F2 起改成排队，不再直接并入当前 run)现在由
# tests/router/test_session_router_queue.py::
# test_photo_at_gate_is_queued_not_merged_into_the_current_run 覆盖，
# 这里不再重复一份针对旧行为的测试。


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


def test_unparseable_reply_at_gate_keeps_the_gate_and_apologizes(tmp_path):
    from compose.adjustment_parser import AdjustmentError

    def _raising_classify(text, run):
        raise AdjustmentError("unknown_action", "boom")

    router, store, transport, _ = _make_router(tmp_path)
    router.classify_gate_reply_fn = _raising_classify
    _run_to_gate(router)

    result = router.handle_message(_text_msg("胡乱说一句"))

    assert result.status == RunStatus.AWAITING_GATE
    assert any("没听懂这句话" in text for _, text in transport.sent_texts)


def test_casual_approval_not_matching_keywords_is_recognized_via_classify_gate_reply(tmp_path):
    # 真机验证时复现的真实 bug："挺好的，就这三张吧"不精确等于
    # _APPROVE_KEYWORDS 里任何一项，之前会被当成一句解析不出来的调整，
    # Gate 卡住不动。classify_gate_reply_fn 兜底识别这种自然口语的同意。
    def _fake_classify_casual_approve(text, run):
        from compose.adjustment_parser import GateReply
        assert text == "挺好的，就这三张吧"
        return GateReply(action="approve")

    router, store, transport, _ = _make_router(tmp_path)
    router.classify_gate_reply_fn = _fake_classify_casual_approve
    _run_to_gate(router)

    result = router.handle_message(_text_msg("挺好的，就这三张吧"))

    assert result.status == RunStatus.DONE
    assert len(transport.sent_files) == 2
