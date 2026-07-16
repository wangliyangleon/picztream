import json

from compose.llm_client import LlmRequestError
from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError
from stages.style import StyleStage


def _ollama_response(recipe_name: str) -> str:
    body = json.dumps({"recipe_name": recipe_name, "reasoning": "fits the mood"})
    return json.dumps({"message": {"content": body}})


def _fake_http_post(recipe_name: str = "Havana 1959"):
    def fn(url, headers, body):
        del url, headers, body
        return 200, _ollama_response(recipe_name)
    return fn


def _make_client(call_log, fail_paths=None):
    fail_paths = fail_paths or set()

    def fake_runner(argv):
        call_log.append(argv)
        assert argv[1] == "recipe" and argv[2] == "apply"
        path = argv[4]
        if path in fail_paths:
            import subprocess
            return subprocess.CompletedProcess(
                argv, 1, stdout="", stderr='{"error": "recipe_not_found", "message": "no such recipe"}\n')
        import subprocess
        return subprocess.CompletedProcess(
            argv, 0, stdout='{"applied": true, "recipe_name": "' + argv[5] + '"}', stderr="")

    return PztClient(pzt_bin="/fake/pzt", runner=fake_runner)


def _ctx(selected):
    return StageContext(run_id="run-1", project_id="proj-1",
                         outputs={"Curate": StageOutput(ok=True, data={"selected": selected})})


def test_style_matches_a_preset_and_applies_it_to_the_representative_photo_only():
    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client, http_post=_fake_http_post("Havana 1959"))

    output = stage.run(_ctx(["a.jpg", "b.jpg"]), {"style_description": "暖色调怀旧"})

    assert len(call_log) == 1
    assert call_log[0] == ["/fake/pzt", "recipe", "apply", "proj-1", "a.jpg", "Havana 1959", "--json"]
    assert output.ok is True
    assert output.data == {"chosen_recipe": "Havana 1959", "preview_photo": "a.jpg"}


def test_style_fails_without_a_description():
    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client, http_post=_fake_http_post())

    output = stage.run(_ctx(["a.jpg"]), {})

    assert output.ok is False
    assert call_log == []


def test_style_with_blank_description_fails():
    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client, http_post=_fake_http_post())

    output = stage.run(_ctx(["a.jpg"]), {"style_description": "   "})

    assert output.ok is False
    assert call_log == []


def test_style_with_no_selected_photos_is_a_no_op():
    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client, http_post=_fake_http_post())

    output = stage.run(_ctx([]), {"style_description": "暖色调怀旧"})

    assert output.ok is True
    assert output.data == {"chosen_recipe": None, "preview_photo": None}
    assert call_log == []


def test_style_fails_when_the_matcher_hallucinates():
    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client, http_post=_fake_http_post("Not A Real Preset"))

    output = stage.run(_ctx(["a.jpg"]), {"style_description": "暖色调怀旧"})

    assert output.ok is False
    assert call_log == []


def test_style_fails_when_the_matcher_raises_llm_request_error():
    def bad_http_post(url, headers, body):
        del url, headers, body
        return 200, "not valid json"

    call_log = []
    client = _make_client(call_log)
    stage = StyleStage(client=client, http_post=bad_http_post)

    output = stage.run(_ctx(["a.jpg"]), {"style_description": "暖色调怀旧"})

    assert output.ok is False
    assert call_log == []


def test_style_fails_when_recipe_apply_fails():
    call_log = []
    client = _make_client(call_log, fail_paths={"a.jpg"})
    stage = StyleStage(client=client, http_post=_fake_http_post("Havana 1959"))

    output = stage.run(_ctx(["a.jpg"]), {"style_description": "暖色调怀旧"})

    assert output.ok is False
