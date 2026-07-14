from pathlib import Path

from compose.validate import ValidationError
from orchestrator.types import RunStatus
from transport.base import InboundMessage

from fakes import CHAT_ID, _fake_runner, _make_router, _text_msg


def test_first_photo_message_creates_a_collecting_run_and_stages_the_file(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    src = tmp_path / "downloaded" / "a.jpg"
    src.parent.mkdir(parents=True)
    src.write_bytes(b"a")

    run = router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    assert run.status == RunStatus.COLLECTING
    active = store.list_active()
    assert len(active) == 1
    assert active[0].run_id == run.run_id
    assert (tmp_path / "incoming" / run.run_id / "a.jpg").read_bytes() == b"a"


def test_second_photo_message_reuses_the_same_collecting_run(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")

    first = router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    second = router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))

    assert second.run_id == first.run_id
    assert len(store.list_active()) == 1
    run_dir = tmp_path / "incoming" / first.run_id
    assert (run_dir / "a.jpg").read_bytes() == b"a"
    assert (run_dir / "b.jpg").read_bytes() == b"b"


def test_intent_after_photos_drives_pipeline_to_gate_and_sends_exported_preview(tmp_path):
    router, store, transport, _ = _make_router(tmp_path)
    downloaded = tmp_path / "downloaded"
    downloaded.mkdir()
    (downloaded / "a.jpg").write_bytes(b"a")
    (downloaded / "b.jpg").write_bytes(b"b")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "a.jpg")))
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(downloaded / "b.jpg")))

    run = router.handle_message(_text_msg("筛一下留2张"))

    assert run.status == RunStatus.AWAITING_GATE
    assert run.plan.stages[0].params["folder"] == str(tmp_path / "incoming" / run.run_id)
    assert run.plan.stages[-1].params["out_folder"] == str(tmp_path / "deliver-out")
    assert run.plan.stages[-1].gate == "required"
    preview_dir = tmp_path / "preview" / run.run_id
    assert transport.sent_photos == [
        (CHAT_ID, str(preview_dir / "a.jpg")),
        (CHAT_ID, str(preview_dir / "b.jpg")),
    ]
    assert len(transport.sent_texts) == 1


def test_compose_plan_failure_keeps_the_run_collecting_and_apologizes(tmp_path):
    def _raising_compose_plan(intent, profile, last_config):
        raise ValidationError("bad_stage_names", "boom")

    router, store, transport, _ = _make_router(tmp_path, compose_plan_fn=_raising_compose_plan)
    src = tmp_path / "downloaded" / "a.jpg"
    src.parent.mkdir(parents=True)
    src.write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    run = router.handle_message(_text_msg("瞎说一通"))

    assert run.status == RunStatus.COLLECTING
    assert len(transport.sent_texts) == 1
    assert (tmp_path / "incoming" / run.run_id / "a.jpg").read_bytes() == b"a"


def test_critical_stage_failure_sends_a_failure_text_with_error_detail(tmp_path):
    def _failing_runner(argv):
        if argv[1] == "eval":
            import subprocess
            return subprocess.CompletedProcess(argv, 1, stdout="",
                                                stderr='{"error": "quota_exceeded", "message": "no credits"}\n')
        return _fake_runner(argv)

    router, store, transport, _ = _make_router(tmp_path, runner=_failing_runner)
    src = tmp_path / "downloaded" / "a.jpg"
    src.parent.mkdir(parents=True)
    src.write_bytes(b"a")
    router.handle_message(InboundMessage(kind="photo", chat_id=CHAT_ID, file_path=str(src)))

    run = router.handle_message(_text_msg("筛一下"))

    assert run.status == RunStatus.FAILED
    assert any("quota_exceeded" in text for _, text in transport.sent_texts)
