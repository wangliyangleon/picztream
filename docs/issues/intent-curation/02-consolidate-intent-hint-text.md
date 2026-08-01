# 02 - 引导语收成单一常量

**What to build:** 纯 prefactor，**用户可见行为一字不变**。

agent 在多处提示用户该怎么说出自己的意图，示例文案"选3张发朋友圈"在 session 层散落**五处**各写了一遍。票 08 要把这句示例扩写成能引导用户说出题材偏好的形式（"选三张有景有人、表情活泼的照片发朋友圈"），五处分头改必漏。

先把它们收敛成一个常量，让后面那次改写只需要动一个地方。"Make the change easy, then make the easy change."

**Blocked by:** None - can start immediately.

**Status:** ready-for-agent

- [ ] 五处示例文案统一引用同一个常量
- [ ] 用户看到的文字与改动前逐字相同
- [ ] 既有 session 层测试全绿，无需修改断言
