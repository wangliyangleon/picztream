#!/usr/bin/env python3
"""Telegram 长驻会话 runner（W2026-07-15 目标五）：consumer/worker 双线程
运行时。consumer（主线程）只做秒级消息反应/事件应用/timers；长活（LLM
分类/编排、pzt drive）经两条 job 队列交给两条 worker 线程（classify lane
+ drive lane）并发处理。见 docs/W2026-07-15_AgentRuntime_Eng_Design.md。

同一个 bot 同时只能跑一个入口（Telegram getUpdates 单消费者，双开会 409
Conflict）。取代了早期的单线程同步主循环 + 旧 router.SessionRouter。
"""
from __future__ import annotations

import argparse
import queue
import threading
import time
from pathlib import Path
from typing import Any, Tuple

from compose.adjustment_parser import (
    classify_cancel_confirmation,
    classify_collecting_message,
    classify_gate_reply,
    classify_running_message,
    classify_style_describe,
    classify_style_gate_reply,
    refine_plan_confirmation,
)
from compose.plan_composer import compose_plan
from orchestrator.driver import Driver
from pzt_client import PztClient
from session.consumer import SessionConsumer
from session.worker import SessionWorker
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from stages.style import StyleStage
from stages.style_apply_all import StyleApplyAllStage
from store.run_store import RunStore
from transport.telegram import TelegramTransport
from transport.telegram_client import chat_id_from_env, token_from_env


def build_runtime(state_dir: Path, transport: Any, chat_id: str,
                  idle_reminder_seconds: float = 300.0,
                  progress_interval_seconds: float = 60.0,
                  eval_poll_interval_seconds: float = 60.0,
                  ) -> Tuple[SessionConsumer, SessionWorker]:
    state_dir = Path(state_dir)
    store = RunStore(state_dir / "runs")
    marker_dir = state_dir / "delivered"
    staging_dir = state_dir / "staging"
    incoming_root = state_dir / "incoming"
    preview_root = state_dir / "preview"
    deliver_out_folder = state_dir / "deliver-out"
    for d in (incoming_root, preview_root, deliver_out_folder):
        d.mkdir(parents=True, exist_ok=True)

    # 两个 client 实例：worker 专属的挂可取消布防点；consumer 只读查询
    # （eval 进度轮询）用另一个，互不影响（Eng Design 第六节）。
    worker_client = PztClient()
    readonly_client = PztClient()

    stages = {
        "Ingest": IngestStage(client=worker_client),
        "Evaluate": EvaluateStage(client=worker_client),
        "Dedup": DedupStage(client=worker_client),
        "Curate": CurateStage(client=worker_client),
        "Style": StyleStage(client=worker_client),
        "StyleApplyAll": StyleApplyAllStage(client=worker_client),
        "Deliver": DeliverStage(client=worker_client, transport=transport,
                                 marker_dir=marker_dir, staging_dir=staging_dir,
                                 chat_id=chat_id, inputs=["StyleApplyAll"]),
    }
    driver = Driver(stages=stages, store=store)
    classify_jobs: "queue.Queue" = queue.Queue()
    drive_jobs: "queue.Queue" = queue.Queue()
    events: "queue.Queue" = queue.Queue()
    worker = SessionWorker(
        classify_jobs=classify_jobs, drive_jobs=drive_jobs, events=events,
        driver=driver, store=store, client=worker_client,
        transport=transport, chat_id=chat_id, preview_root=preview_root,
        compose_plan_fn=compose_plan,
        classify_collecting_message_fn=classify_collecting_message,
        classify_gate_reply_fn=classify_gate_reply,
        refine_plan_confirmation_fn=refine_plan_confirmation,
        classify_style_gate_reply_fn=classify_style_gate_reply,
        classify_running_message_fn=classify_running_message,
        classify_cancel_confirmation_fn=classify_cancel_confirmation,
        classify_style_describe_fn=classify_style_describe,
    )
    consumer = SessionConsumer(
        store=store, driver=driver, transport=transport, chat_id=chat_id,
        incoming_root=incoming_root, deliver_out_folder=deliver_out_folder,
        classify_jobs=classify_jobs, drive_jobs=drive_jobs, events=events,
        readonly_client=readonly_client,
        idle_reminder_seconds=idle_reminder_seconds,
        progress_interval_seconds=progress_interval_seconds,
        eval_poll_interval_seconds=eval_poll_interval_seconds,
        preview_root=preview_root, staging_dir=staging_dir, marker_dir=marker_dir,
    )
    return consumer, worker


def main() -> None:
    parser = argparse.ArgumentParser(description="Telegram 长驻会话 runner（consumer/worker 双线程）")
    parser.add_argument("--state-dir", help="agent 状态落盘目录，默认 ~/.pzt-agent")
    parser.add_argument("--poll-interval", type=float, default=0.5,
                         help="consumer 主循环节奏秒数（消息/事件/timers 的反应粒度）")
    parser.add_argument("--idle-reminder-seconds", type=float, default=300.0,
                         help="Collecting/Planned/AwaitingGate 静默多久后主动提醒一次，默认 300 秒")
    parser.add_argument("--progress-interval-seconds", type=float, default=60.0,
                         help="收图期间进度播报间隔秒数")
    parser.add_argument("--eval-poll-interval-seconds", type=float, default=60.0,
                         help="Evaluate 期间量化进度轮询/播报间隔秒数")
    args = parser.parse_args()

    state_dir = Path(args.state_dir) if args.state_dir else Path.home() / ".pzt-agent"
    token = token_from_env()
    chat_id = chat_id_from_env()
    transport = TelegramTransport(token=token, chat_id=chat_id,
                                   download_dir=state_dir / "telegram-inbox")
    consumer, worker = build_runtime(
        state_dir=state_dir, transport=transport, chat_id=chat_id,
        idle_reminder_seconds=args.idle_reminder_seconds,
        progress_interval_seconds=args.progress_interval_seconds,
        eval_poll_interval_seconds=args.eval_poll_interval_seconds,
    )

    stop_event = threading.Event()
    # 两条 lane 各一条线程：classify（纯 LLM，轻）和 drive（pzt 子进程，
    # 重）并发跑，处理中也能跑取消/进度的 LLM 分类（见 SessionWorker）。
    classify_thread = threading.Thread(target=worker.run_classify, args=(stop_event,),
                                        daemon=True, name="pzt-session-classify")
    drive_thread = threading.Thread(target=worker.run_drive, args=(stop_event,),
                                     daemon=True, name="pzt-session-drive")

    transport.start()
    classify_thread.start()
    drive_thread.start()
    consumer.bootstrap()  # RUNNING 自动续跑/遗留 AWAITING_REVIEW 收尾在这里
    print(f"Telegram runner 已启动，chat_id={chat_id}，state_dir={state_dir}，"
          f"poll_interval={args.poll_interval}s")
    try:
        while True:
            try:
                consumer.step()
            except Exception as e:
                # 单轮出错不拖死常驻进程（对齐旧主循环语义）：状态该落盘
                # 的都在各自边界落过了，这轮当作跳过。
                print(f"[run_telegram] consumer step 出错，已跳过：{e!r}")
            time.sleep(args.poll_interval)
    except KeyboardInterrupt:
        print("收到中断，正在停止…")
    finally:
        stop_event.set()
        # worker 若正卡在长 stage 里就不等它：daemon 线程随进程退出，run
        # 停在最后一次落盘的检查点，下次启动 bootstrap 自动续跑。
        classify_thread.join(timeout=2)
        drive_thread.join(timeout=2)
        transport.stop()


if __name__ == "__main__":
    main()
