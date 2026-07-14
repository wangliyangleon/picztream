import subprocess

from orchestrator.stage import StageContext
from pzt_client import PztClient
from stages.evaluate import EvaluateStage


def test_evaluate_calls_pzt_eval_with_scope_and_provider_and_auto_reject():
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(
            argv, 0,
            stdout='{"submitted": 2, "evaluated": [{"path": "a.jpg", "passes_gate": true, "overall_score": 8}], '
                   '"failed": [{"path": "b.jpg", "error": "decode_failed"}]}\n',
            stderr="",
        )

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = EvaluateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"provider": "gemini", "auto_reject": True})

    assert captured["argv"] == [
        "/fake/pzt", "eval", "proj-1", "--scope", "*", "--provider", "gemini", "--auto-reject", "--json",
    ]
    assert output.ok is True
    assert output.data["submitted"] == 2
    assert output.skipped == [{"path": "b.jpg", "error": "decode_failed"}]


def test_evaluate_omits_auto_reject_flag_when_disabled():
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(argv, 0, stdout='{"submitted": 0, "evaluated": [], "failed": []}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = EvaluateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    stage.run(ctx, {"provider": "gemini", "auto_reject": False})

    assert "--auto-reject" not in captured["argv"]


def test_evaluate_maps_command_failure_to_stage_failure():
    def fake_runner(argv):
        return subprocess.CompletedProcess(
            argv, 1, stdout="", stderr='{"error": "missing_api_key", "message": "GEMINI_API_KEY not set"}\n'
        )

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = EvaluateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"provider": "gemini", "auto_reject": True})

    assert output.ok is False
    assert "missing_api_key" in output.error
