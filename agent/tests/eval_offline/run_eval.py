#!/usr/bin/env python3
"""compose_plan/parse_adjustment 的离线 eval 集：手工挑的几条真实意图/
调整文本，跑一次真实 LLM，人眼过一遍输出像不像话。LLM 输出非确定，这
里不断言、不接入 pytest(文件名不是 test_*.py，pytest 默认 python_files
模式不会收集它，见 agent/pyproject.toml 的 [tool.pytest.ini_options])。
真正的自动化回归覆盖是 tests/compose/test_plan_composer.py 和
tests/compose/test_adjustment_parser.py 里注入假 http_post 的那些用
例，这份脚本只管"prompt 有没有跑偏"，靠人读，见 docs/M4_Eng_Design.md
第七节子增量 E 验证要求。

用法：
    cd agent && python tests/eval_offline/run_eval.py --provider gemini
需要设好 GEMINI_API_KEY(或 --provider claude 时的 ANTHROPIC_API_KEY)。
会花真实 API 额度，不是每次提交前都要跑的东西。
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))  # agent/ 根目录

from compose.adjustment_parser import parse_adjustment
from compose.plan_composer import compose_plan
from compose.validate import ValidationError, validate_plan
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus

COMPOSE_CASES = [
    "帮我筛一下，留9张",
    "出去玩了一天拍了40张，挑12张发朋友圈",
    "随便挑几张就行，不要太严格",
    "用 claude 评估，留6张，标签叫 精选投稿",
    "这批照片可能有点糊，严格点，多剔除一些",
]

ADJUSTMENT_CASES = [
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "换掉第2张"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "留2张就行"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "标签换成 朋友圈投稿"},
]


def _make_run(selected):
    plan = Plan(stages=[StageSpec(name="Curate", params={"count": len(selected), "apply_tag": "精选"})])
    return RunState(
        run_id="eval-offline", project_id="eval-offline", plan=plan,
        stage_states={"Curate": StageStatus.DONE},
        outputs={"Curate": StageOutput(ok=True, data={"selected": selected})},
        status=RunStatus.AWAITING_REVIEW,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="compose_plan/parse_adjustment 离线人工 eval(花真实 API 额度)")
    parser.add_argument("--provider", default="gemini", choices=["gemini", "claude"])
    args = parser.parse_args()

    print("=== compose_plan ===")
    for intent in COMPOSE_CASES:
        print(f"\n意图：{intent!r}")
        try:
            plan = validate_plan(compose_plan(intent, None, None, meta_provider=args.provider))
        except ValidationError as e:
            print(f"  校验失败：{e.code}: {e.message}")
            continue
        except Exception as e:
            print(f"  LLM 调用失败：{e}")
            continue
        evaluate = next(s for s in plan.stages if s.name == "Evaluate")
        curate = next(s for s in plan.stages if s.name == "Curate")
        print(f"  Evaluate.params = {evaluate.params}")
        print(f"  Curate.params   = {curate.params}")

    print("\n=== parse_adjustment ===")
    for case in ADJUSTMENT_CASES:
        run = _make_run(case["selected"])
        print(f"\n已选：{case['selected']}，调整消息：{case['msg']!r}")
        try:
            delta = parse_adjustment(case["msg"], run, meta_provider=args.provider)
        except Exception as e:
            print(f"  解析失败：{e}")
            continue
        print(f"  PlanDelta(stage_name={delta.stage_name!r}, params={delta.params})")


if __name__ == "__main__":
    main()
