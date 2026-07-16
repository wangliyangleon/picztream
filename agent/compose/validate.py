"""compose_plan/parse_adjustment 输出喂进驱动器之前的最后一道确定性护
栏，见 docs/M4_Agent_Workflow_Design.md 五"这道校验挡住 LLM 输出污染
确定性驱动器"。纯逻辑，不调 LLM，是子增量 E 里测得最重的一块(docs
M4_Eng_Design.md 第七节子增量 E 验证要求)。
"""
from __future__ import annotations

from orchestrator.types import Plan

_EXPECTED_STAGE_NAMES = ["Ingest", "Evaluate", "Dedup", "Curate", "Style", "StyleApplyAll", "Deliver"]
_VALID_PROVIDERS = ("local", "gemini", "claude")
# 1 到 50：下限 1 是"至少选一张"这个最基本的合法性，交给 curate 自己
# 处理"候选不够"这种更细的场景；上限 50 对齐 PRD 示例(一天出去玩拍
# 40 张、挑 9 到 12 张)的量级上限，用来拦住模型编出"挑 500 张"这种
# 明显脱离场景的输出，不是精确调出来的数字，真机观察后可以再调。
_MIN_COUNT = 1
_MAX_COUNT = 50


class ValidationError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def validate_plan(plan: Plan) -> Plan:
    names = [s.name for s in plan.stages]
    if names != _EXPECTED_STAGE_NAMES:
        raise ValidationError(
            "bad_stage_names", f"expected stages {_EXPECTED_STAGE_NAMES}, got {names}"
        )

    by_name = {s.name: s for s in plan.stages}

    provider = by_name["Evaluate"].params.get("provider")
    if provider not in _VALID_PROVIDERS:
        raise ValidationError(
            "bad_evaluate_provider",
            f"Evaluate.params['provider'] must be one of {_VALID_PROVIDERS}, got {provider!r}",
        )

    auto_reject = by_name["Evaluate"].params.get("auto_reject")
    if not isinstance(auto_reject, bool):
        raise ValidationError(
            "bad_evaluate_auto_reject",
            f"Evaluate.params['auto_reject'] must be a bool, got {auto_reject!r}",
        )

    count = by_name["Curate"].params.get("count")
    # bool 是 int 的子类(isinstance(True, int) 为真)，模型如果回
    # true/false 混进 count 字段要单独拦住，不能被 isinstance(count, int)
    # 放过。
    if not isinstance(count, int) or isinstance(count, bool) or not (_MIN_COUNT <= count <= _MAX_COUNT):
        raise ValidationError(
            "bad_curate_count",
            f"Curate.params['count'] must be an int in [{_MIN_COUNT}, {_MAX_COUNT}], got {count!r}",
        )

    apply_tag = by_name["Curate"].params.get("apply_tag")
    if not isinstance(apply_tag, str) or not apply_tag:
        raise ValidationError(
            "bad_curate_apply_tag",
            f"Curate.params['apply_tag'] must be a non-empty string, got {apply_tag!r}",
        )

    style_provider = by_name["Style"].params.get("provider")
    if style_provider not in _VALID_PROVIDERS:
        raise ValidationError(
            "bad_style_provider",
            f"Style.params['provider'] must be one of {_VALID_PROVIDERS}, got {style_provider!r}",
        )

    return plan
