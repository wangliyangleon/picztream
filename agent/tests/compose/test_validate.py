import pytest

from compose.validate import ValidationError, validate_plan
from orchestrator.types import Plan, StageSpec


def _valid_plan(**overrides):
    stages = {
        "Ingest": StageSpec(name="Ingest"),
        "Evaluate": StageSpec(name="Evaluate", params={"provider": "gemini", "auto_reject": True}),
        "Dedup": StageSpec(name="Dedup"),
        "Curate": StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选"}),
        "Deliver": StageSpec(name="Deliver"),
    }
    for name, params in overrides.items():
        stages[name].params.update(params)
    return Plan(stages=list(stages.values()))


def test_valid_plan_passes_through_unchanged():
    plan = _valid_plan()

    result = validate_plan(plan)

    assert result is plan


def test_rejects_wrong_stage_names_or_order():
    plan = Plan(stages=[StageSpec(name="Ingest"), StageSpec(name="Curate")])

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_stage_names"


def test_rejects_missing_stage():
    plan = Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Evaluate", params={"provider": "gemini", "auto_reject": True}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选"}),
    ])

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_stage_names"


@pytest.mark.parametrize("provider", ["openai", "", None, 123])
def test_rejects_bad_evaluate_provider(provider):
    plan = _valid_plan(Evaluate={"provider": provider})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_evaluate_provider"


@pytest.mark.parametrize("auto_reject", ["true", 1, None])
def test_rejects_non_bool_auto_reject(auto_reject):
    plan = _valid_plan(Evaluate={"auto_reject": auto_reject})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_evaluate_auto_reject"


@pytest.mark.parametrize("count", [0, -1, 51, "9", 9.5, True])
def test_rejects_bad_curate_count(count):
    plan = _valid_plan(Curate={"count": count})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_curate_count"


@pytest.mark.parametrize("count", [1, 9, 50])
def test_accepts_curate_count_at_range_boundaries(count):
    plan = _valid_plan(Curate={"count": count})

    assert validate_plan(plan) is plan


@pytest.mark.parametrize("apply_tag", ["", None, 123])
def test_rejects_bad_curate_apply_tag(apply_tag):
    plan = _valid_plan(Curate={"apply_tag": apply_tag})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_curate_apply_tag"
