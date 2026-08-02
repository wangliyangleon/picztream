"""SessionView：consumer 私有的会话内存视图（docs/W2026-07-15_AgentRuntime_
Eng_Design.md 第四节）。它回答"现在到哪一步/进度多少"，让 consumer 在
worker 独占 RunState 期间（DriveJob 活跃）也能秒级应答——**不是**持久
真相，重启从 RunStore 重建，平时被 worker 事件更新。

photo_count 故意不做成字段：照片数的真相是 incoming 目录本身，惰性现算
（旧 router 同款），缓存进字段只会跟目录漂移。用户设想的
ai_eval_in_progress 这类步骤枚举也不单独建：(status, current_stage,
stage_progress) 组合已完整表达，平行枚举只会漂移。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Tuple

from orchestrator.types import RunState, RunStatus
from router.collecting import incoming_dir_for

# stage 运行前发给用户的"正在…"进度文案。Style/StyleApplyAll 故意缺席：它们是
# required 闸门，advance() 会在运行前停下问人，"正在自动套用风格..."紧
# 贴着闸门自己的提问会自相矛盾，闸门消息本身就是恰当的进度提示。
STAGE_PROGRESS_MESSAGES = {
    "Ingest": "正在导入照片...",
    "Dedup": "正在执行去重...",
    "Curate": "正在筛选...",
    "Deliver": "正在交付...",
}

# 进度的整句措辞，按"数的是什么"分（orchestrator.stage.PROGRESS_*），不按
# stage 分：同一个 Curate，本地分簇阶段数的是候选簇、AI 阶段数的是比较次
# 数，共用一句话必然有一边说错。prose 全部落在这里，orchestrator 与
# stages 只传语义 key。
_PROGRESS_PHRASINGS = {
    "groups": ("正在处理需要比较筛选的照片组", "组"),
    "comparisons": ("正在两两比较、挑出更好的那张", "次"),
    "photos": ("正在套滤镜", "张"),
    "evaluations": ("正在逐张看照片、记下画面内容与优缺点", "张"),
}

# 同一条进度消息的终态措辞（真机反馈）。跑完还停在"正在…"上的话，用户看
# 到的最后一条永远是未完成态，然后下一件事无预警发生。
_PROGRESS_DONE_PHRASINGS = {
    "groups": "照片组都处理完了，共 {n} 组",
    "comparisons": "两两比较跑完了，共 {n} 次",
    "photos": "滤镜都套好了，共 {n} 张",
    "evaluations": "照片都看完了，共 {n} 张",
}


def describe_progress_done(kind: str, total: int) -> str:
    return _PROGRESS_DONE_PHRASINGS.get(kind, "处理完了，共 {n} 项").format(n=total)


# 取消时"已经落地了多少"的措辞，同样按"数的是什么"分（票 10 决策四）。
# 两条说的是两件不同的事：滤镜留在照片上，评估留在库里且下次还能省钱 - 
# 后半句是必须说的，不然用户会以为那几次调用白花了。分簇/比较不在表里，
# 它们的取消是零写入，走裸回执。
_CANCEL_PARTIAL_PHRASINGS = {
    "photos": "已经给 {done}/{total} 张套上滤镜了，这部分保留",
    "evaluations": "已经看完 {done}/{total} 张，这部分评价留在库里了，下次不用重看",
}


def describe_cancel_partial(kind: str, done: int, total: int) -> Optional[str]:
    """取消回执里那句括号内容，没有对应措辞就 None（调用方发裸回执）。"""
    phrasing = _CANCEL_PARTIAL_PHRASINGS.get(kind)
    return None if phrasing is None else phrasing.format(done=done, total=total)


# 本地模型一次视觉调用的实测量级（票 10 拍板时记的真机数据：40 张照片、
# 本地 Ollama，一次约 40 秒）。**只给 local 一个数**：云端 provider 每次
# 调用要多久没有实测过，而在一条"接下来要花多少"的消息里编一个数字，是让
# 用户从此不再信这条消息的最快办法。云端那条路只报次数，不报时长。
_LOCAL_SECONDS_PER_AI_CALL = 40


def describe_ai_cost(comparisons: int, evaluations: int, provider: str,
                      first: bool) -> Optional[str]:
    """AI 开跑之前那条开销告知（票 10 决策一）。

    它不是"要不要跑"的提问 - headless 那一侧 core 报完数字就继续跑了，
    这条消息的作用是让用户**知道**接下来几分钟要花什么，并且知道可以停。
    所以措辞是陈述 + 一句"想停就说"，不是"要跑吗？"。

    两个数分别措辞、不合成一个总数：单位不同（次比较 / 张评估），说成
    "24 项"用户读不出来那是什么（同 T-8 真机验收推翻"把 phase 压掉"的理
    由）。

    first=False 时改成接续口吻：真机上 Dedup 先报一次、Curate 再报一次，
    照抄第一条会读成"怎么又要跑一遍"，而它其实是同一笔账的后半段（决策
    五：开销这件事对用户是一笔账，不是两笔）。

    两个数都是 0 时返回 None（没有 AI 调用要发，core 也不会报），由调用
    方整条消息都不发 - 翻译成一句"接下来要跑 0 次"毫无意义。
    """
    parts = []
    if comparisons > 0:
        parts.append(f"两两比较 {comparisons} 次")
    if evaluations > 0:
        parts.append(f"逐张看 {evaluations} 张照片")
    if not parts:
        return None

    what = "、".join(parts)
    head = f"接下来 AI 要{what}" if first else f"接着还要{what}"

    calls = comparisons + evaluations
    if provider == "local":
        minutes = max(1, round(calls * _LOCAL_SECONDS_PER_AI_CALL / 60))
        head += f"，大概 {minutes} 分钟"

    # 写明"停下之后回到哪儿"（真机反馈 2026-08-02）。用户点这个按钮的动机
    # 是"别用 AI 跑这一步"，不是"这批不要了" - 不写清楚的话，它读起来像个
    # 会炸掉整批的红色按钮，而实际上代价只是回到刚才那个问题。两条消息都
    # 要写：第二条（选片）跟第一条（去重）在这件事上没有区别。
    tail = ("。想停就点下面的按钮，会回到\u201c要不要用 AI\u201d那一步，已经跑完的不白费"
            if first else "。想停一样点下面的按钮，回到\u201c要不要用 AI\u201d那一步")
    return head + tail


@dataclass
class SessionView:
    incoming_root: Path
    run_id: Optional[str] = None
    project_id: Optional[str] = None
    status: Optional[RunStatus] = None
    current_stage: Optional[str] = None            # StageStarted 事件更新
    stage_progress: Optional[Tuple[int, int, str]] = None  # (done, total, kind)
    gate_stage: Optional[str] = None               # GateReached 事件更新
    plan_summary: Optional[dict] = None            # count/apply_tag/ai_enabled/provider
    selected_count: Optional[int] = None           # GateReached payload / 重建时从 outputs 抄
    drive_active: bool = False                     # DriveJob 入队 True，闸门/终态事件 False

    def photo_count(self) -> int:
        if self.run_id is None:
            return 0
        return len(list(incoming_dir_for(self.incoming_root, self.run_id).iterdir()))

    def describe(self) -> str:
        # COLLECTING/PLANNED/AWAITING_GATE 三条逐字对齐旧
        # _status_snapshot_text；RUNNING 是 2.0 新增分支（旧实现跑批期间
        # 根本收不到消息，没有这个应答场景）。
        if self.status == RunStatus.COLLECTING:
            return f"目前收到 {self.photo_count()} 张照片，还没告诉我想怎么处理"
        if self.status == RunStatus.PLANNED and self.plan_summary is not None:
            if self.plan_summary.get("count") is None:
                # deferred 形状（W2026-07-21 目标三案例二）：Curate 数量待
                # 定，这一步只说"去重"，不预告后面还要问什么（真机反馈）。
                return (f"目前收到 {self.photo_count()} 张照片，方案是："
                        f"先帮你去重，"
                        f"标签叫\"{self.plan_summary['apply_tag']}\"")
            # 措辞与 consumer._send_plan_confirmation 的确认文案对齐：状态
            # 查询是用户核对选片简述的第二个入口（确认那条消息可能已经被
            # 后面几十条进度顶上去了），两处说法不一致会让用户以为方案变
            # 过。两处仍是各自的字符串 - 前缀不同（"方案是："vs"理解你想："）、
            # 结尾不同（这里不带按钮提示），能共享的只有中间这半句。
            # 关 AI 时不说"按拍摄时间"，理由同 consumer._send_plan_confirmation。
            picker = "使用AI帮你选择" if self.plan_summary.get("ai_enabled") else "帮你选择"
            brief = self.plan_summary.get("selection_brief", "")
            subject = f"{brief}的照片" if brief else "照片"
            return (f"目前收到 {self.photo_count()} 张照片，方案是："
                    f"{picker} {self.plan_summary['count']} 张{subject}，"
                    f"标签叫\"{self.plan_summary['apply_tag']}\"")
        if self.status == RunStatus.AWAITING_GATE:
            if self.gate_stage == "Curate":
                return "去重完了，等你说要不要再筛选一下"
            # 票 11：跟 consumer._render_selection_confirm_gate 同一句措辞。
            # 简述在这个闸门上是可改的（"要活泼一点的"就地重选），改完必须
            # 到处都看得见此刻生效的是哪一句 - 两处不一致会读成方案又变过，
            # 同 PLANNED 分支上面那条注释的理由。
            brief = (self.plan_summary or {}).get("selection_brief", "")
            picked = f"按你说的「{brief}」选好了" if brief else "选好了"
            return f"已经{picked} {self.selected_count or 0} 张，等你回复"
        if self.status == RunStatus.RUNNING:
            base = STAGE_PROGRESS_MESSAGES.get(self.current_stage or "", "正在处理...")
            if self.stage_progress is None:
                return base.rstrip(".")
            done, total, kind = self.stage_progress
            # 有进度时整句都由 kind 决定，不拼在 stage 的通用句子后面：数
            # 的东西不同，前半句也该不同。"正在筛选，已完成 1/1 张"是真机
            # 验收踩到的原话 —— 用户要选 3 张，那个 1/1 其实是 1 个候选簇，
            # 单位和主语双错。
            phrase, unit = _PROGRESS_PHRASINGS.get(kind, (base.rstrip("."), "张"))
            return f"{phrase}，已完成 {done}/{total}{unit}"
        return "没什么可说的"


def view_from_run(run: RunState, incoming_root: Path) -> SessionView:
    """启动恢复用：能从落盘 RunState 抄的都抄上。current_stage/进度抄不
    到（那是 drive 过程中的瞬时态，不落盘），留空等续跑事件重新填。"""
    view = SessionView(incoming_root=Path(incoming_root), run_id=run.run_id,
                       project_id=run.project_id, status=run.status)
    curate = next((s for s in run.plan.stages if s.name == "Curate"), None)
    if curate is not None:
        view.plan_summary = {
            "count": curate.params.get("count"),
            "apply_tag": curate.params.get("apply_tag"),
            "ai_enabled": curate.params.get("ai_enabled"),
            # 归一成空串而不是 None：describe() 只判真假，但 plan_summary
            # 也进 dict 比较的测试断言，两种"没有简述"的表示会让它时灵时不灵。
            "selection_brief": curate.params.get("selection_brief") or "",
            # 票 10：开销告知要按 provider 决定报不报时长。跟
            # _current_plan_params 保持同一组 key，两处构造出来的
            # plan_summary 形状不该有差别。
            "provider": curate.params.get("provider", "local"),
        }
    curate_output = run.outputs.get("Curate")
    if curate_output is not None:
        view.selected_count = len(curate_output.data.get("selected", []))
    if run.gate_state is not None:
        view.gate_stage = run.gate_state.stage_name
    return view
