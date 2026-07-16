# 历史文档归档

这里存放已经完成或被取代的里程碑级文档，不需要每个 session 都加载。长期 ground truth 见 `docs/SPEC.md`，本周细节见 `docs/W{日期}_*`。需要追溯某个能力"当初是怎么设计的、为什么这么定"时，再按下表查阅对应文档。

## 里程碑 PRD / Eng Design

- `M0_PRD.md` / `M0_Eng_Design.md` - MVP，选图管理核心（全键盘、项目化、带上限标签分组，纯 JPEG，零 AI/网络依赖）
- `M1_PRD.md` / `M1_Eng_Design.md` - 风格化，`recipe.json` 色彩配方系统与同步预览
- `M2_PRD.md` / `M2_Eng_Design.md` - RAW 支持，LibRaw 接入，双层流水线打通（RAW 当前基线与风险见 `docs/RAW_Support.md`）
- `M3_PRD.md` / `M3_Eng_Design.md` - AI 辅助之选片辅助评分（主动触发）
- `M3_Dedup_PRD.md` / `M3_Dedup_Eng_Design.md` - AI 辅助之近似重复检测去重
- `M4_PRD.md` / `M4_Eng_Design.md` - Agent 半自动增量一，Telegram 选片-交付闭环
- `M4_Agent_Workflow_Design.md` - agent 编排层架构设计（Stage/Plan/Driver/闸门/调整模型的原始设计）
- `M4_Brainstorm.md` - M4 前期头脑风暴（agent 层、用例、本地模型策略的广义参考）

## 跨里程碑活文档（归档时的状态快照）

- `Roadmap.md` - 项目路线图原稿；其背景、双层流水线、设计哲学、里程碑总览已吸收进 `docs/SPEC.md`
- `Optimization_Backlog.md` - M1 收尾/M2 开始前的一次优化 review 快照，多数条目已完成，残留观察项在 `docs/Fix_It_Night_Review.md` 中被交叉引用

## Fix-it Night 评审

- `Fix_It_Night_2026-07_Completion_Report.md` - 2026-07 Fix-it Night 全面评审（基于 commit `8f5af14`）的完成报告：已完成的 22 条 F 编号逐项详情、执行日志、E2E 反馈、以及原始四视角评审分析。尚未完成的条目继续在 `docs/Fix_It_Night_Review.md` 里作为活跃 backlog 维护
