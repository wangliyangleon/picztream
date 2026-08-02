# 12 - `rerun_stage` 答完的闸门没解除，之后任何一次调整都会把它再问一遍

**What to build:** 让"闸门已经问过、答案已经拿到"这件事在 `spec.gate` 上留下痕
迹，而不是只跳过这一次。

`orchestrator/driver.py::rerun_stage` 的 docstring 写的是"跳过 stage_name 自己
的闸门，直接用新 params 重跑它 …… 用于'闸门已经问过、调用方这次给的就是答案，
不需要闸门再问一遍'的场景"。它只兑现了**这一次**：`spec.gate` 原封不动。

于是在"只说去重没给数量"那条流程（`bare_compose_plan_deferred_curate`，Curate
`gate="required"`、`count=None`）里：

1. Dedup 跑完 → Curate 闸门弹出"去重后还剩 N 张，要不要再筛选一下？"
2. 用户答"留5张" → `rerun_curate` → `rerun_stage` 跳过闸门直接跑 Curate ✅
3. Curate 跑完 → Style 闸门（选片确认）弹出"选好了 5 张"
4. 用户在这里做**任何一次**调整（"换掉第3张"/"留3张"/"标签叫ins"/票 11 的
   "要活泼一点的"）→ `apply_adjustment` 把 Curate 连同下游重置成 PENDING
5. 下一次 `advance()` 看到 Curate 是 PENDING 且 `gate="required"`，于是**又把第
   1 步那句问出来** - 用户刚说完"要活泼一点的"，收到的是"去重后还剩 N 张，要不
   要再筛选一下？"，而 Curate 根本没重跑

实测（`agent/tests/orchestrator/test_driver_adjustment.py::
test_rerun_stage_leaves_its_own_gate_armed_so_a_later_adjustment_reasks` 钉住了当
前行为）：`apply_adjustment` + `advance()` 之后 `run.status ==
AWAITING_GATE`、`gate_state.stage_name == "Curate"`、Curate 的调用次数仍是 1。

**这不是票 11 引入的**，对 `set_count`/`set_apply_tag`/`swap_out` 三个既有 action
一视同仁，已经随现有版本分发出去了。票 11 只是让它多了一个触发入口（改题材要
求），并因此把它撞了出来。

**为什么另一条流程没事：** 用户一开始就给了数量时（`bare_compose_plan`），
`consumer._apply_confirmed_plan_params` 有一行 `curate.gate = "off"`，注释写着
"提前给了数量 = 提前回答了 Dedup 后才会问的追问，解除 Curate 的待定状态（目标
三决策七）"。那条路径已经做对了这件事，`rerun_curate` 这条漏了同一步。

**Blocked by:** 无

**Status:** ready-for-agent

## 开工前该知道的事

**一、别直接在 `rerun_stage` 里无条件 `spec.gate = "off"`。** 它有两个调用方，
另一个是 `rerun_style`，而 Style 的闸门必须留着：描述没匹配上任何 preset 时
`worker.py` 会 `rearm_gate(run, "Style")` 用 `spec.gate` 重新挂回去（AG-01），
关掉它那条重问路径就断了。

**二、`rearm_gate` 是第二个要一起想的地方。** 它构造 `GateState(setting=
spec.gate)`。票 10 的"停下"路径上 `consumer.py` 会 `rearm_gate(run, "Curate")`
（`_on_run_rewound`，回到"要不要用 AI"那一步）。如果 Curate 的 `gate` 此时已被
解除成 `"off"`，这里挂出来的 `GateState` 的 setting 就是 `"off"`。现读下来
`resolve_gate` 不看 setting、`timeout_gate` 只认 `"courtesy"`，似乎无实际影响，
但这条要在实现时确认，不能靠"看起来没事"。

**三、判据是"这道闸门要问的东西已经有答案了吗"，不是"跑过了吗"。** Curate 的
deferred 闸门问的是"留几张"，`count` 一旦确定它就永远不该再问；Style 的闸门问
的是"要什么风格"，而它在匹配失败时**确实需要**再问一次。两者的区别不在
`rerun_stage` 这一层，所以解除动作大概率该由调用方显式表达（一个opt-in 参数，
或者由 `rerun_curate` 那条分支自己动手），而不是 `rerun_stage` 替所有人决定。

## 验收标准

- [ ] deferred 流程（只说去重没给数量）里，答完"留5张"之后在选片确认闸门上做
      调整，Curate 真的重跑并回到选片确认，不再弹出"要不要再筛选一下"
- [ ] 上面这条对 `set_count`/`set_apply_tag`/`swap_out`/`set_selection_brief`
      四个 action 都成立
- [ ] Style 匹配失败后的重问路径（AG-01）行为不变，有测试钉住
- [ ] 票 10 的"停下"→`rearm_gate(Curate)` 路径行为不变，且 `GateState.setting`
      的取值经过确认而不是想当然
- [ ] `test_rerun_stage_leaves_its_own_gate_armed_so_a_later_adjustment_reasks`
      被有意翻转成新行为（它现在钉的是缺陷本身）
