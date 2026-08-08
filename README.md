# PicZTream (`pzt`)

终端里的全键盘照片筛选与色彩处理工具 - 把"从几百张里挑出该留的"这件事的成本压下去。

> **仅支持 macOS / Apple Silicon（M 系列芯片）。** 依赖 CoreGraphics/ImageIO 系统框架,
> 且色彩流水线针对 arm64 调优,Intel Mac 与其它平台暂不支持。

## 这是什么

旅行回来面对几百张照片,真正吃时间的不是精修那几张,是"筛选"这个动作本身。它的
成本可以写成一个乘式:

> **选片成本 = 单次决策延迟 × 决策次数**

系统相册、Lightroom、Capture One 都是为鼠标和缩略图网格设计的,切图延迟和 UI 响应
链路把第一个因子撑得很大;而第二个因子直接等于照片张数,没人替你减。`pzt` 两个因子
分别打。

**第一个因子:把单次决策的延迟压到零。** 纯终端、全键盘,配合 [Ghostty](https://ghostty.org)
(Kitty 图像协议)在终端里直接看物理分辨率的大图。`h`/`l` 翻页按键即出图(预取缓存),
`space` 打标签、`x` 标废片,手不离键盘,心流不断。

**第二个因子:让 AI 替你减少要做的决策。** 近似重复检测把连拍收成一簇,簇内用**两两
比较**挑该留的那张 - 而不是各自打分再横向比,因为单张照片独立打出来的分跨图比大小
并不可靠。再说一句话讲清这批照片是干嘛的,它读着每张照片的描述替你跨簇选片、排好
顺序,顺带写一段配文。这一整块**都是可选的**,关掉不影响上面那条零延迟主路径。

**RAW 是第一个因子的极端案例。** RAW 在精修阶段价值明确,但对绝大多数看一眼就该淘汰
的照片,它的解码成本是纯粹负担 - 这正是"想用 RAW 却因为嫌麻烦而放弃"的根因。`pzt`
选片时只读相机 ISP 已经渲染好的内嵌 JPEG,绕开 RAW 解码;只有你明确标记要精修的那
几张,才真正走 LibRaw 那条重路径。

此外还有**本地色彩流水线**:内置一组 recipe 预设(城市+年份风格),可在预设上自建
version 微调,实时预览。以及**配套 agent**:一个 Telegram bot,把"发照片 → 一句话说
想怎么弄 → 收成品"跑成半自动闭环,见 [`agent/README.md`](agent/README.md)。

> **输入格式现状**:目前支持 **JPEG**。**RAW 是 opt-in** - `pzt new` / `pzt rescan`
> 要显式带 `--support-raw` 才会扫描 RAW,不带时纯 RAW 目录会报"没有找到任何
> JPEG/RAW 文件"。iPhone 默认拍摄的 **HEIC 暂不支持**,这是已知的高优先级缺口。

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

其它命令用 `pzt --help` 查看完整 usage：`pzt list` / `pzt delete` / `pzt rescan` /
`pzt tag list` / `pzt recipe list` 等。

## 数据位置

所有项目、标签、recipe 存在一个本地 SQLite 库：`~/.config/pzt/pzt.db`
（若设了 `XDG_CONFIG_HOME` 则在其下）。照片本身不搬动,库里只存路径与元数据。
同目录下还可能有 `pzt.db-wal` / `pzt.db-shm`（SQLite WAL 边车文件）和
`raw_previews/`；备份时连 `-wal` 一起拷，`pzt` 运行期间只拷 `pzt.db` 会丢
掉最近的写入。

## AI 辅助（可选）

看图点评、去重时的两两比较、跨簇选片都需要一个模型。两条路：

- **本地(推荐,免配额)**：装 [Ollama](https://ollama.com)、`ollama pull gemma4:e2b`,
  不用改配置,`ai_provider` 默认就是 `local`。
- **云端**：设好 `ANTHROPIC_API_KEY` 或 `GEMINI_API_KEY`,并在 `~/.config/pzt/config.json`
  里把 `ai_provider` 设成 `claude` / `gemini`。

配好之后在 `pzt open` 里用:

- `:` 进控制台、`/ai_eval *` 批量点评(`/ai_eval #标签名` 只评某个标签下的,
  `/ai_eval` 不带范围就只评当前这张)，`/tasks` 看进度。
- `/dedup <范围> --ai` 近似重复检测,簇内由 AI 两两比较挑保留哪张(开跑前会
  报出精确的比较次数等你确认,跑起来之后可以按 Ctrl-C 取消)。

**按一句话跨簇选片只在 Telegram 那条路上**：发一句"去重,挑 9 张有景有人的、别都是
风景,发朋友圈",agent 会把它解析成一条流水线 - 去重、**读着每张照片的描述按你这句
话选片并排序**、套风格、交付,结果还带一段配文。`pzt open` 里没有对应的 `/curate`,
理由见 [`docs/SPEC.md`](docs/SPEC.md) §3.2。完整用法见 [`agent/README.md`](agent/README.md)。

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
