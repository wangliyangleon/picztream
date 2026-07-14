#!/usr/bin/env python3
"""意图驱动的 watch-folder runner(子增量 E)：一句自由文本意图经
compose_plan 组成 Plan，跑到 AwaitingReview 之后支持对话式调整(留在
Curate 参数上打转)，approve 才算结束。跟 run_watchfolder.py(子增量 D
的固定 Plan、自动 approve)是两条并行入口，D 继续留作回归基线，见
docs/M4_Eng_Design.md 第七节子增量 D/E。
"""
from __future__ import annotations

import argparse
import uuid
from pathlib import Path
from typing import List

from compose.adjustment_parser import parse_adjustment
from compose.plan_composer import compose_plan
from compose.validate import validate_plan
from orchestrator.driver import Driver
from orchestrator.types import Plan, RunState, RunStatus, StageStatus
from pzt_client import PztClient
from run_watchfolder import build_transport
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from store.run_store import RunStore


def _fill_transport_params(plan: Plan, in_folder: str, out_folder: str) -> None:
    ingest_spec = next(s for s in plan.stages if s.name == "Ingest")
    deliver_spec = next(s for s in plan.stages if s.name == "Deliver")
    ingest_spec.params["folder"] = in_folder
    deliver_spec.params["out_folder"] = out_folder


def _print_selected(run: RunState) -> List[str]:
    curate_output = run.outputs.get("Curate")
    selected = curate_output.data.get("selected", []) if curate_output else []
    for i, path in enumerate(selected, start=1):
        print(f"  第{i}张：{path}")
    return selected


def main() -> None:
    parser = argparse.ArgumentParser(
        description="意图驱动的 watch-folder runner（子增量 E，LLM 组 Plan + 对话调整）"
    )
    parser.add_argument("in_folder", nargs="?", help="待处理的照片文件夹")
    parser.add_argument("out_folder", nargs="?", help="产出 keeper 落地的文件夹")
    parser.add_argument("--intent", help="一句话意图，例如 '筛一下留9张'")
    parser.add_argument("--run-id")
    parser.add_argument("--resume", metavar="RUN_ID", help="加载已存在的 run_id 续跑")
    parser.add_argument("--state-dir", help="agent 状态落盘目录，默认 ~/.pzt-agent")
    args = parser.parse_args()

    state_dir = Path(args.state_dir) if args.state_dir else Path.home() / ".pzt-agent"
    store = RunStore(state_dir / "runs")
    marker_dir = state_dir / "delivered"
    staging_dir = state_dir / "staging"
    client = PztClient()

    if args.resume:
        run = store.load(args.resume)
        print(f"续跑 run_id={run.run_id}，当前状态：" +
              ", ".join(f"{k}={v.value}" for k, v in run.stage_states.items()))
    else:
        if not args.in_folder or not args.out_folder or not args.intent:
            parser.error("in_folder/out_folder/--intent 必填（或改用 --resume RUN_ID）")
        plan = validate_plan(compose_plan(args.intent, None, None))
        _fill_transport_params(plan, args.in_folder, args.out_folder)
        run_id = args.run_id or f"intent-{uuid.uuid4().hex[:8]}"
        run = RunState(run_id=run_id, project_id=run_id, plan=plan,
                        stage_states={s.name: StageStatus.PENDING for s in plan.stages},
                        status=RunStatus.RUNNING)
        print(f"新建 run_id={run_id}，project={run_id}，意图：{args.intent!r}")

    transport = build_transport(run)
    inbound = list(transport.receive())
    print(f"watch-folder 里现有 {len(inbound)} 张图片：{transport.in_dir}")

    stages = {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=marker_dir, staging_dir=staging_dir),
    }
    driver = Driver(stages=stages, store=store)

    while run.status == RunStatus.RUNNING:
        driver.advance(run)
        print("  " + ", ".join(f"{k}={v.value}" for k, v in run.stage_states.items()) + f"  [{run.status.value}]")

    while run.status == RunStatus.AWAITING_REVIEW:
        print("选好了：")
        _print_selected(run)
        reply = input("满意就直接回车/approve；不满意说说想怎么调（换掉第N张 / 留M张 / 换标签）：").strip()
        if reply == "" or reply.lower() == "approve":
            driver.approve(run)
            print("已 approve")
            break
        delta = parse_adjustment(reply, run)
        driver.apply_adjustment(run, delta)
        while run.status == RunStatus.RUNNING:
            driver.advance(run)
            print("  " + ", ".join(f"{k}={v.value}" for k, v in run.stage_states.items()) + f"  [{run.status.value}]")

    print(f"Run {run.run_id} 结束，status={run.status.value}")
    if run.status == RunStatus.FAILED:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
