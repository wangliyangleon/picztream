from pathlib import Path

from compose.validate import ValidationError
from orchestrator.types import RunStatus
from transport.base import InboundMessage

from router_fakes import CHAT_ID, _fake_runner, _make_router, _text_msg


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
    router.handle_message(_text_msg("筛一下"))  # 现在只会停在 PLANNED 等确认

    run = router.handle_message(_text_msg("好的"))  # 确认后才真正开跑，这时才会跑到失败

    assert run.status == RunStatus.FAILED
    assert any("quota_exceeded" in text for _, text in transport.sent_texts)
