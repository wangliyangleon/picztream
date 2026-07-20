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

## 安装统计

第三方个人 tap **拿不到 Homebrew 官方 analytics**（formulae.brew.sh / `brew analytics` 只覆盖 homebrew/core）。现有粗略信号：**tap 仓 `homebrew-pzt` → Insights → Traffic → Git clones**（≈ 多少台机器 tap 过；14 天窗口，不区分 formula）。GitHub 对自动生成的源码 tarball 下载不计数。

想要真实每-formula 安装数：需改发 **bottle**（预编译，作为 Release asset，读 `download_count`）—— 已作为低优先级条目记在 [`Task_Pool.md`](Task_Pool.md)（触发前提=有用户量诉求）。
