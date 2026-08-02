"""consumer <-> worker 的消息协议（docs/history/W2026-07-15_AgentRuntime_Eng_Design.md
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
from typing import Any, Optional, Tuple

# -- jobs (consumer -> worker) --


@dataclass
class ClassifyJob:
    generation: int
    kind: str  # collecting | gate_reply | refine_plan | style_describe |
               # style_gate | dedup_followup | running | cancel_confirm
    text: str
    context: dict = field(default_factory=dict)
    # collecting: {photo_count}; gate_reply/style_describe/style_gate: {run_id};
    # refine_plan: {intent_raw, current_params}; dedup_followup: {remaining}


@dataclass
class ComposeJob:
    generation: int
    intent_text: str


@dataclass
class DriveJob:
    generation: int
    action: str  # "start" | "resume" | "resolve_gate" | "adjustment" | "rerun_style" | "rerun_curate"
    run_id: str
    args: dict = field(default_factory=dict)
    # 取消 = consumer set()；worker 在 stage 边界必查，并在可杀 stage
    # （见 session.worker.KILLABLE_STAGES）期间把它挂到 PztClient 上做子
    # 进程级终止。
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


# T-8：stage 运行到一半的进度。两个来源，事件本身不区分：
#   - 进程内产生的（StyleApplyAll 是 N 次子进程调用的 Python for 循环，
#     i/N 在循环里天然就有）；
#   - 跨进程解析出来的（dedup/curate 的 --ai 在 stderr 上逐行吐进度）。
# 尽力而为：丢一条不影响正确性，consumer 只拿它刷 SessionView 和播报。
# 节流在 consumer 侧（决策二），worker 收到多少发多少。
@dataclass
class StageProgress:
    generation: int
    run_id: str
    stage: str
    done: int
    total: int
    # 数的是什么（orchestrator.stage.PROGRESS_*）。跟着数字一起传，展示层
    # 才不用猜单位，见那边的说明。
    kind: str


# 票 10：这一趟 AI 开跑之前算出来的**精确**开销，一个 stage 最多一条。
# 它是 PRD G5（用户可以在 AI 开跑前拒绝）在 headless 上的全部兑现方式：
# core 那一侧的闸门不等人（Telegram 那头的用户不在同一个时间轴上），改成
# 报完数字继续跑，consumer 收到这条之后立刻告知用户并给可取消入口。
#
# 跟 StageProgress 分成两种事件而不是加个字段：consumer 对两者的处置完全
# 不同 - 进度节流后原地编辑同一条消息，开销必须立刻新发一条独立消息，晚
# 一分钟就失去意义。
@dataclass
class StageCost:
    generation: int
    run_id: str
    stage: str
    comparisons: int  # 两两比较次数（dedup 的锦标赛 / curate 的簇内锦标赛）
    evaluations: int  # 逐张评估的照片数（只有 curate 有，dedup 恒为 0）


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
    # T-8 决策五：取消时已经落地的部分成果 (stage, done, total)。只对写入
    # 是逐张的 stage 有值（见 worker.PARTIAL_ON_CANCEL_STAGES）；dedup/
    # curate 的取消按 core 契约一定是零写入，报"已经处理了 N 张"是主动
    # 误导，所以那些路径恒为 None。文案由 consumer 渲染，这里只给数据。
    cancelled_partial: Optional[Tuple[str, int, int]] = None


@dataclass
class JobCrashed:
    generation: int
    lane: str  # "classify" | "drive"——只清崩掉那条 lane 的状态，另一条不受牵连
    error: str
