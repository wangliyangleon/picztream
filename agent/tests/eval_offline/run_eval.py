#!/usr/bin/env python3
"""compose_plan/parse_adjustment 的离线 eval 集：手工挑的几条真实意图/
调整文本，跑一次真实 LLM，人眼过一遍输出像不像话。LLM 输出非确定，这
里不断言、不接入 pytest(文件名不是 test_*.py，pytest 默认 python_files
模式不会收集它，见 agent/pyproject.toml 的 [tool.pytest.ini_options])。
真正的自动化回归覆盖是 tests/compose/test_plan_composer.py 和
tests/compose/test_adjustment_parser.py 里注入假 http_post 的那些用
例，这份脚本只管"prompt 有没有跑偏"，靠人读，见 docs/history/M4_Eng_Design.md
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

from compose.adjustment_parser import (classify_gate_reply, parse_adjustment,
                                        refine_plan_confirmation)
from compose.plan_composer import compose_plan
from compose.validate import ValidationError, validate_plan
from orchestrator.types import Plan, RunState, RunStatus, StageOutput, StageSpec, StageStatus

COMPOSE_CASES = [
    "帮我筛一下，留9张",
    "出去玩了一天拍了40张，挑12张发朋友圈",
    "随便挑几张就行，不要太严格",
    "用 claude 评估，留6张，标签叫 精选投稿",
    "这批照片可能有点糊，严格点，多剔除一些",
    # 票 08 的三条：选片简述抽得对不对，是 prompt 质量问题、不是逻辑问题，
    # 只能靠人读（自动化那边注入假 http_post，验的是接线）。分别要看：
    # 1) 题材偏好与叙事要求进没进简述、张数与用途有没有被它带跑；
    # 2) 去重/服务商/寒暄这些跟"选哪几张"无关的噪声有没有漏进去（验收标准
    #    六，透传原文的话必漏）；
    # 3) 什么偏好都没说时是不是干脆给空串，而不是硬编一段出来。
    "选三张有景有人、表情活泼的照片发朋友圈",
    "你好呀～先去重，然后用 gemini 挑5张有小孩的，按拍摄时间顺序排",
    "选3张发朋友圈",
    # 真机反馈 2026-08-02 打出来的两条，一句话同时考两件事：
    # 1) 只有一个笼统形容词（"比较活泼"）时它必须活着进简述 - 本地模型原
    #    先照着上面"选3张发朋友圈 -> 发朋友圈用"那条例子只留目的地，把偏好
    #    整个吃掉，而这会实打实改变选出哪几张；
    # 2) "选一张"这个中文数字必须解成 count=1 而不是 null（null 会走下游
    #    默认的 9 张，一句"选一张"变成选 9 张）。
    "选一张比较活泼的照片发Facebook",
]

ADJUSTMENT_CASES = [
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "换掉第2张"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "留2张就行"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "标签换成 朋友圈投稿"},
]

# 票 11：选片确认闸门上的回复。这一组是本票验收标准最后一条的落点 -
# 本票往 _GATE_SCHEMA_INSTRUCTION 上加了一个 action 和一个可选字段，而
# 票 08 的真机教训正是"罐头响应永远绿，往 schema 说明上加规则会挤掉别的
# 字段"。tests/compose 那边注入假 http_post 验的是接线，挤没挤掉只能真
# 模型跑一遍、人读。每条要看的：
#   1-3) 纯题材要求要落成 set_selection_brief，简述里不该混进张数/标签；
#   4-5) 一句话两件事，count/index 与 selection_brief 必须同时出现（新加
#        的可选字段最容易在这里把原有字段挤掉）；
#   6-7) 没提题材要求时 selection_brief 必须缺席或 null，绝不能凭空造一
#        句出来 - 造出来就等于用户没要求却改了题材（决策二下这是静默覆盖）；
#   8)   明确要求去掉题材限制时才给空串；
#   9-10) approve/query 不能被新加的 action 抢走。
GATE_REPLY_CASES = [
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "要活泼一点的"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "换成有人的那几张"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "别都是风景"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "把第3张换掉，要活泼点的"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "留5张，要有人的"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "留2张就行"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "换掉第2张"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "不用管题材了，随便选"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "挺好的，就这三张吧"},
    {"selected": ["a.jpg", "b.jpg", "c.jpg"], "msg": "选了几张呀？"},
]


# 票 13：方案确认阶段（PLANNED / refine_plan）。真机 2026-08-03 打出来的那
# 两条在这儿：没有 selection_brief 这个可调字段时，"小清新"会被硬塞进
# apply_tag 或 ai_enabled。每条要看的：
#   1-2) 纯题材要求 + 数量，两件事都要落到位，且 ai_enabled 不能被带翻；
#   3)   只改题材要求时其余字段原样保留；
#   4-5) 没提题材要求的轮次不能凭空造简述、也不能把旧的清掉；
#   6)   明确不要题材限制时才给空串；
#   7-8) approve/query 不能被新字段抢走。
_CONFIRM_CURRENT = {"count": 2, "apply_tag": "ins", "ai_enabled": False,
                    "provider": "local", "selection_brief": "要有景有人的"}

CONFIRMATION_CASES = [
    "选两张小清新一点的吧",
    "选两张画面小清新一点的照片",
    "要活泼点的",
    "改成6张",
    "标签叫朋友圈",
    "不用管题材了",
    "好的，处理吧",
    "现在留几张？",
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
    # local 也是一个合法选项：这几个分类器在生产里的默认 meta_provider 就
    # 是 local(Ollama)，拿云端模型验过的 prompt 不等于本地模型也读得懂 -
    # 票 08 真机踩的正是本地模型把偏好整个吃掉那一次。
    parser.add_argument("--provider", default="gemini", choices=["gemini", "claude", "local"])
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
        curate = next(s for s in plan.stages if s.name == "Curate")
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

    print("\n=== classify_gate_reply（票 11）===")
    for case in GATE_REPLY_CASES:
        run = _make_run(case["selected"])
        print(f"\n已选：{case['selected']}，闸门回复：{case['msg']!r}")
        try:
            reply = classify_gate_reply(case["msg"], run, meta_provider=args.provider)
        except Exception as e:
            print(f"  解析失败：{e}")
            continue
        params = reply.delta.params if reply.delta is not None else None
        print(f"  action={reply.action!r}, params={params}")

    print("\n=== refine_plan_confirmation（票 13）===")
    print(f"当前方案：{_CONFIRM_CURRENT}")
    for msg in CONFIRMATION_CASES:
        print(f"\n用户回复：{msg!r}")
        try:
            reply = refine_plan_confirmation("选几张发ins", dict(_CONFIRM_CURRENT), msg,
                                              meta_provider=args.provider)
        except Exception as e:
            print(f"  解析失败：{e}")
            continue
        print(f"  action={reply.action!r}, count={reply.count!r}, "
              f"apply_tag={reply.apply_tag!r}, ai_enabled={reply.ai_enabled!r}, "
              f"selection_brief={reply.selection_brief!r}")


if __name__ == "__main__":
    main()
