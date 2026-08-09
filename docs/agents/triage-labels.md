# Triage Labels

The skills speak in terms of five canonical triage roles. This file maps those roles to the actual label strings used in this repo's issue tracker.

| Label in mattpocock/skills | Label in our tracker | Meaning                                  |
| -------------------------- | -------------------- | ---------------------------------------- |
| `needs-triage`             | `needs-triage`       | Maintainer needs to evaluate this issue  |
| `needs-info`               | `needs-info`         | Waiting on reporter for more information |
| `ready-for-agent`          | `ready-for-agent`    | Fully specified, ready for an AFK agent  |
| `ready-for-human`          | `ready-for-human`    | Requires human implementation            |
| `wontfix`                  | `wontfix`            | Will not be actioned                     |

When a skill mentions a role (e.g. "apply the AFK-ready triage label"), use the corresponding label string from this table.

Edit the right-hand column to match whatever vocabulary you actually use.

---

## 本仓库的落地状态（2026-08-09）

五个 label 全部存在于 `wangliyangleon/picztream`：

- `wontfix` 是 GitHub 建仓时的默认 label，**沿用未重建**。
- 其余四个于 2026-08-09 用 `gh label create` 建出。

仓库此前从未使用过 GitHub Issues，因此不存在"已有别的命名要对齐"的情况，直接用规范名。
