# 13 - 方案确认阶段也能改题材要求

**What to build:** 把票 11 在**选片确认**闸门上做成的事，补到**方案确认**这一步。

票 11 解决的是 `AWAITING_GATE`（看到选出来的照片之后）。但用户更早就会想改题材要
求：方案回显完、还没开跑的时候（`PLANNED`）。这一步走的是另一条路
（`refine_plan_confirmation` / `PlanConfirmationReply`），它认得的可调字段只有四
个 - `count`、`apply_tag`、`ai_enabled`、`provider`，**没有 `selection_brief`**。

## 真机证据（2026-08-03）

方案已回显成"帮你选择 2 张发 ins 用，**要有景有人的**照片"，用户想把题材要求换掉：

```
text='选两张小清新一点的吧'  status=planned  kind=refine_plan
-> 回复: '请问您指的"小清新"是指特定的标签名称，还是希望我开启AI智能筛选来选择
          风格更清新的照片？'
```

模型手里只有那四个字段，于是把"小清新"往 `apply_tag` 和 `ai_enabled` 上套。用户
澄清"风格"之后它还是问不出所以然。第二轮更糟：

```
text='选两张画面小清新一点的吧'  status=planned  kind=refine_plan
-> 回复: '理解你想：使用AI帮你选择 2 张发 ins 用，要有景有人的照片…'
```

`ai_enabled` 被静默翻成了 `true`，而题材要求仍是最初那句"要有景有人的"。这正是票
11 开头描述的两种失败形态（"被判成听不懂"与"更糟：静默做了别的事"），只是发生在
另一个阶段。

## 一条要有意翻掉的既有论证

`consumer._plan_summary` 的 docstring 写着：

> 刻意分成两个方法：`_current_plan_params` 还喂着 refine_plan 那次分类的提示词，
> 而 `PlanConfirmationReply` 没有 `selection_brief` 字段 - 把它塞进提示词，模型改
> 不了，只会当噪音读，还可能误以为自己该改。

这条论证在当时是对的，前提是"模型改不了"。本票要做的就是让它可改，前提翻转，两个
方法的差别随之消失，应当合并回一个。**不要把这条注释当成不能碰的约束**，它是在
记录一个当时成立的推理，不是一条设计禁令。

**Blocked by:** 无（票 11 已落地，两块板可直接沿用）

**Status:** ready-for-agent

## 沿用票 11 的拍板，不重新拍

- **决策二（新简述整体替换旧简述）** 直接适用。
- **决策一（`exclude` 保留）** 在这一步是空的：`PLANNED` 阶段什么都还没跑，不存在
  已累积的 `exclude`。
- **`None` / `""` 的区分**沿用票 08 立、票 11 复用的那一套：没提到就不动旧值，空
  串才是明确清除。注意 `refine_plan_confirmation` 现有的 `confirmed` 分支用的是
  `decision.get(k, current_params.get(k))`，这个写法对**显式 null** 是错的（key
  在、值为 null 时会把旧值覆盖成 None），brief 这一路要显式判 `isinstance(str)`。

## 验收标准

- [ ] 方案确认阶段说"选两张小清新一点的吧"，`selection_brief` 被改掉并回显新的
- [ ] 同一句里的数量也生效（"选两张…"要同时改到 `count=2`）
- [ ] 不再把题材要求误当成 `apply_tag` 或 `ai_enabled`（真机那两条日志是判据）
- [ ] 没提题材要求的轮次（"改成6张"）不清空已有简述
- [ ] `_current_plan_params` 与 `_plan_summary` 合并回一个，docstring 里那条已翻
      转的论证一并改掉
- [ ] AI 快捷按钮（`_BTN_AI_CURATE` / `_BTN_AI_DEDUP`）走同一条参数应用路径，不
      因为多了一个字段而把简述弄丢
- [ ] 用真模型验过，不只是注入假 `http_post`（同票 11：这次又往
      `_CONFIRMATION_SCHEMA_INSTRUCTION` 上加了字段，正是票 08 教训点名的风险）
