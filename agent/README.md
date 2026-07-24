# pzt-agent

PicZTream 的 Telegram 半自动选片-交付 agent。把"发照片 → 一句话说想怎么弄 →
收成品"跑成一个对话闭环:收图、按需去重、AI 两两比较选片、按你的描述套风格、导出交付,
每个主观环节都带闸门等你确认。

> 面向用户的项目总览见根 [`README.md`](../README.md)。**仅 macOS / Apple Silicon。**

## 前置条件

1. **`pzt` CLI 已装**:`brew install pzt-agent` 会顺带装 `pzt`,无需单独装。
2. **一个 Telegram bot**:找 [@BotFather](https://t.me/BotFather) 发 `/newbot`,拿到 **bot token**;
   再拿到**你自己的 chat_id**(给你的 bot 发一条消息,然后开
   `https://api.telegram.org/bot<token>/getUpdates` 看 `chat.id`,或问 [@userinfobot](https://t.me/userinfobot))。
3. **一个 AI 模型**(去重/选片的两两比较、按描述匹配风格要用):
   - 本地(默认,免配额):装 [Ollama](https://ollama.com)(`brew install --cask ollama`),
     `ollama pull gemma4:e2b`,保持 Ollama 在跑。
   - 或云端:设 `ANTHROPIC_API_KEY` / `GEMINI_API_KEY`,并把 `PZT_AGENT_META_PROVIDER`
     设成 `claude` / `gemini`。

## 安装

```sh
brew tap wangliyangleon/pzt
brew trust wangliyangleon/pzt        # Homebrew 6 第三方 tap 一次性信任门
brew install pzt-agent               # 顺带装 pzt
```

## 配置与启动

```sh
# 1) 拷配置样例并填 token / chat_id
cp "$(brew --prefix)/share/pzt-agent/pzt-agent.env.example" ~/.pzt-agent.env
$EDITOR ~/.pzt-agent.env             # 填 TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID

# 2) 载入环境变量并启动(前台)
set -a; source ~/.pzt-agent.env; set +a
pzt-agent
```

长驻建议放进 tmux:

```sh
tmux new -s pzt 'set -a; source ~/.pzt-agent.env; set +a; pzt-agent'
```

启动后,在 Telegram 里给你的 bot 连发几张照片、再发一句话(比如"给朋友圈选 9 张,暖色调"),
按提示确认即可。

## 环境变量

| 变量 | 必填 | 说明 |
|---|---|---|
| `TELEGRAM_BOT_TOKEN` | 是 | @BotFather 给的 token |
| `TELEGRAM_CHAT_ID` | 是 | 只接受这个 chat 的消息(单用户自托管) |
| `PZT_AGENT_META_PROVIDER` | 否 | 语言推理 provider:`local`(默认)/`gemini`/`claude` |
| `PZT_BIN` | 否 | 指定 `pzt` 二进制;brew 装了就在 PATH 上,一般无需设 |

## 命令行参数

`pzt-agent --help` 可查。常用:

- `--state-dir`:状态落盘目录,默认 `~/.pzt-agent`(下含 `runs/`、`incoming/`、`preview/`、
  `deliver-out/`、`agent.log` 等)。
- `--poll-interval`:Telegram 长轮询间隔。
- `--idle-reminder-seconds` / `--progress-interval-seconds`:提醒与进度播报节奏。

## 单用户自托管语义

当前设计是**单聊天、单活跃 run**:同一时刻只处理一批。处理进行中新发的照片进
`_pending` 排队,本批结束后并入下一批。多用户/多 chat_id 与公网托管不在当前范围。
进程被 kill 后重启会自动续跑上次没跑完的 run(状态都在 `--state-dir` 里)。
