# Issue tracker: GitHub

Issues and PRDs for this repo live as GitHub issues (`wangliyangleon/picztream`). Use the `gh` CLI for all operations.

**ADR 不在这里** - 见下方"本仓库的偏离"。

## Conventions

- **Create an issue**: `gh issue create --title "..." --body "..."`. Use a heredoc for multi-line bodies.
- **Read an issue**: `gh issue view <number> --comments`, filtering comments by `jq` and also fetching labels.
- **List issues**: `gh issue list --state open --json number,title,body,labels,comments --jq '[.[] | {number, title, body, labels: [.labels[].name], comments: [.comments[].body]}]'` with appropriate `--label` and `--state` filters.
- **Comment on an issue**: `gh issue comment <number> --body "..."`
- **Apply / remove labels**: `gh issue edit <number> --add-label "..."` / `--remove-label "..."`
- **Close**: `gh issue close <number> --comment "..."`

Infer the repo from `git remote -v` - `gh` does this automatically when run inside a clone.

> **注意编号空间**：这个仓库的 issue 编号从 **#16** 起，因为 GitHub 的 issue 与 PR **共用同一个编号空间**，而此前已有 15 个 PR。看到一个裸 `#42` 时它可能是 PR，用 `gh pr view 42` 探一次、失败再回落 `gh issue view 42`。

## Pull requests as a triage surface

**PRs as a request surface: no.** _(Set to `yes` if this repo treats external PRs as feature requests; `/triage` reads this flag.)_

When set to `yes`, PRs run through the same labels and states as issues, using the `gh pr` equivalents:

- **Read a PR**: `gh pr view <number> --comments` and `gh pr diff <number>` for the diff.
- **List external PRs for triage**: `gh pr list --state open --json number,title,body,labels,author,authorAssociation,comments` then keep only `authorAssociation` of `CONTRIBUTOR`, `FIRST_TIME_CONTRIBUTOR`, or `NONE` (drop `OWNER`/`MEMBER`/`COLLABORATOR`).
- **Comment / label / close**: `gh pr comment`, `gh pr edit --add-label`/`--remove-label`, `gh pr close`.

## When a skill says "publish to the issue tracker"

Create a GitHub issue.

## When a skill says "fetch the relevant ticket"

Run `gh issue view <number> --comments`.

## Wayfinding operations

Used by `/wayfinder`. The **map** is a single issue with **child** issues as tickets.

- **Map**: a single issue labelled `wayfinder:map`, holding the Notes / Decisions-so-far / Fog body. `gh issue create --label wayfinder:map`.
- **Child ticket**: an issue linked to the map as a GitHub sub-issue (`gh api` on the sub-issues endpoint). Where sub-issues aren't enabled, add the child to a task list in the map body and put `Part of #<map>` at the top of the child body. Labels: `wayfinder:<type>` (`research`/`prototype`/`grilling`/`task`). Once claimed, the ticket is assigned to the driving dev.
- **Blocking**: GitHub's **native issue dependencies** - add an edge with `gh api --method POST repos/<owner>/<repo>/issues/<child>/dependencies/blocked_by -F issue_id=<blocker-db-id>`, where `<blocker-db-id>` is the blocker's numeric **database id** (`gh api repos/<owner>/<repo>/issues/<n> --jq .id`, _not_ the `#number` or `node_id`). GitHub reports `issue_dependencies_summary.blocked_by` (open blockers only). Where dependencies aren't available, fall back to a `Blocked by: #<n>, #<n>` line at the top of the child body. A ticket is unblocked when every blocker is closed.
- **Frontier query**: list the map's open children (`gh issue list --state open`, scoped to the map's sub-issues / task list), drop any with an open blocker or an assignee; first in map order wins.
- **Claim**: `gh issue edit <n> --add-assignee @me` - the session's first write.
- **Resolve**: `gh issue comment <n> --body "<answer>"`, then `gh issue close <n>`, then append a context pointer to the map's Decisions-so-far.

> **已验证（2026-08-20）**：**sub-issues 可用**。T-15 的五张实现票（#29-#33）已用 `gh api --method POST repos/<owner>/<repo>/issues/<parent>/sub_issues -F sub_issue_id=<child-db-id>` 成功挂到 PRD #28 之下，`<child-db-id>` 取自 `gh api repos/<owner>/<repo>/issues/<n> --jq .id`（**不是** `#number`，也不是 `node_id`），调用返回的是**父** issue 对象。列出子票用 `gh api repos/<owner>/<repo>/issues/<parent>/sub_issues`，返回顺序即挂载顺序，可以直接拿来表达施工顺序。
>
> **原生 issue dependencies（`blocked_by`）仍未验证** - T-15 这批的依赖关系写在票正文的"依赖"行里，没有试过那个端点。

---

## 重申一条默认（不是本仓库的特例）

### ADR 永远留在仓库，不进 issue

**`docs/adr/` 是 ADR 的唯一住所。** 不要把 ADR 发成 issue。

这**不是**本仓库的偏离，而是 skill 体系本来的默认：本文件的 GitHub 模板原文只说 "Issues and **PRDs** live as GitHub issues"，ADR 不在其中；`docs/agents/domain.md` 与 `/domain-modeling` 都把 `docs/adr/` 当作仓库内文件。之所以在这里再写一遍，是因为切到 GitHub 之后容易顺手把"所有设计文档"一起搬走。

底层理由：ADR 的语义是"永不关闭的既定事实"，issue 的语义是"待办、会关"，放进 issue 是类型错误。且 `docs/SPEC.md` §2.3/§4.3 与 `AGENTS.md` 都按路径引用 ADR。

---

## 本仓库的偏离（与 skill 默认行为不同，共两条）

### 其一：PRD 正在**渐进迁移**，两处并存是当前的正常状态

2026-08-09 之前，这个项目的 PRD 一律是仓库文件（`docs/<Name>_PRD.md`，收口后移进 `docs/history/`），票是 `docs/issues/<feature>/` 下的编号 markdown。自该日起改为 **PRD 与票都上 GitHub Issues**，但**存量不批量迁移**，一条条来。

因此当你要找一份 PRD 时：**先查下面的已迁清单，不在清单里就去仓库找。**

**已迁到 GitHub Issues 的 PRD**

| PRD | Issue | 迁移日期 | 备注 |
|---|---|---|---|
| HEIC 支持 | [#16](https://github.com/wangliyangleon/picztream/issues/16) | 2026-08-09 | 首份迁移的试点。原文件 `docs/HEIC_Support_PRD.md` 已删除，可从 `git show b67047e:docs/HEIC_Support_PRD.md` 取回 |
| T-29 recipe 自建 version 向导 + 实时预览 | [#17](https://github.com/wangliyangleon/picztream/issues/17) | 2026-08-11 | 第二份，且是**直接开在 Issues 上、从来没有过仓库文件**的第一份（不是"迁移"而是原生）。实现票 [#18](https://github.com/wangliyangleon/picztream/issues/18)/[#19](https://github.com/wangliyangleon/picztream/issues/19)/[#20](https://github.com/wangliyangleon/picztream/issues/20)，已于 2026-08-12 全部收口 |
| T-16 scope 解析与排除规则收进 core | [#23](https://github.com/wangliyangleon/picztream/issues/23) | 2026-08-13 | 第三份，同样是原生开在 Issues 上。立项时 D-1/D-2/D-3 已拍板，Q-2/Q-3 带建议待确认 |
| T-15 交互层批量作用域（`/recipe` + 当前视图） | [#28](https://github.com/wangliyangleon/picztream/issues/28) | 2026-08-20 | 第四份，原生开在 Issues 上。立项时 D-1..D-16 全部拍板，无待确认项；术语那条另有 `docs/adr/0002-*` |

**仍在仓库里的存量**（不完整列举，以 `docs/` 与 `docs/history/` 实际内容为准）：`docs/history/Intent_Curation_PRD.md`、`docs/history/Dedup_Cancel_PRD.md`、`docs/history/Dedup_AI_Console_PRD.md`、`docs/history/Env_Preflight_*`、`docs/history/Headless_Observability_*`、各里程碑与周目标的 PRD/Eng Design。**`docs/SPEC.md` 里有几十处按路径引用这些文件，迁移任何一份时必须同时改掉它的入边**，否则审计链断掉。

### 其二：已收口的票不迁移

`docs/history/issues/intent-curation/` 那 13 张原样留档。补建 closed issue 不产生价值，反而丢掉归档说明。

### 迁移一份存量 PRD 的步骤（承其一）

1. `grep -rn "<PRD 文件名>" --include="*.md" .` 找全入边
2. `gh issue create --title "<名字>" --label ready-for-agent --body-file <文件>`（去掉与 issue 标题重复的 H1）
3. 把每一处入边改成 issue 链接
4. `git rm` 掉原文件
5. 在上面的已迁清单里加一行
