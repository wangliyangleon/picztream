from orchestrator.types import RunStatus
from transport.base import InboundMessage

from router_fakes import CHAT_ID, _make_router, _text_msg


def test_query_while_collecting_answers_with_status_snapshot_and_stays_collecting(tmp_path):
    from compose.adjustment_parser import CollectingReply

    def _fake_query(text, photo_count):
        assert photo_count == 1
        return CollectingReply(action="query")

    router, store, transport, _ = _make_router(tmp_path, classify_collecting_message_fn=_fake_query)
    src = tmp_path / "downloaded" / "a.jpg"
    src.parent.mkdir(parents=True)
    src.write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    run = router.handle_message(_text_msg("你收到几张图片了？"))

    assert run.status == RunStatus.COLLECTING
    assert any("目前收到 1 张照片" in text for _, text in transport.sent_texts)


def test_real_intent_while_collecting_still_proposes_a_plan(tmp_path):
    from compose.adjustment_parser import CollectingReply

    def _fake_intent(text, photo_count):
        return CollectingReply(action="intent")

    router, store, transport, _ = _make_router(tmp_path, classify_collecting_message_fn=_fake_intent)
    src = tmp_path / "downloaded" / "a.jpg"
    src.parent.mkdir(parents=True)
    src.write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    run = router.handle_message(_text_msg("筛一下留2张"))

    assert run.status == RunStatus.PLANNED


def test_classification_failure_while_collecting_falls_back_to_treating_message_as_intent(tmp_path):
    from compose.adjustment_parser import AdjustmentError

    def _raising(text, photo_count):
        raise AdjustmentError("unknown_action", "boom")

    router, store, transport, _ = _make_router(tmp_path, classify_collecting_message_fn=_raising)
    src = tmp_path / "downloaded" / "a.jpg"
    src.parent.mkdir(parents=True)
    src.write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    run = router.handle_message(_text_msg("筛一下留2张"))

    assert run.status == RunStatus.PLANNED  # 分类失败不能卡住，照老办法当意图处理
