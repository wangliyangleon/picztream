import pytest

from compose.validate import ValidationError, validate_plan
from orchestrator.types import Plan, StageSpec


def _valid_plan(**overrides):
    stages = {
        "Ingest": StageSpec(name="Ingest"),
        "Dedup": StageSpec(name="Dedup"),
        "Curate": StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选"}),
        "Style": StageSpec(name="Style", params={"provider": "gemini"}, gate="required"),
        "StyleApplyAll": StageSpec(name="StyleApplyAll", gate="required"),
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
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选"}),
    ])

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_stage_names"


def test_rejects_plan_missing_style_apply_all():
    plan = Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选"}),
        StageSpec(name="Style", params={"provider": "gemini"}),
        StageSpec(name="Deliver"),
    ])

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_stage_names"


@pytest.mark.parametrize("provider", ["openai", "", None, 123])
def test_rejects_bad_style_provider(provider):
    plan = _valid_plan(Style={"provider": provider})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_style_provider"


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
