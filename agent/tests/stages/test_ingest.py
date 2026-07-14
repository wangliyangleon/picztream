import subprocess

from orchestrator.stage import StageContext
from pzt_client import PztClient
from stages.ingest import IngestStage


def test_ingest_calls_pzt_new_with_project_name_and_folder():
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(argv, 0, stdout='{"project": "proj-1", "image_count": 40}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = IngestStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"folder": "/tmp/photos"})

    assert output.ok is True
    assert output.data == {"image_count": 40}
    assert captured["argv"] == ["/fake/pzt", "new", "proj-1", "/tmp/photos", "--json"]


def test_ingest_maps_command_failure_to_stage_failure():
    def fake_runner(argv):
        return subprocess.CompletedProcess(
            argv, 1, stdout="", stderr='{"error": "no_images_found", "message": "no images found"}\n'
        )

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = IngestStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"folder": "/tmp/empty"})

    assert output.ok is False
    assert "no_images_found" in output.error
