import subprocess

from orchestrator.stage import StageContext
from pzt_client import PztClient
from stages.dedup import DedupStage


def test_dedup_calls_pzt_dedup_with_full_scope():
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(
            argv, 0, stdout='{"groups": 3, "tagged": 5, "skipped_no_capture_time": 0}\n', stderr=""
        )

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = DedupStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {})

    assert captured["argv"] == ["/fake/pzt", "dedup", "proj-1", "--scope", "*", "--json"]
    assert output.ok is True
    assert output.data == {"groups": 3, "tagged": 5, "skipped_no_capture_time": 0}


def test_dedup_maps_command_failure_to_stage_failure():
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 1, stdout="", stderr='{"error": "dedup_failed", "message": "x"}\n')

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = DedupStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {})

    assert output.ok is False
