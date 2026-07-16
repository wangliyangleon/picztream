import subprocess

from orchestrator.stage import StageContext
from pzt_client import PztClient
from stages.style import StyleStage


def _make_client(call_log, suggestions=None, fail_paths=None):
    # test_curate.py 的 _make_client 按 argv[1] 分派就够了(curate/tag 不
    # 会撞名字)，这里 "recipe suggest" 和 "recipe apply" 的 argv[1] 都是
    # "recipe"，必须看 argv[2] 才能区分。
    suggestions = suggestions or {}
    fail_paths = fail_paths or set()

    def fake_runner(argv):
        call_log.append(argv)
        assert argv[1] == "recipe"
        if argv[2] == "suggest":
            path = argv[4]
            if path in fail_paths:
                return subprocess.CompletedProcess(
                    argv, 1, stdout="", stderr='{"error": "image_unavailable", "message": "decode failed"}\n')
            recipe_name = suggestions.get(path, "Havana 1959")
            return subprocess.CompletedProcess(
                argv, 0,
                stdout=f'{{"recipe_name": "{recipe_name}", "reasoning": "fits the mood"}}', stderr="")
        if argv[2] == "apply":
            return subprocess.CompletedProcess(
                argv, 0, stdout='{"applied": true, "recipe_name": "' + argv[5] + '"}', stderr="")
        raise AssertionError(f"unexpected argv: {argv}")

    return PztClient(pzt_bin="/fake/pzt", runner=fake_runner)


def _ctx(selected):
    from orchestrator.types import StageOutput
    return StageContext(run_id="run-1", project_id="proj-1",
                         outputs={"Curate": StageOutput(ok=True, data={"selected": selected})})


def test_style_suggests_then_applies_for_every_selected_photo():
    call_log = []
    client = _make_client(call_log, suggestions={"a.jpg": "Havana 1959", "b.jpg": "Munich 1951"})
    stage = StyleStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg"]), {"provider": "gemini"})

    assert call_log[0] == ["/fake/pzt", "recipe", "suggest", "proj-1", "a.jpg", "--provider", "gemini", "--json"]
    assert call_log[1] == ["/fake/pzt", "recipe", "apply", "proj-1", "a.jpg", "Havana 1959", "--json"]
    assert call_log[2] == ["/fake/pzt", "recipe", "suggest", "proj-1", "b.jpg", "--provider", "gemini", "--json"]
    assert call_log[3] == ["/fake/pzt", "recipe", "apply", "proj-1", "b.jpg", "Munich 1951", "--json"]
    assert len(call_log) == 4
    assert output.ok is True
    assert output.data["applied"] == {"a.jpg": "Havana 1959", "b.jpg": "Munich 1951"}
    assert output.skipped == []


def test_style_partial_failure_keeps_successes_and_reports_skipped():
    call_log = []
    client = _make_client(call_log, suggestions={"b.jpg": "Munich 1951"}, fail_paths={"a.jpg"})
    stage = StyleStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg"]), {"provider": "gemini"})

    assert output.ok is True
    assert output.data["applied"] == {"b.jpg": "Munich 1951"}
    assert len(output.skipped) == 1
    assert output.skipped[0]["path"] == "a.jpg"
    assert "image_unavailable" in output.skipped[0]["error"]


def test_style_total_failure_reports_stage_failure():
    call_log = []
    client = _make_client(call_log, fail_paths={"a.jpg", "b.jpg"})
    stage = StyleStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg"]), {"provider": "gemini"})

    assert output.ok is False
    assert output.data["applied"] == {}
    assert len(output.skipped) == 2


def test_style_with_no_selected_photos_makes_no_calls():
    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client)

    output = stage.run(_ctx([]), {"provider": "gemini"})

    assert output.ok is True
    assert output.data["applied"] == {}
    assert call_log == []
