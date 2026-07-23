import pytest

from compose.validate import ValidationError, validate_plan
from orchestrator.types import Plan, StageSpec


def _valid_plan(**overrides):
    stages = {
        "Ingest": StageSpec(name="Ingest"),
        "Dedup": StageSpec(name="Dedup", params={"ai_enabled": False, "provider": "local"}),
        "Curate": StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选",
                                                     "ai_enabled": False, "provider": "local"}),
        "Style": StageSpec(name="Style", params={"provider": "gemini"}, gate="required"),
        "StyleApplyAll": StageSpec(name="StyleApplyAll", gate="required"),
        "Deliver": StageSpec(name="Deliver"),
    }
    for name, params in overrides.items():
        stages[name].params.update(params)
    return Plan(stages=list(stages.values()))


def _valid_plan_without_dedup(**overrides):
    plan = _valid_plan(**overrides)
    plan.stages = [s for s in plan.stages if s.name != "Dedup"]
    return plan


def _valid_plan_deferred_curate(count=None):
    # W2026-07-21 目标三案例二：Dedup 存在、Curate 待定（count=None,
    # gate="required"）。count 参数只用于构造"本该是 None 却给了数字"的
    # 非法样例（见 test_rejects_count_present_when_curate_gate_required）。
    plan = _valid_plan(Curate={"count": count})
    curate = next(s for s in plan.stages if s.name == "Curate")
    curate.gate = "required"
    return plan


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


def test_valid_plan_without_dedup_passes():
    plan = _valid_plan_without_dedup()

    assert validate_plan(plan) is plan


def test_valid_plan_with_deferred_curate_passes():
    plan = _valid_plan_deferred_curate()

    assert validate_plan(plan) is plan


def test_rejects_count_present_when_curate_gate_required():
    plan = _valid_plan_deferred_curate(count=9)

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_curate_count"


@pytest.mark.parametrize("provider", ["openai", "", None, 123])
def test_rejects_bad_style_provider(provider):
    plan = _valid_plan(Style={"provider": provider})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == "bad_style_provider"


@pytest.mark.parametrize("stage_name", ["Dedup", "Curate"])
@pytest.mark.parametrize("ai_enabled", ["true", 1, None])
def test_rejects_non_bool_ai_enabled(stage_name, ai_enabled):
    plan = _valid_plan(**{stage_name: {"ai_enabled": ai_enabled}})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == f"bad_{stage_name.lower()}_ai_enabled"


@pytest.mark.parametrize("stage_name", ["Dedup", "Curate"])
@pytest.mark.parametrize("provider", ["openai", "", None, 123])
def test_rejects_bad_dedup_or_curate_provider(stage_name, provider):
    plan = _valid_plan(**{stage_name: {"provider": provider}})

    with pytest.raises(ValidationError) as exc_info:
        validate_plan(plan)

    assert exc_info.value.code == f"bad_{stage_name.lower()}_provider"


@pytest.mark.parametrize("count", [0, -1, 51, "9", 9.5, True, None])
def test_rejects_bad_curate_count(count):
    # None 只在 Curate.gate == "required" 时合法（见
    # test_valid_plan_with_deferred_curate_passes）；这里 gate 是默认的
    # "off"，None 也该被拒。
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
