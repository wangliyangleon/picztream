from __future__ import annotations

from typing import Callable, Optional

from .stage import Stage, StageContext, _noop_cost, _noop_progress
from .types import (
    GateState,
    Plan,
    PlanDelta,
    RunState,
    RunStatus,
    StageSpec,
    StageStatus,
)


class Driver:
    """纯确定性循环：取 Stage -> 查依赖 -> 过闸门 -> 调 Stage.run -> 存状态。
    挂不挂闸门、超时怎么处理，全部来自 Plan 里已经定好的 StageSpec 参数，
    Driver 不做任何"决定"，零 LLM。
    """

    def __init__(self, stages: dict[str, Stage], store) -> None:
        self.stages = stages
        self.store = store
        # 布防点：worker 在执行 DriveJob 期间挂上、结束时摘掉，跟
        # PztClient.cancel_event 同一个套路 —— 挂在实例上而不是 advance()
        # 的参数，stages 内部的 ctx.on_progress 才能零改动吃到上报能力。
        # Driver 不关心进度送去哪，只负责绑上 stage 名再交给 StageContext。
        self.progress_sink: Optional[Callable[[str, int, int, str], None]] = None
        # 同一个套路的第二条（票 10）：AI 开跑前的精确开销。跟进度分开是因
        # 为下游处置不同 - 进度节流后原地编辑一条消息，开销要立刻新发一条
        # 并带上可取消入口。
        self.cost_sink: Optional[Callable[[str, int, int], None]] = None

    def advance(self, run: RunState) -> RunState:
        if run.status in (RunStatus.DONE, RunStatus.FAILED, RunStatus.CANCELLED):
            return run
        if run.status == RunStatus.AWAITING_GATE:
            return run

        next_spec = self._next_pending(run)
        if next_spec is None:
            run.status = RunStatus.AWAITING_REVIEW
            self.store.save(run)
            return run

        if next_spec.gate != "off" and run.gate_state is None:
            run.gate_state = GateState(stage_name=next_spec.name, setting=next_spec.gate)
            run.status = RunStatus.AWAITING_GATE
            self.store.save(run)
            return run

        self._run_stage(run, next_spec)
        run.gate_state = None
        self.store.save(run)
        return run

    def peek_next_spec(self, run: RunState) -> StageSpec | None:
        """无副作用地查询接下来会被 advance() 选中的 stage 的 spec，供 worker
        在运行前判断"这一步会真运行还是会停在闸门"（要读 spec.gate）。复用
        _next_pending，不重复实现一遍。"""
        return self._next_pending(run)

    def peek_next_stage(self, run: RunState) -> str | None:
        """peek_next_spec 的薄壳，只取名字。"""
        spec = self.peek_next_spec(run)
        return spec.name if spec else None

    def resolve_gate(self, run: RunState, decision: str) -> RunState:
        if run.gate_state is None:
            raise ValueError("no gate is pending on this run")
        if decision == "hold":
            run.gate_state.decision = "hold"
            run.status = RunStatus.CANCELLED
            self.store.save(run)
            return run
        run.gate_state.decision = "proceed"
        run.status = RunStatus.RUNNING
        spec = self._spec_by_name(run, run.gate_state.stage_name)
        self._run_stage(run, spec)
        run.gate_state = None
        self.store.save(run)
        return run

    def rearm_gate(self, run: RunState, stage_name: str) -> RunState:
        """把一个已经运行过、但需要重新征询用户的 stage 重新挂回它的闸门。
        当前用于 Style：描述没匹配上任何 preset 时退回去重新问（AG-01）——
        stage 已 DONE，但 rerun_stage 直接调 _run_stage、不看 stage 状态，
        所以重挂闸门后用户再给新描述仍能重跑。"""
        spec = self._spec_by_name(run, stage_name)
        run.gate_state = GateState(stage_name=stage_name, setting=spec.gate)
        run.status = RunStatus.AWAITING_GATE
        self.store.save(run)
        return run

    def timeout_gate(self, run: RunState) -> RunState:
        if run.gate_state is None or run.gate_state.setting != "courtesy":
            raise ValueError("no courtesy gate is pending on this run")
        spec = self._spec_by_name(run, run.gate_state.stage_name)
        return self.resolve_gate(run, spec.gate_on_timeout)

    def approve(self, run: RunState) -> RunState:
        if run.status != RunStatus.AWAITING_REVIEW:
            raise ValueError("run is not awaiting review")
        run.status = RunStatus.DONE
        self.store.save(run)
        return run

    def cancel(self, run: RunState) -> RunState:
        run.status = RunStatus.CANCELLED
        self.store.save(run)
        return run

    def rerun_stage(self, run: RunState, stage_name: str, params: dict) -> RunState:
        """跳过 stage_name 自己的闸门，直接用新 params 重跑它，并把它的下
        游重置成 PENDING——用于"闸门已经问过、调用方这次给的就是答案，不
        需要闸门再问一遍"的场景（比如 Style 闸门首次拿到风格描述、或者
        StyleApplyAll 闸门被拒绝后用新描述重新挑风格）。跟 apply_adjustment
        的区别：apply_adjustment 只重置状态、把 gate_state 清空，下一次
        advance() 发现目标 stage 又是 PENDING 且带闸门，会重新触发一次闸
        门；rerun_stage 直接调 _run_stage，不经过 advance() 的闸门检查。
        """
        spec = self._spec_by_name(run, stage_name)
        spec.params.update(params)

        for name in self._downstream_of(run.plan, stage_name):
            run.stage_states[name] = StageStatus.PENDING
            run.outputs.pop(name, None)

        run.gate_state = None
        run.status = RunStatus.RUNNING
        self._run_stage(run, spec)
        self.store.save(run)
        return run

    def rewind_stage(self, run: RunState, stage_name: str) -> RunState:
        """把一个被中途叫停的 stage 退回未运行，连同它的下游；上游不动。

        跟 cancel 的区别是这个 run 还活着 - 用户叫停的是"这一步以当前的
        方式跑完"，不是整批。跟 rerun_stage 的区别是它**不立刻重跑**：退
        回 PENDING 之后就停在这里，等调用方重新征询一次再推进（真机反馈
        2026-08-02：停下之后要回到"要不要用 AI"那一步）。

        status 落在 PLANNED 而不是 RUNNING：run 此刻确实在等人回话，留成
        RUNNING 会让 bootstrap 的自愈把它当成"崩在半路"接着往下跑。要停
        在闸门上的那种形状由调用方随后自己 rearm_gate。
        """
        run.stage_states[stage_name] = StageStatus.PENDING
        run.outputs.pop(stage_name, None)
        for name in self._downstream_of(run.plan, stage_name):
            run.stage_states[name] = StageStatus.PENDING
            run.outputs.pop(name, None)
        run.gate_state = None
        run.status = RunStatus.PLANNED
        self.store.save(run)
        return run

    def apply_adjustment(self, run: RunState, delta: PlanDelta) -> RunState:
        spec = self._spec_by_name(run, delta.stage_name)
        spec.params.update(delta.params)

        for name in self._downstream_of(run.plan, delta.stage_name):
            run.stage_states[name] = StageStatus.PENDING
            run.outputs.pop(name, None)

        run.gate_state = None
        run.status = RunStatus.RUNNING
        self.store.save(run)
        return run

    # -- internal --

    def _next_pending(self, run: RunState) -> StageSpec | None:
        for spec in run.plan.stages:
            if run.stage_states.get(spec.name) != StageStatus.PENDING:
                continue
            stage = self.stages[spec.name]
            unmet = [
                d for d in stage.inputs
                if run.stage_states.get(d) not in (StageStatus.DONE, StageStatus.SKIPPED)
            ]
            if unmet:
                raise RuntimeError(
                    f"stage {spec.name!r} is next in Plan order but its inputs {unmet} "
                    "are not resolved yet -- Plan is not topologically ordered"
                )
            return spec
        return None

    def _progress_for(self, stage_name: str):
        """每次 _run_stage 现绑，不在 Driver 上缓存 —— 同一个 sink 服务整
        条 Plan，缓存住的话第二个 stage 的进度会顶着第一个的名字上报。"""
        sink = self.progress_sink
        if sink is None:
            return _noop_progress
        return lambda done, total, kind: sink(stage_name, done, total, kind)

    def _cost_for(self, stage_name: str):
        """同 _progress_for：每次现绑。真机上 Dedup 与 Curate 各报一次，
        缓存住的话第二条会顶着第一个 stage 的名字。"""
        sink = self.cost_sink
        if sink is None:
            return _noop_cost
        return lambda comparisons, evaluations: sink(stage_name, comparisons, evaluations)

    def _spec_by_name(self, run: RunState, name: str) -> StageSpec:
        for spec in run.plan.stages:
            if spec.name == name:
                return spec
        raise KeyError(name)

    def _run_stage(self, run: RunState, spec: StageSpec) -> None:
        stage = self.stages[spec.name]
        run.stage_states[spec.name] = StageStatus.RUNNING
        ctx = StageContext(run_id=run.run_id, project_id=run.project_id, outputs=run.outputs,
                            on_progress=self._progress_for(spec.name),
                            on_cost=self._cost_for(spec.name))
        output = stage.run(ctx, spec.params)
        run.outputs[spec.name] = output

        if output.ok:
            run.stage_states[spec.name] = StageStatus.DONE
            run.status = RunStatus.RUNNING
            return

        if stage.criticality == "critical":
            run.stage_states[spec.name] = StageStatus.FAILED
            run.status = RunStatus.FAILED
        else:
            run.stage_states[spec.name] = StageStatus.SKIPPED
            run.status = RunStatus.RUNNING

    def _downstream_of(self, plan: Plan, stage_name: str) -> list[str]:
        names_in_plan = [s.name for s in plan.stages]
        affected = {stage_name}
        changed = True
        while changed:
            changed = False
            for name in names_in_plan:
                if name in affected:
                    continue
                stage = self.stages[name]
                if any(dep in affected for dep in stage.inputs):
                    affected.add(name)
                    changed = True
        return [n for n in names_in_plan if n in affected]
