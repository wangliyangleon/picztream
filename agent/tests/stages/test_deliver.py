import subprocess

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient
from stages.deliver import DeliverStage


class FakeTransport:
    def __init__(self):
        self.sent_files = []
        self.sent_texts = []

    def send_file(self, chat_id, path):
        self.sent_files.append((chat_id, path))

    def send_photo(self, chat_id, path):
        self.sent_files.append((chat_id, path))

    def send_text(self, chat_id, text):
        self.sent_texts.append((chat_id, text))


def make_curate_output_ctx(selected):
    return StageContext(
        run_id="run-1", project_id="proj-1",
        outputs={"Curate": StageOutput(ok=True, data={"selected": selected})},
    )


def test_deliver_exports_then_sends_each_selected_file(tmp_path):
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 2, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    stage = DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered")
    ctx = make_curate_output_ctx(["a.jpg", "b.jpg"])

    output = stage.run(ctx, {"out_folder": str(tmp_path / "out")})

    assert captured["argv"] == ["/fake/pzt", "export-images", "proj-1", "a.jpg", "b.jpg", str(tmp_path / "out"), "--json"]
    assert output.ok is True
    assert len(transport.sent_files) == 2
    assert len(transport.sent_texts) == 1


def test_deliver_writes_marker_after_successful_send(tmp_path):
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=transport, marker_dir=marker_dir)
    ctx = make_curate_output_ctx(["a.jpg"])

    stage.run(ctx, {"out_folder": str(tmp_path / "out")})

    assert (marker_dir / "run-1.json").exists()


def test_deliver_skips_resend_when_marker_already_exists(tmp_path):
    calls = []

    def fake_runner(argv):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=transport, marker_dir=marker_dir)
    ctx = make_curate_output_ctx(["a.jpg"])

    stage.run(ctx, {"out_folder": str(tmp_path / "out")})  # 第一次真正发送
    output = stage.run(ctx, {"out_folder": str(tmp_path / "out")})  # 第二次:模拟重跑

    assert len(calls) == 1  # export-images 只被真正调用了一次
    assert len(transport.sent_files) == 1  # 只发送了一次，没有重发
    assert output.ok is True
    assert output.data.get("already_delivered") is True


def test_deliver_maps_export_failure_to_stage_failure(tmp_path):
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 1, stdout="", stderr='{"error": "io_error", "message": "x"}\n')

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    stage = DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered")
    ctx = make_curate_output_ctx(["a.jpg"])

    output = stage.run(ctx, {"out_folder": str(tmp_path / "out")})

    assert output.ok is False
    assert len(transport.sent_files) == 0
