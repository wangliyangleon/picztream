# 10 - headless 的开销闸门与取消接线

**What to build:** 让 PRD 的 G5（用户可以在 AI 开跑前拒绝）在真机上端到端可达。今天它只在 core 里可达。

票 05 把开销闸门、取消、以及 `ai_declined`/`cancelled` 两个返回字段都做进了 `core/curate`，也有测试覆盖。但 headless 这一侧没接：

- `cli/commands/commands.cpp` 的 `cmd_curate` 给 `on_ai_gate` 传的是 `nullptr`，`on_cancel` 取默认值 `nullptr`（`pzt dedup --ai` 的 headless 路径同一处置，但 dedup 有 `pzt open` 控制台这个 TUI 入口兜底，curate 按 SPEC §3.2 没有）。
- `CurateResult.ai_declined`/`cancelled` 没有序列化进 `pzt curate` 的 JSON 输出。票 05 的落地记录写明了当时不加的理由：两个钩子都是 `nullptr` 时这两个字段恒为 false，加进去是死字段。

**后果**：真机上 `pzt curate --ai` 不会问任何人就开始花钱，也不能中途叫停。agent 那一侧无从得知用户拒绝过。

**待拍板（本票开工前必须定）**：闸门问在哪一层。

- *由 agent 侧现有的 Curate stage 对话闸门承担*：agent 在调 `pzt curate --ai` 之前自己算一遍开销并问用户。问题是那个数今天只有 core 算得出来（比较次数要分簇之后才知道，评估张数要扣掉缓存），agent 要么重算一遍（第四份实现，仓库已经吃过 scope 解析和"排除废片"各写一份的亏，见提案 T-16 / T-25），要么接受报一个更粗的估计。
- *经 headless 参数表达*：例如加一个"开销超过某阈值就直接拒绝并如实返回"的参数，由 agent 决定阈值。这条不违反 PRD 决策九 - **决策九否掉的是"把流程拆成两次调用"**（先问开销、用户点头后再调一次真跑），任何形式的往返式闸门都不在选项内，这一点写死。

取消这一侧相对简单：headless 已经有取消通路（T-8 把 `Style`/`StyleApplyAll` 补进过可取消集合），curate 照同一套接即可。

**语义约束（票 05 已定，本票不得推翻）**：取消**不是零写入**。评估逐张写库，喊停时已评估完的那几张留在库里，这是有意的 - 每条记录本身完整，留着正好被下次运行的缓存判据命中。零写入的承诺只对闸门（`ai_declined`）成立，那时一张都还没评估。用户话术必须如实反映这个区别。

**Blocked by:** 06（模型选择与排序接进 curate）

**Status:** needs-decision（闸门归属未定，不可直接开工）

- [ ] 真机上 `pzt curate --ai` 在任何视觉调用发生之前，用户被问过一次开销，且能拒绝
- [ ] 拒绝时零写入，且 agent 对用户说的话与"这个项目里没有可选的照片"可分辨
- [ ] 中途取消能真的停下，且话术如实说明已评估的那几张留在库里，不谎称零写入
- [ ] `ai_declined`/`cancelled` 进入 `pzt curate` 的 JSON 输出，且不再恒为 false
- [ ] 闸门仍然只问一次（core 侧 `gate_consulted` 的两个触发点行为不变）
- [ ] `pzt dedup --ai` 的闸门与取消不受影响
- [ ] 关 AI 时不触发闸门、不产生这两个字段的非默认值
