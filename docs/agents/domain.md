# Domain Docs

How the engineering skills should consume this repo's domain documentation when exploring the codebase.

**Layout: single-context.** 一个根 `CONTEXT.md` + 一个 `docs/adr/`，无 `CONTEXT-MAP.md`（本仓库是 C++/Python 单体仓，不是 monorepo）。

## Before exploring, read these

- **`CONTEXT.md`** at the repo root
- **`docs/adr/`** - read ADRs that touch the area you're about to work in.

If any of these files don't exist, **proceed silently**. Don't flag their absence; don't suggest creating them upfront. The `/domain-modeling` skill (reached via `/grill-with-docs` and `/improve-codebase-architecture`) creates them lazily when terms or decisions actually get resolved.

## File structure

```
/
├── CONTEXT.md
├── docs/adr/
│   └── 0001-core-hosts-photo-reasoning-even-when-text-only.md
├── core/    cli/    agent/
```

## Use the glossary's vocabulary

When your output names a domain concept (in an issue title, a refactor proposal, a hypothesis, a test name), use the term as defined in `CONTEXT.md`. Don't drift to synonyms the glossary explicitly avoids.

`CONTEXT.md` 的每个词条都带一行 `_Avoid_:`，列出**不要用**的近义词。这不是风格建议 - 这个项目里已经出现过因为词义漂移而产生的真实成本（"多用户"同时指"多人协作"与"多用户托管"两件事，导致提案 T-20 被误判为可以随 T-4 自动收敛）。

If the concept you need isn't in the glossary yet, that's a signal - either you're inventing language the project doesn't use (reconsider) or there's a real gap (note it for `/domain-modeling`).

## Flag ADR conflicts

If your output contradicts an existing ADR, surface it explicitly rather than silently overriding:

> _Contradicts ADR-0001 (关于照片的推理归 core) - but worth reopening because…_

## 这个仓库特有的一点

**`docs/SPEC.md` 是每个 session 唯一必读的 ground truth**，先于本文件列的任何东西读。它承载项目定位、模块划分、设计哲学、对外接口轮廓、技术契约。`CONTEXT.md` 只是术语表，**不重复 SPEC 的内容**，也不记实现决策。

## 一条容易被切 tracker 带跑的默认

**ADR 永远留在仓库、不进 issue tracker**，即便 PRD 与票已经迁到 GitHub Issues。这是 skill 体系本来的默认（本文件上方的目录结构图就把 `docs/adr/` 画在仓库里），**不是本仓库的特例** - 写在这里只是因为切到 GitHub 之后容易顺手把"所有设计文档"一起搬走。理由与 PRD 的迁移政策见 `docs/agents/issue-tracker.md`。
