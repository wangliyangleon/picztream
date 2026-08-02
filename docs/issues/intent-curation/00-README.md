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
| [11](11-adjust-selection-brief-at-gate.md) | 选片确认阶段能改题材要求 | 08 | **done**（两条待真机验）|
| [12](12-rerun-stage-leaves-its-gate-armed.md) | `rerun_stage` 答完的闸门没解除 | 无 | ready-for-agent |

## 依赖图

```
01  (游离)                                       12 (游离，票 11 落地时发现)
02 ────────────────────────┐
03 ──┬──────┐              │
04 ──┴─→ 05 ─┴─→ 06 ──┬─→ 07
         └─→ 09        ├─→ 08 ←┘(02) ─→ 11
                       └─→ 10
```

**可并行开工**：01、02、03、04 四张没有任何阻塞；12 同样无阻塞。

## 剩余的开工次序

07 与 12 互不阻塞、改的地方也不重叠，可并行：07 在 `core/ai/selection.*`，12 在
`agent/orchestrator`。

- **07** 的阻塞边（06）已经解开，可以开工。它与已收口的 08 都改
  `core/ai/selection.{h,cpp}`：08 只往提示词里加输入，07 要动返回结构与本地
  的约束解码 schema，两张并行必冲突，所以 08 先走完了。
- **11** 已于 2026-08-02 实现（拍板 + 落地同日）。两块板：`exclude` 在简述变更
  时保留、新简述整体替换旧简述，理由见票内"拍板"一节。落地方式与两个 Eng
  Design 问题的答案见票内"落地记录"。剩两条待真机验：重选是否真的不重跑评估、
  以及简述抽取的质量（eval 集已补 `GATE_REPLY_CASES`，要人读一遍输出）。
- **12** 是票 11 落地时发现的既有缺陷，不是票 11 引入的：`rerun_stage` 答完的
  闸门不解除 `spec.gate`，于是"只说去重没给数量"那条流程里，选片确认闸门上的
  任何一次调整都会把"要不要再筛选一下"再问一遍、Curate 不重跑。对现有三个
  adjust action 一视同仁，已随现有版本分发出去。票 11 里只用一个测试钉住了现
  状，没顺手改 - 它要动 `orchestrator/driver.py` 的 gate 生命周期，而那条路跟
  票 10 的 rewind、AG-01 的 Style 重问共用。
- **10** 已于 2026-08-02 完全收口（闸门在 headless 上从"阻塞式确认"改成"告知 + 随时可撤"，PRD 的 G5/决策十八已按**入口**改述）。`ai_declined`/`cancelled` 按拍板决策三**永久不进** headless JSON。Telegram 端到端真机验收已通过。

## 一条必须遵守的顺序约束

**03 必须先于 05 完成，这是正确性约束而非偏好。**

PRD 决策七定的缓存判据是"有评估记录就跳过"。若 05（curate 内部跑评估）先落地，那批照片会写入**没有 `content` 的评估记录**，而这些记录之后**永远不会被刷新** - 03 落地之后它们照样命中缓存。顺序颠倒会留下一批哑数据，且不报错。
