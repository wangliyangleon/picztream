import json

import pytest

from compose.adjustment_parser import AdjustmentError, parse_adjustment
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus


def _fake_http_post(decision_json):
    def fake(url, headers, body):
        response = {"candidates": [{"content": {"parts": [{"text": json.dumps(decision_json)}]}}]}
        return 200, json.dumps(response)
    return fake


def _make_run(selected, exclude=None):
    curate_spec = StageSpec(name="Curate", params={"count": len(selected), "apply_tag": "精选"})
    if exclude is not None:
        curate_spec.params["exclude"] = exclude
    plan = Plan(stages=[curate_spec])
    return RunState(
        run_id="run-1", project_id="proj-1", plan=plan,
        stage_states={"Curate": StageStatus.DONE},
        outputs={"Curate": StageOutput(ok=True, data={"selected": selected})},
        status=RunStatus.AWAITING_REVIEW,
    )


def test_parse_adjustment_set_count(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    run = _make_run(["a.jpg", "b.jpg"])

    delta = parse_adjustment("留12张", run, http_post=_fake_http_post({"action": "set_count", "count": 12}))

    assert delta.stage_name == "Curate"
    assert delta.params == {"count": 12}


def test_parse_adjustment_set_apply_tag(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    run = _make_run(["a.jpg", "b.jpg"])

    delta = parse_adjustment(
        "标签换成 朋友圈投稿", run,
        http_post=_fake_http_post({"action": "set_apply_tag", "apply_tag": "朋友圈投稿"}),
    )

    assert delta.stage_name == "Curate"
    assert delta.params == {"apply_tag": "朋友圈投稿"}


def test_parse_adjustment_swap_out_resolves_index_against_selected_order(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    delta = parse_adjustment("换掉第3张", run, http_post=_fake_http_post({"action": "swap_out", "index": 3}))

    assert delta.stage_name == "Curate"
    assert delta.params == {"exclude": ["c.jpg"]}


def test_parse_adjustment_swap_out_merges_with_existing_exclude(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"], exclude=["x.jpg"])

    delta = parse_adjustment("换掉第2张", run, http_post=_fake_http_post({"action": "swap_out", "index": 2}))

    assert delta.params == {"exclude": ["x.jpg", "b.jpg"]}


def test_parse_adjustment_swap_out_out_of_range_raises(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    run = _make_run(["a.jpg", "b.jpg", "c.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        parse_adjustment("换掉第5张", run, http_post=_fake_http_post({"action": "swap_out", "index": 5}))

    assert exc_info.value.code == "index_out_of_range"


def test_parse_adjustment_unknown_action_raises(monkeypatch):
    monkeypatch.setenv("GEMINI_API_KEY", "fake-key")
    run = _make_run(["a.jpg"])

    with pytest.raises(AdjustmentError) as exc_info:
        parse_adjustment("随便你", run, http_post=_fake_http_post({"action": "do_something_else"}))

    assert exc_info.value.code == "unknown_action"
