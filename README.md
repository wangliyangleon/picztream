# PicZTream (`pzt`)

终端里的全键盘照片筛选与色彩处理工具 - 零延迟选片体验 + 高性能本地色彩流水线。

> **仅支持 macOS / Apple Silicon（M 系列芯片）。** 依赖 CoreGraphics/ImageIO 系统框架,
> 且色彩流水线针对 arm64 调优,Intel Mac 与其它平台暂不支持。

## 这是什么

`pzt` 把"从一堆照片里快速挑出要留的、给它们套上风格、导出"这件事做成纯终端、
全键盘的流程,配合 [Ghostty](https://ghostty.org)(Kitty 图像协议)在终端里直接看图:

- **零延迟选片**:`h/l` 翻页、`space` 打标签、`x` 标废片,配合预取缓存做到按键即出图。
- **本地色彩流水线**:内置一组 recipe 预设(城市+年份风格),可在预设上自建 version 微调,
  实时预览。
- **可选的 AI 辅助**:看图点评(文字点评 + 硬伤标记,不打分)、去重/选片用 AI 锦标赛式两两比较,
  支持云端(Claude/Gemini)或本地(Ollama)模型。
- **配套 agent**:一个 Telegram bot,把"发照片 → 一句话说想怎么弄 → 收成品"跑成半自动闭环,
  见 [`agent/README.md`](agent/README.md)。

## 安装

```sh
brew tap wangliyangleon/pzt
brew trust wangliyangleon/pzt   # Homebrew 6+ 对第三方 tap 的一次性信任门
brew install pzt
pzt --version
```

`brew` 会一并装好原生依赖(sqlite / libraw / libomp / nlohmann-json)。首次从第三方
tap 装东西时,Homebrew 6 会要求先 `brew trust` 这个 tap(一次即可),否则报
"untrusted tap"。

## 快速上手

```sh
pzt new trip ~/Photos/trip                 # 建项目,指向一个照片目录
pzt open trip                              # 进入全键盘选片,x 标废片
pzt export trip --all-keep ~/Photos/out    # 导出没被标废片的图
```

也可以先用 `space` 给要留的图打自定义标签（比如"精选"），再用
`pzt export trip 精选 ~/Photos/out` 按标签导出。

选片界面按键（`pzt open` 里）:

| 键 | 作用 |
|---|---|
| `h` / `l` | 上一张 / 下一张 |
| `j` / `k` | 跳到下一张 / 上一张**未打标签**的图 |
| `space` | 给当前图打标签（分组） |
| `x` | 标记为废片 |
| `e` | 导出（当前这张 / 全部,排除废片重复 / 当前筛选结果，二级菜单选） |
| `f` | 按标签筛选视图 |
| `r` | 应用 / 清除 / 新建 / 删除风格；`r v` 临时看原图 |
| `q` | 退出 |

其它命令用 `pzt --help` 查看完整 usage：`pzt list` / `pzt archive` / `pzt unarchive` /
`pzt delete` / `pzt rescan` / `pzt tag list` / `pzt recipe list` 等。

## 数据位置

所有项目、标签、recipe 存在一个本地 SQLite 库：`~/.config/pzt/pzt.db`
（若设了 `XDG_CONFIG_HOME` 则在其下）。照片本身不搬动,库里只存路径与元数据。
同目录下还可能有 `pzt.db-wal` / `pzt.db-shm`（SQLite WAL 边车文件）和
`raw_previews/`；备份时连 `-wal` 一起拷，`pzt` 运行期间只拷 `pzt.db` 会丢
掉最近的写入。

## AI 评估（可选）

看图点评(`pzt eval`)、去重/选片的两两比较(`pzt dedup` / `pzt curate` 内部调用 `pzt compare`)
需要一个模型。两条路：

- **本地(推荐,免配额)**：装 [Ollama](https://ollama.com)、`ollama pull gemma4:e2b`,
  然后 `pzt eval <项目> --provider local`。
- **云端**：设好 `ANTHROPIC_API_KEY` 或 `GEMINI_API_KEY`,用 `--provider claude` / `--provider gemini`。

半自动的 Telegram 闭环见 [`agent/README.md`](agent/README.md)。

## 从源码构建（贡献者）

```sh
brew install cmake ninja pkg-config sqlite libraw libomp nlohmann-json
git clone git@github.com:wangliyangleon/picztream.git
cd picztream
cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build_release
./build_release/cli/pzt --version
```

跑测试(Debug 带 ASan/UBSan)：

```sh
cmake -S . -B build -G Ninja       # 默认 Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## 文档

- [`AGENTS.md`](AGENTS.md) 是给 **AI agent 的开发指令**,不是用户手册。
- 设计文档(SPEC、各里程碑/周的 PRD 与 Eng Design)在 [`docs/`](docs/)。
- 维护者发布流程(一次性 GitHub 设置 + 怎么发版)见 [`docs/RELEASE.md`](docs/RELEASE.md)。

## 许可

[MIT](LICENSE)
