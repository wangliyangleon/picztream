#!/usr/bin/env python3
"""Telegram 长驻会话 runner(子增量 F1)：单聊天白名单，轮询式主循环，
每条入站消息交给 SessionRouter 分类处理。跟 run_watchfolder.py(子增
量 D 固定 Plan)/run_intent.py(子增量 E 阻塞 input() 循环)是并行入
口，互不修改；F1 用后台线程+队列换掉了 input() 那种"一个进程只服务
一次交互"的形状，改成常驻轮询。
"""
from __future__ import annotations

import argparse
import time
from pathlib import Path
from typing import Any

from compose.adjustment_parser import parse_adjustment
from compose.plan_composer import compose_plan
from orchestrator.driver import Driver
from pzt_client import PztClient
from router.session_router import SessionRouter
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from store.run_store import RunStore
from transport.telegram import TelegramTransport
from transport.telegram_client import chat_id_from_env, token_from_env


def build_router(state_dir: Path, client: PztClient, transport: Any, chat_id: str) -> SessionRouter:
    state_dir = Path(state_dir)
    store = RunStore(state_dir / "runs")
    marker_dir = state_dir / "delivered"
    staging_dir = state_dir / "staging"
    incoming_root = state_dir / "incoming"
    preview_root = state_dir / "preview"
    deliver_out_folder = state_dir / "deliver-out"
    for d in (incoming_root, preview_root, deliver_out_folder):
        d.mkdir(parents=True, exist_ok=True)

    stages = {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=marker_dir,
                                 staging_dir=staging_dir, chat_id=chat_id),
    }
    driver = Driver(stages=stages, store=store)
    return SessionRouter(
        store=store, driver=driver, transport=transport, client=client, chat_id=chat_id,
        incoming_root=incoming_root, preview_root=preview_root, deliver_out_folder=deliver_out_folder,
        compose_plan_fn=compose_plan, parse_adjustment_fn=parse_adjustment,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Telegram 长驻会话 runner（子增量 F1）")
    parser.add_argument("--state-dir", help="agent 状态落盘目录，默认 ~/.pzt-agent")
    parser.add_argument("--poll-interval", type=float, default=1.5, help="receive() 轮询间隔秒数")
    args = parser.parse_args()

    state_dir = Path(args.state_dir) if args.state_dir else Path.home() / ".pzt-agent"
    token = token_from_env()
    chat_id = chat_id_from_env()
    client = PztClient()
    transport = TelegramTransport(token=token, chat_id=chat_id, download_dir=state_dir / "telegram-inbox")
    router = build_router(state_dir=state_dir, client=client, transport=transport, chat_id=chat_id)

    transport.start()
    print(f"Telegram runner 已启动，chat_id={chat_id}，state_dir={state_dir}")
    try:
        while True:
            for msg in transport.receive():
                print(f"[收到消息] kind={msg.kind} text={msg.text!r} file_path={msg.file_path!r}")
                run = router.handle_message(msg)
                if run is not None:
                    print(f"  -> run_id={run.run_id} status={run.status.value}")
            time.sleep(args.poll_interval)
    except KeyboardInterrupt:
        print("收到中断，正在停止…")
    finally:
        transport.stop()


if __name__ == "__main__":
    main()
