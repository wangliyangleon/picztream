# 关于照片的推理归 core,哪怕它不看图

**Status**: accepted

`CLAUDE.md` 与 `SPEC.md` §4 原先按"视觉 vs 语言"划分 AI 职责:视觉推理归 `core`(C++ headless),语言推理归 `agent`(Python)。跨簇选片这个新能力打破了这条轴 - 它读的是每张照片的文字描述,不看像素,按原轴该归 agent,但它推理的对象自始至终是照片。我们把轴改述为**关于照片的推理归 core,关于用户的推理归 agent**,判据是"输入里有没有照片信息(含照片的衍生描述)",并据此把这次纯文本的 LLM 调用放进 `core`。

## Considered Options

- **放 agent**:符合原轴的字面表述,且 `agent/compose/style_matcher.py` 已有"纯文本 LLM 调用"的成熟先例,零新增基础设施。否决理由是它会让 agent 的 LLM 职责从"理解用户说了什么"扩张到"判断哪张照片更好",而后者是这个项目一直刻意收在 `core` 里的东西。
- **放 core**(采纳):代价是 `core/ai/ai.h` 今天两个 `request_json` 重载都吃图,没有纯文本路径,需要新开一条;`core::curate::curate` 也从纯本地计算变成会超时会失败的网络调用。

新轴不是把原规矩放宽,而是换了个更准的说法:它把现有的每一条(看图点评/两两比较/看图选风格归 core,意图解析/对话调整/按文字描述匹配风格归 agent)都分类正确,同时自然容纳了新增的这一条。

## Consequences

- `core` 第一次出现非视觉的外部网络依赖,T-8 建立的进度与取消机制需要覆盖到它。
- `pzt curate --ai` 从纯本地计算变成一条会跑几十次 vision 调用加一次文本调用的重命令,性质向 `pzt dedup --ai` 靠拢。
- 唯一的消费者仍然是 agent(`curate` 按 SPEC §3.2 不进 TUI),所以这个决定不能靠"core 更通用"来辩护,只能靠职责边界本身。
