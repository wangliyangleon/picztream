# 07 - 文案端到端

**What to build:** 开 AI 选完片之后，用户顺带拿到一段可以直接发的**文案**。选片那一刻系统手里正好有关于这批照片的最完整信息，写文案不需要额外的调用。

文案与选片是**同一次模型调用**的产物，材料是被选中照片的内容描述与质量评价。因为用途（发朋友圈 / 发 ins / 给家人看）已经在用户意图里，文案的语气与平台适配由它驱动，不需要新的输入。

**失败必须隔离**（PRD 决策十五）：文案在返回结构里是**可选**的。缺失或不合法时**只丢文案，不影响选片结果**。选片是关键结果，文案是附赠品，附赠品坏了不能把关键结果一起拖下水 - 与 T-8 定的"进度是观测，不是结果"是同一形状的推理。

关 AI 时不产出文案，且不因此报错。

**已知风险（PRD 风险二，标为高）**：模型写文案时手里只有描述，没有照片。文案质量**完全取决于内容描述写得够不够具体**。若票 03 的内容描述偏向摄影评语，文案只能是空洞的漂亮话。本票需要真机验证文案是否可用，不可用时问题多半出在 03 的提示词而不是本票。

**Blocked by:** 06（模型选择与排序接进 curate）

**Status:** done（真机验证那一条待用户跑）

- [x] 开 AI 且模型返回合法文案时，文案随选片结果经 headless 输出、agent 携带、最终展示给用户
- [x] 模型未返回文案时，选片结果完全不受影响，仅缺文案
- [x] 模型返回的文案不合法时，同上，且不抛错、不重试
- [x] 关 AI 时不产出文案，流程不报错
- [x] 文案的语气/平台适配跟随用户意图里的用途
- [ ] 真机验证：文案基于画面内容而非摄影评语，可直接使用（需要 Ollama，agent 未跑）

## 落地记录

文案挂在票 06 已经铺好的那条管子上，全程没有新增第二次模型调用，也没有新
增一个字段之外的形状：`ai::SelectionResult` 多一个 `caption`，一路
`curate` → `pzt curate --json` → `CurateStage` → `DeliverStage`。

### 失败隔离落在哪三处

决策十五的"可选"不是一句态度，是三个具体位置：

1. **约束解码 schema**：`caption` 进 `properties`、**不进 `required`**。进
   `required` 会让本地模型在写不出文案时被逼着编一段，或者让整个响应作废、
   连 `picks` 一起丢 - 两者恰好都是失败隔离要防的事。
2. **解析**：缺 key、类型不对、全是空白，三种情况折叠成同一个空串，**不构
   成 `SelectionError`**。折叠是因为对每一个下游而言处置完全相同（少展示一
   段话），分开报只是让调用方多一个判不出所以然的分支。
3. **headless JSON**：没有文案时 `caption` 这个键**整个不出现**，不留空字
   段（同 `ai_fallback_count` 只在 `--ai` 时出现）。agent 侧一律 `.get`，
   下标会把一个已经选好片的 run 打成失败。

### 两处本票拍板的边界

- **整批退化时文案跟着作废**（`ai_selection_fallback=true` ⇒ `caption` 为
  空）。文案写的是模型挑的那几张，而交付的是确定性路径挑的另一批，留着等于
  让一段讲 A 的话配着 B 发出去。这与决策十三拒绝"不足时用确定性结果补齐"是
  同一个立场：不交付两套逻辑拼接的结果。
- **文案的语言跟着描述走，不给 core 接界面语言参数**。提示词里写的是
  "write it in the same language as the notes above"，而描述由
  `evaluate_and_store` 用固定语言写出，于是文案自动落在同一种语言上。
  `curate.cpp` 里那条"票 07 如果需要跟随界面语言，那时再把语言接进来"的注
  释已按此改写 - i18n 只在 cli 层，curate 只有 headless 一个入口，接进来是
  给一条没有第二个调用方的路径加参数。

### 展示形态

文案在 `DeliverStage` 里是**单独一条消息、不带任何前缀**。Telegram 上复制一
整条消息是一下的事，掺进"配文："之类的引导语就得手动挑起止，而这段字的全部
用途就是被原样贴出去。没有文案时什么都不说 - 那是附赠品的缺席，不是需要报
告的事件。

### 语气/平台适配没有新增输入

票 08 的 `selection_brief` 里已经含着用途（`plan_composer` 的提示词明写
"the destination or audience if the user named one"），本票只是在同一段提示
词里多说一句"让它也定文案的语气"。

### 验证

- core 388 + cli 69，Debug 与 Release 两套都跑过
- agent 527 passed
- `pzt curate --json` 不带 `--ai` 的输出与 main **逐字节相同**（6 张图的临
  时项目，两个 release 二进制对跑 `diff`）
- 未做真机 `--ai`（需要 Ollama）。风险二（文案会不会写成摄影评语）因此仍未
  被证伪 - 提示词里已经明写 "never critique or mention the photography
  itself -- this is the user's own post, not a review"，但这一类问题**只有
  真模型能发现**，注入假 `http_post` 的用例喂的是罐头答案（同票 08 真机验收
  的结论）。

### code review 收的两条（都在提示词里）

1. **文案的对象没有被限定到"选中的那几张"**。模型眼前列着 K 条描述而只挑
   `count` 条，原文一句含糊的 "these photos" 会让它顺手把落选的那几张也写进
   去 - 交付的照片里没有的东西出现在文案里，用户当场就能发现。改成 "the
   photos you picked -- it is about those, not about the ones you left out"。
2. **原文把 `assessment` 从材料里剔除了**，写的是 "draw on the 'content'
   notes, not the 'quality' ones"，与决策十五"文案的材料是被选中照片的
   `content` 与 `assessment`"、决策六"两个字段进了上下文就对两者都可见"抵
   触。风险二要防的是**成品**变成摄影评语，不是把那一栏从材料里拿走。改成两
   栏都留着、只约束落笔："their 'quality' notes tell you how well each one
   came out, so let those steer which moment you lead with, but never critique
   or mention the photography itself"。
