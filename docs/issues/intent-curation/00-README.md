# 意图驱动的跨簇选片 - 任务拆解

- **对应 PRD**：`docs/Intent_Curation_PRD.md`
- **相关 ADR**：`docs/adr/0001-core-hosts-photo-reasoning-even-when-text-only.md`
- **术语**：见 `CONTEXT.md`
- **拆法**：tracer-bullet 纵切，每张票自带阻塞边。一票一文件，编号即依赖序（阻塞者在前）。

## 一览

| # | 标题 | 阻塞于 | 状态 |
|---|---|---|---|
| [01](01-chronological-order-when-ai-off.md) | AI 关时交付按拍摄时间排序 | 无 | **done** (`158ee3a`) |
| [02](02-consolidate-intent-hint-text.md) | 引导语收成单一常量 | 无 | **done** (`bc778e3`) |
| [03](03-content-field-in-description.md) | 描述新增 `content` 字段 | 无 | **done** (`dc91dc7`..`6ee4baa`) |
| [04](04-preselection-clamp-and-m-knob.md) | 预选集裁剪与 M 旋钮 | 无 | **done** (`904ba3a`..`35dcb92`) |
| [05](05-evaluate-preselection-gate-progress.md) | curate 内部评估预选集：闸门 + 进度 + 偿还语义折叠 | 03, 04 | **done** |
| [06](06-model-selection-and-ordering.md) | 模型选择与排序接进 curate | 03, 04, 05 | **done** |
| [07](07-caption-end-to-end.md) | 文案端到端 | 06 | ready-for-agent |
| [08](08-selection-brief-plumbing.md) | 选片简述贯通 | 02, 06 | **done** (`edfcf2e`..`a841aad`) |
| [09](09-agent-progress-rendering.md) | agent 侧进度渲染 | 05 | **done** (`1f34fb0`..`95cfb37`) |
| [10](10-headless-gate-and-cancel-wiring.md) | headless 的开销闸门与取消接线 | 06 | **done** |
| [11](11-adjust-selection-brief-at-gate.md) | 选片确认阶段能改题材要求 | 08 | 已实现，**待真机收口** |
| [12](12-rerun-stage-leaves-its-gate-armed.md) | `rerun_stage` 答完的闸门没解除 | 无 | **done** |
| [13](13-adjust-selection-brief-at-plan-confirmation.md) | 方案确认阶段也能改题材要求 | 无 | **done** |

## 依赖图

```
01  (游离)                                  12 (已 done) ─→ 11 收口
02 ────────────────────────┐
03 ──┬──────┐              │
04 ──┴─→ 05 ─┴─→ 06 ──┬─→ 07
         └─→ 09        ├─→ 08 ←┘(02) ─→ 11
                       └─→ 10
```

**唯一还没开工的是 07**，它的阻塞边（06）早已解开。

## 剩余的开工次序

只剩 **07**（未开工）与 **11 的真机收口**（无代码工作）。两者互不相干。

- **07** 的阻塞边（06）已经解开，可以开工。它与已收口的 08 都改
  `core/ai/selection.{h,cpp}`：08 只往提示词里加输入，07 要动返回结构与本地
  的约束解码 schema，两张并行必冲突，所以 08 先走完了。
- **11** 已于 2026-08-02 实现（拍板 + 落地同日）。两块板：`exclude` 在简述变更
  时保留、新简述整体替换旧简述（**决策二已于 2026-08-03 真机后修订为"让模型决
  定"**），理由见票内"拍板"一节；落地方式与两个 Eng Design 问题的答案见票内
  "落地记录"，真模型验证结果见"真模型结果"。票 12 落地后第一条验收的 deferred
  流程阻塞已解除，**只剩"重选只花一次文本调用、不重跑评估"待真机验**（本票无对
  应代码，验的是票 05 的评估缓存），验过即可标 done。
- **13** 是票 11 真机验证时打出来的：票 11 只堵了**选片确认**（`AWAITING_GATE`）
  那个入口，用户更早在**方案确认**（`PLANNED`，走 `refine_plan_confirmation`）
  就会想改题材要求，而那条路的可调字段里没有 `selection_brief`，于是"小清新"被
  硬塞进 `apply_tag` / `ai_enabled`。票 11 拍的两块板直接沿用，没有新的板要拍。
- **12** 是票 11 落地时发现的既有缺陷，不是票 11 引入的，**已于 2026-08-03 收口**：
  `rerun_stage` 答完的闸门不解除，于是"只说去重没给数量"那条流程里，选片确认闸门
  上的任何一次调整都会把"要不要再筛选一下"再问一遍、Curate 不重跑。修法是新加
  `StageSpec.gate_answered`、**不动 `spec.gate`** - 后者被 consumer 的 rewind 路
  径和 `rearm_gate` 当判据读，写坏它会静默改掉票 10 的"停下"行为。解除动作做成
  调用方 opt-in（`rerun_stage(..., mark_gate_answered=True)`），`rerun_style` 保持
  默认，AG-01 一个字没改。详见票内"落地记录"。
- **10** 已于 2026-08-02 完全收口（闸门在 headless 上从"阻塞式确认"改成"告知 + 随时可撤"，PRD 的 G5/决策十八已按**入口**改述）。`ai_declined`/`cancelled` 按拍板决策三**永久不进** headless JSON。Telegram 端到端真机验收已通过。

## 一条必须遵守的顺序约束

**03 必须先于 05 完成，这是正确性约束而非偏好。**

PRD 决策七定的缓存判据是"有评估记录就跳过"。若 05（curate 内部跑评估）先落地，那批照片会写入**没有 `content` 的评估记录**，而这些记录之后**永远不会被刷新** - 03 落地之后它们照样命中缓存。顺序颠倒会留下一批哑数据，且不报错。
