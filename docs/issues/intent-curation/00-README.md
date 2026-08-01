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
| [06](06-model-selection-and-ordering.md) | 模型选择与排序接进 curate | 03, 04, 05 | ready-for-agent |
| [07](07-caption-end-to-end.md) | 文案端到端 | 06 | ready-for-agent |
| [08](08-selection-brief-plumbing.md) | 选片简述贯通 | 02, 06 | ready-for-agent |
| [09](09-agent-progress-rendering.md) | agent 侧进度渲染 | 05 | ready-for-agent |

## 依赖图

```
01  (游离)
02 ────────────────────────┐
03 ──┬──────┐              │
04 ──┴─→ 05 ─┴─→ 06 ──┬─→ 07
         └─→ 09        └─→ 08 ←┘(02)
```

**可并行开工**：01、02、03、04 四张没有任何阻塞。

## 一条必须遵守的顺序约束

**03 必须先于 05 完成，这是正确性约束而非偏好。**

PRD 决策七定的缓存判据是"有评估记录就跳过"。若 05（curate 内部跑评估）先落地，那批照片会写入**没有 `content` 的评估记录**，而这些记录之后**永远不会被刷新** - 03 落地之后它们照样命中缓存。顺序颠倒会留下一批哑数据，且不报错。
