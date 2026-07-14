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


def test_vague_reply_while_planned_asks_a_clarifying_question(tmp_path):
    from compose.adjustment_parser import PlanConfirmationReply

    def _fake_clarify(original_intent, current_params, followup):
        assert followup == "不对"
        return PlanConfirmationReply(action="clarify", question="具体想改哪一项？")

    router, store, transport, _ = _make_router(tmp_path, refine_plan_confirmation_fn=_fake_clarify)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(_text_msg("筛一下"))

    run = router.handle_message(_text_msg("不对"))

    assert run.status == RunStatus.PLANNED
    assert any("具体想改哪一项？" in text for _, text in transport.sent_texts)


def test_specific_correction_while_planned_updates_params_and_re_confirms_without_running(tmp_path):
    from compose.adjustment_parser import PlanConfirmationReply

    def _fake_confirmed(original_intent, current_params, followup):
        assert followup == "改成6张"
        return PlanConfirmationReply(
            action="confirmed", provider=current_params["provider"],
            auto_reject=current_params["auto_reject"], count=6,
            apply_tag=current_params["apply_tag"],
        )

    router, store, transport, _ = _make_router(tmp_path, refine_plan_confirmation_fn=_fake_confirmed)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))
    router.handle_message(_text_msg("筛一下留2张"))

    run = router.handle_message(_text_msg("改成6张"))

    assert run.status == RunStatus.PLANNED  # 改完参数不自动开跑，等另一句"好的"
    curate_spec = next(s for s in run.plan.stages if s.name == "Curate")
    assert curate_spec.params["count"] == 6
    assert len(transport.sent_texts) == 2  # 第一次提议确认 + 改完之后重新确认
    assert "6" in transport.sent_texts[-1][1]

    approved = router.handle_message(_text_msg("好的"))
    assert approved.status == RunStatus.AWAITING_GATE


def test_natural_language_approval_while_planned_begins_running(tmp_path):
    from compose.adjustment_parser import PlanConfirmationReply

    def _fake_approve(original_intent, current_params, followup):
        return PlanConfirmationReply(action="approve")

    router, store, transport, _ = _make_router(tmp_path, refine_plan_confirmation_fn=_fake_approve)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))
    router.handle_message(_text_msg("筛一下留2张"))

    run = router.handle_message(_text_msg("好的，处理吧"))

    assert run.status == RunStatus.AWAITING_GATE


def test_natural_language_rejection_while_planned_cancels(tmp_path):
    from compose.adjustment_parser import PlanConfirmationReply

    def _fake_reject(original_intent, current_params, followup):
        return PlanConfirmationReply(action="reject")

    router, store, transport, _ = _make_router(tmp_path, refine_plan_confirmation_fn=_fake_reject)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(_text_msg("筛一下"))

    run = router.handle_message(_text_msg("算了不用了"))

    assert run.status == RunStatus.CANCELLED
    assert any("已取消" in text for _, text in transport.sent_texts)


def test_send_preview_falls_back_to_send_file_when_a_photo_is_too_big_and_still_sends_the_summary(tmp_path):
    # 真机验证时发现的真实 bug：_send_preview 循环里一张照片超过
    # Telegram 压缩图上限(BadRequest: File is too big)会直接把整个
    # for 循环炸掉，连"选好了 N 张"这句结尾提示都发不出去 -- 用户实际
    # 上已经停在 AwaitingGate 等回复，但完全不知道该做什么，看起来就
    # 是"卡住了"。
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))
    router.handle_message(_text_msg("筛一下留2张"))

    original_send_photo = transport.send_photo

    def _send_photo_rejecting_a(chat_id, path):
        if path.endswith("a.jpg"):
            raise RuntimeError("File is too big")
        original_send_photo(chat_id, path)

    transport.send_photo = _send_photo_rejecting_a

    run = router.handle_message(_text_msg("好的"))

    assert run.status == RunStatus.AWAITING_GATE
    preview_dir = tmp_path / "preview" / run.run_id
    assert transport.sent_photos == [(CHAT_ID, str(preview_dir / "b.jpg"))]
    assert transport.sent_files == [(CHAT_ID, str(preview_dir / "a.jpg"))]
    assert any("选好了 2 张" in text for _, text in transport.sent_texts)


def test_query_while_planned_answers_with_status_snapshot_and_stays_planned(tmp_path):
    from compose.adjustment_parser import PlanConfirmationReply

    def _fake_query(original_intent, current_params, followup):
        return PlanConfirmationReply(action="query")

    router, store, transport, _ = _make_router(tmp_path, refine_plan_confirmation_fn=_fake_query)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(_text_msg("筛一下"))

    run = router.handle_message(_text_msg("你收到几张图片了？"))

    assert run.status == RunStatus.PLANNED
    assert any("目前收到 1 张照片" in text for _, text in transport.sent_texts)


def test_unparseable_reply_while_planned_apologizes_via_refine_fn_error(tmp_path):
    from compose.adjustment_parser import AdjustmentError

    def _raising(original_intent, current_params, followup):
        raise AdjustmentError("unknown_action", "boom")

    router, store, transport, _ = _make_router(tmp_path, refine_plan_confirmation_fn=_raising)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(_text_msg("筛一下"))

    run = router.handle_message(_text_msg("乱说一句"))

    assert run.status == RunStatus.PLANNED
    assert any("没听懂" in text for _, text in transport.sent_texts)
