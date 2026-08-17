from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient, PztCommandError
from stages.progress import forwarding

# T-25：系统标签的稳定 ASCII 别名，由 `pzt images --json` 的 system_tags 字
# 段送过来，定义在 core（tagging::kRejectTagAlias/kDuplicateTagAlias），跟
# `--scope '#Reject'` 认的是同一组常量。
#
# 这里刻意**不**写 "废片"/"重复"：那是 core 的 canonical 存储名，属于显示
# 文案那一侧。此前这个 passthrough 分支直接拿中文字面量比对 `tags`，于是
# core 改名或未来把系统标签名 i18n 化时，这里会静默地不过滤任何东西——不
# 报错，只是把废片和重复项当成入选结果交付出去。
#
# 别名是标识符、不随界面语言变，所以在 Python 里写下它不构成"第三份排除规
# 则实现"：这一行是**策略**（passthrough 要排掉哪些系统标签），归调用方，
# 见 PRD #23 "统一的是机制不是策略"。机制（哪个标签算系统标签）在 core。
REJECT_TAG_ALIAS = "Reject"
DUPLICATE_TAG_ALIAS = "Duplicate"
_PASSTHROUGH_EXCLUDED_SYSTEM_TAGS = frozenset({REJECT_TAG_ALIAS, DUPLICATE_TAG_ALIAS})


@dataclass
class CurateStage:
    client: PztClient
    name: str = "Curate"
    # "Ingest" 而非 "Dedup"：run() 从不读 ctx.outputs["Dedup"]，这里只是顺
    # 序声明，Dedup 存在时 Plan 的 list 顺序已经保证先后。W2026-07-21 目标
    # 三起 Dedup 可能不在 Plan 里，声明成 "Dedup" 会让 Driver 的拓扑检查把
    # 它当成永远解不开的依赖。
    inputs: List[str] = field(default_factory=lambda: ["Ingest"])
    cost_class: str = "local"
    criticality: str = "critical"

    def run(self, ctx: StageContext, params: Dict[str, Any]) -> StageOutput:
        count = params["count"]
        apply_tag = params.get("apply_tag", "精选")
        exclude = params.get("exclude", [])

        # T-8：整簇因为 AI 比较失败退化成"按拍摄时间选最新"的簇数。不带出
        # 去的话，退化时用户拿到的话术跟 AI 真跑通时完全一样。passthrough
        # 分支根本不调 pzt curate，没有退化可言，保持 0。
        ai_fallback_count = 0
        # 票 07：可以直接发出去的一段话，跟选片是 core 那边同一次模型调用的
        # 产物。空串 = 没有文案，Deliver 那边就少发一条消息 - 缺席不是错误
        # （PRD 决策十五：附赠品坏了不能把关键结果一起拖下水）。passthrough
        # 分支不调 pzt curate，没有模型调用也就没有文案。
        caption = ""
        try:
            if count is None:
                # passthrough：Curate 被跳过聚类，直接把去重后的候选原样
                # 交付，不调 pzt curate（W2026-07-21 目标三决策三：pzt
                # curate 的 count 语义是"每簇最多一个 winner"，不是
                # "top N"，冒充会把用户明确拒绝的"再筛一次"悄悄做了）。
                images = self.client.call("images", ctx.project_id)["images"]
                # 下标而不是 .get：`pzt images --json` **无条件**发这个字
                # 段，缺席只可能是接到了一个过旧的 pzt（CLAUDE.md 记过这个
                # 坑：worktree 里没构建物时会静默回落到 brew 那个）。这种
                # 情况下 .get("system_tags", []) 会退回成"一张都不排"，正
                # 是 T-25 要消灭的那种无声失效；KeyError 打成 stage 失败是
                # 对的。
                survivors = [
                    img["path"] for img in images
                    if not _PASSTHROUGH_EXCLUDED_SYSTEM_TAGS.intersection(img["system_tags"])
                ]
                final_selection = [p for p in survivors if p not in exclude]
                requested = None
            else:
                args = [
                    "curate", ctx.project_id,
                    "--count", str(count + len(exclude)),
                    "--apply-tag", apply_tag,
                ]
                ai_enabled = params.get("ai_enabled", False)
                if ai_enabled:
                    args += ["--ai", "--provider", params.get("provider", "local")]
                # 票 08：用户的题材偏好/叙事要求，进 core 的选择提示词。没
                # 有要求时不发这个参数（空串传下去 core 也会把那一段整个省
                # 掉，少一个参数少一处能出错的地方）。不看 ai_enabled：关
                # AI 时 core 自己忽略它，在这里再判一次等于把"谁消费这个字
                # 段"的知识复制到第二个地方。
                brief = params.get("selection_brief", "")
                if brief:
                    args += ["--brief", brief]
                # T-8：只给这一次调用挂进度 sink。下面的 tag clear / N 次
                # tag apply 是毫秒级、没有进度可言，不该顶着 sink 跑。
                with forwarding(self.client, ctx, ai_enabled):
                    result = self.client.call(*args)
                # pzt curate --apply-tag 无条件给拿到的每一张候选打标(包括
                # 多要的 len(exclude) 张、以及要被换掉的那几张)，过滤裁剪
                # 之后必须重新收口标签状态：不能指望 --apply-tag 自己做对。
                final_selection = [p for p in result["selected"] if p not in exclude][:count]
                requested = count
                # .get 不是下标：不带 --ai 时这个 key 根本不出现（不是
                # "出现但恒为 0"，见 W2026-07-21 目标二），下标会 KeyError
                # 把整个 stage 打成失败。
                ai_fallback_count = result.get("ai_fallback_count", 0)
                # .get 同上：没有文案时这个 key 整个不出现（cli 那边刻意不
                # 留空字段），下标会把一个选好了片的 run 打成失败。
                caption = result.get("caption", "")

            self.client.call("tag", "clear", ctx.project_id, apply_tag)
            for path in final_selection:
                self.client.call("tag", "apply", ctx.project_id, path, apply_tag)
        except PztCommandError as e:
            return StageOutput(ok=False, error=f"{e.code}: {e.message}")

        return StageOutput(ok=True, data={
            "requested": requested,
            "returned": len(final_selection),
            "selected": final_selection,
            "ai_fallback_count": ai_fallback_count,
            "caption": caption,
        })
