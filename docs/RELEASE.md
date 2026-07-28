# 发布（Release）指南

面向维护者。PZT 通过自建 Homebrew tap 分发：**主仓** `wangliyangleon/picztream`（源码 + formula 真相源）+ **tap 仓** `wangliyangleon/homebrew-pzt`（用户 `brew tap` 的对象）。CLI 与 agent 共用一个 CalVer 版本号、同指一个源码 tarball，formula 是 **source-build**（用户机上现构建）。

用户侧安装见根 [`README.md`](../README.md) 与 [`agent/README.md`](../agent/README.md)。

---

## 一次性设置（GitHub 网页，只做一次）

一键发布与主页部署依赖三项仓库设置。按 ①→②→③ 做。

### ① 开启 GitHub Pages（主页上线）

直达：`https://github.com/wangliyangleon/picztream/settings/pages`

1. Settings → 左栏 **Pages**。
2. **"Build and deployment"** → **Source** 下拉改成 **"GitHub Actions"**（即时保存，无需点 Save）。
3. 触发首次部署：**Actions** 标签 → **"Deploy homepage to Pages"** → 进最近一次运行 → **"Re-run all jobs"**（或改一下 `website/` 再 push）。
4. 访问 `https://wangliyangleon.github.io/picztream/` 确认渲染（首次 1-2 分钟）。

### ② 放开 Workflow 写权限（发版推 tag 回本仓）

直达：`https://github.com/wangliyangleon/picztream/settings/actions`

1. Settings → **Actions** → **General**。
2. 底部 **"Workflow permissions"** → 选 **"Read and write permissions"** → **Save**。

> 作用：release workflow 用内置 `GITHUB_TOKEN` 把 `Release` commit 与 tag 推回 `picztream`；默认只读推不动。

### ③ 建 `TAP_PUSH_TOKEN`（发版跨仓推 formula 到 tap 仓）

关键点：token 授权的是 **homebrew-pzt** 仓，secret 存在 **picztream** 仓。

**3a. 造 fine-grained PAT** — 直达：`https://github.com/settings/tokens?type=beta`

1. **"Generate new token"**。
2. Token name 任意（如 `pzt-tap-push`）；Expiration 按需（如 90 天，到期需重建）；Resource owner 选 **wangliyangleon**。
3. **Repository access** → **"Only select repositories"** → 勾 **`wangliyangleon/homebrew-pzt`**（只勾这一个）。
4. **Permissions** → **Repository permissions** → **Contents** → **"Read and write"**（Metadata 会自动 Read-only，强制的，其它留 No access）。
5. **"Generate token"** → **立刻复制** `github_pat_...`（只显示一次）。

**3b. 存成 picztream 的 Actions secret** — 直达：`https://github.com/wangliyangleon/picztream/settings/secrets/actions`

6. **"New repository secret"**。
7. Name 填 **`TAP_PUSH_TOKEN`**（一字不差）；Secret 粘贴 token → **"Add secret"**。

> 若将来 clone 报鉴权错，`release.yml` 里的 clone url 用的是 `https://x-access-token:${TAP_PUSH_TOKEN}@github.com/...`；fine-grained PAT 也支持 `https://${TAP_PUSH_TOKEN}@github.com/...` 形式，二选一。

---

## 发一个新版本

### 方式 A：一键（推荐，②③ 配好后）

仓库 **Actions** → 左栏 **"Release (一键发布)"** → **"Run workflow"** → 分支 `main`、版本留空（默认取当天 UTC 日期 CalVer）→ **Run workflow**。

workflow（`.github/workflows/release.yml`，跑在 `macos-15` arm64 runner）会：
1. 算版本（留空=当天日期，无前导零）；距上个 tag 无新提交则中止。
2. **构建闸门**：CLI Release 构建 + `ctest`；agent 干净 venv `pip install` + `pzt-agent --help` + `pytest` 回归。任一失败 → 不发。
3. 跑 `scripts/release.sh`：打 tag、回填两个 formula 的 url/sha256、推 main + tag。
4. 用 `TAP_PUSH_TOKEN` 把两个 formula 同步推到 tap 仓。

### 方式 B：本地手动（兜底）

在干净的 `main` 上（需装 `git-cliff`）：

```sh
scripts/release.sh 2026.7.20     # CalVer,不带 v 前缀
```

它做方式 A 的 1、3 步（不含构建闸门与自动 tap 推送），末尾会打印同步到 tap 仓的命令，照着执行即可。

---

## 版本号：CalVer

从部署起用 **CalVer 日期版本 `YYYY.M.D`（无前导零）**，例 `2026.7.20`。整仓一个版本号，`pzt` 与 `pzt-agent` 每次一起升。选无前导零是为了让 git tag / CMake `project(VERSION)` / formula url / `pzt --version` 四处一致。**一天一个版本**：同一天再发会撞 tag，脚本 abort。

发布 = 改 `CMakeLists.txt` 的 `project(VERSION)` + `agent/pyproject.toml` 的 `version` + 打 tag `vX.Y.Z` + formula url 跟随，这些都由 `release.sh` 一起做。CHANGELOG 由 `git-cliff`（配置 `cliff.toml`）自动生成，**不手改**。

用户升级：`brew update && brew upgrade pzt pzt-agent`。

---

## 数据兼容性

`pzt.db` 的 schema 版本记在库文件自己的 `PRAGMA user_version` 里，当前是 **1**（源头是 `core/db/schema.h` 的 `kSchemaVersion`）。跟 CalVer 无关，各走各的：CalVer 每次发布都变，schema 版本只在结构真的变了才变。

- **升级**自动、就地、单向。老库首次被新版打开时跑一次 `migrate_v0_to_v1`（建表、补列、盖章），整段在一个事务里，盖章放最后。已盖章的库直接跳过全部 schema 工作。
- **降级**被明确拒绝：库的版本比程序新时 `pzt` 打印一句"数据库是更新版本的 pzt 创建的…请升级"并 exit 1，不会尝试在未知结构上写入，也不碰这个库一个字节。所以**装回旧版之前先备份库**。

### 改 schema 必须 bump 版本号

这是本节最要紧的一条。T-7 之前，往 `initialize_schema` 里加一条 `ensure_column` 就能让新列在所有存量库上自动出现，因为每次开库都会把全部加列检查跑一遍。现在已盖章的库走快路径、根本不看那段代码，所以：

> 任何对 `initialize_schema` 的 DDL 改动，**包括纯粹加一列**，都必须同时把 `kSchemaVersion` 加一，并新增对应的 `migrate_vN_to_vN+1` 步骤。

忘了 bump 不会报错，只会让新列在所有存量安装上永远不出现，而在开发机的新建库上一切正常，是最难发现的那类 bug。

### WAL 与备份

库跑在 WAL 模式下，`pzt.db` 旁边会出现 `pzt.db-wal` 和 `pzt.db-shm`。最后一条连接关闭时会 checkpoint 并删掉它们，但 `pzt`（或 agent）运行期间**只拷 `pzt.db` 不是有效备份**，最近的写入还在 `-wal` 里。要么三个文件一起拷，要么先跑：

```sh
sqlite3 ~/.config/pzt/pzt.db "PRAGMA wal_checkpoint(TRUNCATE);"
```

### 历史记录：一次已经发生的静默数据丢失

T-7 之前，`initialize_schema` 里常驻着一条按列名匹配的破坏性迁移：检测到 `image_evaluations` 有 `exposure_score` 或 `unusable` 列就整表 `DROP`。它的依据是"库里都是迭代测试数据，无真实用户数据要保留"，这在开 tap 之前成立，之后不再成立。结果是：**任何在 v2026.7.20 装过、跑过评估、之后才升级的用户，评估结果在 `brew upgrade` 时被静默丢弃**，无提示无备份。T-7 把它换成了版本闸门下的一次性结构校验。

---

## 安装统计

第三方个人 tap **拿不到 Homebrew 官方 analytics**（formulae.brew.sh / `brew analytics` 只覆盖 homebrew/core）。现有粗略信号：**tap 仓 `homebrew-pzt` → Insights → Traffic → Git clones**（≈ 多少台机器 tap 过；14 天窗口，不区分 formula）。GitHub 对自动生成的源码 tarball 下载不计数。

想要真实每-formula 安装数：需改发 **bottle**（预编译，作为 Release asset，读 `download_count`）—— 已作为低优先级条目记在 [`Task_Pool.md`](Task_Pool.md)（触发前提=有用户量诉求）。
