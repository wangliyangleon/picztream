import subprocess

from orchestrator.stage import StageContext
from pzt_client import PztClient
from stages.curate import CurateStage


def test_curate_calls_pzt_curate_with_count_and_apply_tag():
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(
            argv, 0, stdout='{"requested": 9, "returned": 9, "selected": ["a.jpg", "b.jpg"]}\n', stderr=""
        )

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": 9, "apply_tag": "精选"})

    assert captured["argv"] == ["/fake/pzt", "curate", "proj-1", "--count", "9", "--apply-tag", "精选", "--json"]
    assert output.ok is True
    assert output.data["selected"] == ["a.jpg", "b.jpg"]


def test_curate_maps_command_failure_to_stage_failure():
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 1, stdout="", stderr='{"error": "usage", "message": "x"}\n')

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": 9, "apply_tag": "精选"})

    assert output.ok is False
