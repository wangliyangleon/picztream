from orchestrator.types import RunStatus
from transport.base import InboundMessage

from router_fakes import CHAT_ID, _make_router, _text_msg


def test_intent_after_photos_proposes_a_plan_and_waits_for_confirmation(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))

    run = router.handle_message(_text_msg("筛一下留2张"))

    assert run.status == RunStatus.PLANNED
    # Plan 参数已经填好了(folder/out_folder/gate)，只是还没真的开跑
    assert run.plan.stages[0].params["folder"] == str(tmp_path / "incoming" / run.run_id)
    assert run.plan.stages[-1].params["out_folder"] == str(tmp_path / "deliver-out")
    assert run.plan.stages[-1].gate == "required"
    assert len(transport.sent_texts) == 1
    assert "精选" in transport.sent_texts[0][1]
    assert "2" in transport.sent_texts[0][1]


def test_approving_the_plan_confirmation_drives_pipeline_to_gate_and_sends_exported_preview(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))
    router.handle_message(_text_msg("筛一下留2张"))

    run = router.handle_message(_text_msg("好的"))

    assert run.status == RunStatus.AWAITING_GATE
    preview_dir = tmp_path / "preview" / run.run_id
    assert transport.sent_photos == [
        (CHAT_ID, str(preview_dir / "a.jpg")),
        (CHAT_ID, str(preview_dir / "b.jpg")),
    ]
    assert len(transport.sent_texts) == 2  # 第一条是确认回显，第二条是预览小结


def test_reject_keyword_while_planned_cancels_the_run(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(_text_msg("筛一下"))

    run = router.handle_message(_text_msg("算了"))

    assert run.status == RunStatus.CANCELLED
    assert any("已取消" in text for _, text in transport.sent_texts)


def test_photo_arriving_while_planned_merges_directly_into_the_run_folder(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    run = router.handle_message(_text_msg("筛一下"))
    assert run.status == RunStatus.PLANNED

    (downloaded / "b.jpg").write_bytes(b"b")
    result = router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))

    assert result.status == RunStatus.PLANNED
    assert (tmp_path / "incoming" / run.run_id / "b.jpg").read_bytes() == b"b"
    assert not (tmp_path / "incoming" / "_pending").exists()  # 直接并入，不走 F2 的排队区


def test_unrecognized_reply_while_planned_sends_a_generic_apology(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(_text_msg("筛一下"))

    run = router.handle_message(_text_msg("改成6张"))

    assert run.status == RunStatus.PLANNED  # Task 2 阶段还没接 LLM，先原地不动
    assert any("没听懂" in text for _, text in transport.sent_texts)
