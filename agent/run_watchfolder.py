#!/usr/bin/env python3
"""一次性批处理 runner：丢一个文件夹进去，固定 Plan 跑完
Ingest -> Evaluate -> Dedup -> Curate -> Deliver，产出写到另一个文件
夹。子增量 D 范围：不含 LLM/意图解析/对话式微调/闸门交互——全自动跑
到底，见 docs/M4_Eng_Design.md 第七节子增量 D、本计划 Context。
"""
from __future__ import annotations

import argparse
import uuid
from pathlib import Path

from orchestrator.driver import Driver
from orchestrator.types import Plan, RunState, RunStatus, StageSpec, StageStatus
from pzt_client import PztClient
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from store.run_store import RunStore
from transport.watchfolder import WatchFolderTransport


def build_plan(in_folder: str, out_folder: str, count: int, provider: str, apply_tag: str,
                auto_reject: bool) -> Plan:
    return Plan(stages=[
        StageSpec(name="Ingest", params={"folder": in_folder}),
        StageSpec(name="Evaluate", params={"provider": provider, "auto_reject": auto_reject}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": count, "apply_tag": apply_tag}),
        StageSpec(name="Deliver", params={"out_folder": out_folder}),
    ])


def build_transport(run: RunState) -> WatchFolderTransport:
    ingest_spec = next(s for s in run.plan.stages if s.name == "Ingest")
    deliver_spec = next(s for s in run.plan.stages if s.name == "Deliver")
    return WatchFolderTransport(in_dir=Path(ingest_spec.params["folder"]),
                                 out_dir=Path(deliver_spec.params["out_folder"]))


def main() -> None:
    parser = argparse.ArgumentParser(description="watch-folder 一次性批处理 runner（子增量 D，无 LLM/无对话）")
    parser.add_argument("in_folder", nargs="?", help="待处理的照片文件夹")
    parser.add_argument("out_folder", nargs="?", help="产出 keeper 落地的文件夹")
    parser.add_argument("--count", type=int, default=9)
    parser.add_argument("--provider", default="gemini", choices=["gemini", "claude"])
    parser.add_argument("--apply-tag", default="精选")
    parser.add_argument("--no-auto-reject", action="store_true")
    parser.add_argument("--run-id")
    parser.add_argument("--resume", metavar="RUN_ID", help="加载已存在的 run_id 续跑，不需要再传 in_folder/out_folder")
    parser.add_argument("--state-dir", help="agent 状态落盘目录，默认 ~/.pzt-agent")
    args = parser.parse_args()

    state_dir = Path(args.state_dir) if args.state_dir else Path.home() / ".pzt-agent"
    store = RunStore(state_dir / "runs")
    marker_dir = state_dir / "delivered"
    client = PztClient()

    if args.resume:
        run = store.load(args.resume)
        print(f"续跑 run_id={run.run_id}，当前状态：" +
              ", ".join(f"{k}={v.value}" for k, v in run.stage_states.items()))
    else:
        if not args.in_folder or not args.out_folder:
            parser.error("in_folder/out_folder 必填（或改用 --resume RUN_ID）")
        run_id = args.run_id or f"watchfolder-{uuid.uuid4().hex[:8]}"
        plan = build_plan(in_folder=args.in_folder, out_folder=args.out_folder, count=args.count,
                           provider=args.provider, apply_tag=args.apply_tag, auto_reject=not args.no_auto_reject)
        run = RunState(run_id=run_id, project_id=run_id, plan=plan,
                        stage_states={s.name: StageStatus.PENDING for s in plan.stages}, status=RunStatus.RUNNING)
        print(f"新建 run_id={run_id}，project={run_id}")

    transport = build_transport(run)
    inbound = list(transport.receive())
    print(f"watch-folder 里现有 {len(inbound)} 张图片：{transport.in_dir}")

    stages = {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=marker_dir),
    }
    driver = Driver(stages=stages, store=store)

    while run.status == RunStatus.RUNNING:
        driver.advance(run)
        print("  " + ", ".join(f"{k}={v.value}" for k, v in run.stage_states.items()) + f"  [{run.status.value}]")

    if run.status == RunStatus.AWAITING_REVIEW:
        driver.approve(run)
        print("已自动 approve（子增量 D 无对话复核，E/F 才接上真人确认）")

    print(f"Run {run.run_id} 结束，status={run.status.value}")
    if run.status == RunStatus.FAILED:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
