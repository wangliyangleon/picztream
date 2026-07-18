"""consumer <-> worker 的消息协议（docs/W2026-07-15_AgentRuntime_Eng_Design.md
第三节）。全部是扁平 dataclass，不搞继承——Job 只有三种、字段各不相同，
基类除了共享 generation 什么都省不下来，还会踩 dataclass 默认值排序的坑。

generation 语义：consumer 维护会话代数，每次取消生效或 run 终结时 +1；
事件带着投递时的 generation 原样回来，consumer 丢弃过期事件——这是
"classify 还在跑、用户已取消，结果回来把新会话搞脏"这类陈旧回调问题的
唯一防线，worker 自己不判断新旧。
"""
from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Any, Optional

# -- jobs (consumer -> worker) --


@dataclass
class ClassifyJob:
    generation: int
    kind: str  # "collecting" | "gate_reply" | "refine_plan"
    text: str
    context: dict = field(default_factory=dict)
    # collecting: {photo_count}; gate_reply: {run_id};
    # refine_plan: {intent_raw, current_params}


@dataclass
class ComposeJob:
    generation: int
    intent_text: str


@dataclass
class DriveJob:
    generation: int
    action: str  # "start" | "resume" | "resolve_gate" | "adjustment" | "rerun_style"
    run_id: str
    args: dict = field(default_factory=dict)
    # 取消 = consumer set()；worker 在 stage 边界必查，并在可杀 stage
    # （Evaluate/Dedup）期间把它挂到 PztClient 上做子进程级终止。
    cancel_event: threading.Event = field(default_factory=threading.Event)


# -- events (worker -> consumer) --


@dataclass
class ClassifyDone:
    generation: int
    kind: str
    result: Any  # 对应 compose/adjustment_parser.py 各分类函数的返回对象


@dataclass
class ClassifyFailed:
    generation: int
    kind: str
    # AdjustmentError（没听懂）-> False；LlmRequestError（基础设施故障，
    # collecting 态按旧行为降级为直接当意图）-> True。
    retryable: bool


@dataclass
class ComposeDone:
    generation: int
    plan: Any  # 已通过 validate_plan 的 Plan


@dataclass
class ComposeFailed:
    generation: int
    message: str


@dataclass
class StageStarted:
    generation: int
    run_id: str
    stage: str


# 注意没有 StageProgress 事件：Evaluate 的量化进度是 consumer 侧轮询
# `pzt images --json` 得来的（worker 阻塞在 eval 子进程里报不了）；其余
# stage 秒级不需要。视图的 stage_progress 字段由 consumer 自己填。


@dataclass
class GateReached:
    generation: int
    run_id: str
    stage: str
    payload: dict = field(default_factory=dict)


@dataclass
class RunFinished:
    generation: int
    run_id: str
    status: str  # RunStatus.value: "done" | "failed" | "cancelled"
    detail: Optional[str] = None


@dataclass
class JobCrashed:
    generation: int
    lane: str  # "classify" | "drive"——只清崩掉那条 lane 的状态，另一条不受牵连
    error: str
