"""一句话风格描述到内置 preset 的纯文本匹配，不看图，是目标三 `core::ai::style`
（逐图 vision 挑风格）之外的新路径：用户描述想要的风格，只交给 LLM 做一次文本
匹配，见 docs/W2026-07-15_AgentStyle_Eng_Design.md 的后续设计。

`_PRESET_DESCRIPTIONS` 是 core/ai/style.cpp::preset_descriptions() 的第三份手写
副本（第一份是 core/recipe/recipe.cpp::builtin_presets() 的数值表，第二份是
core/ai/style.cpp 自己的文字描述副本）——agent/ 按现有架构约束（docs/M4_Eng_Design.md
"agent -> cli -> core 单向依赖的唯一通道"）不能直接读 C++ 那份表，只能自己再抄
一份，跟 core/ai/style.cpp 已经接受的漂移风险是同一个 tradeoff。
"""
from __future__ import annotations

from typing import List, Optional, Tuple

from compose.llm_client import HttpPostFn, request_json

_PRESET_DESCRIPTIONS: List[Tuple[str, str]] = [
    ("Havana 1959", "暖调、高饱和、古巴风情"),
    ("Tokyo 1966", "昭和时代暖调、低饱和、柔亮怀旧"),
    ("Paris 1974", "暖调、中低饱和、低对比"),
    ("Miami 1986", "中性白平衡、高饱和高对比、glossy 商业感"),
    ("New York 1994", "冷调、低饱和、高对比、粗颗粒"),
    ("Shanghai 2010", "中冷调、高饱和、glossy 商业感、极细颗粒"),
    ("Munich 1951", "黑白，极高对比、深邃影调、粗颗粒"),
    ("Rome 1960", "黑白，中低对比、柔亮影调"),
    ("Berlin 1989", "黑白，高对比、偏暗、情绪紧迫"),
]


class StyleMatchError(Exception):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


def describe_presets() -> str:
    """把内置 preset 一览拼成一段面向用户的文本（Style 问描述闸门 query
    分类的回复）。数据只有 _PRESET_DESCRIPTIONS 这一份来源。"""
    lines = "\n".join(f"- {name}：{desc}" for name, desc in _PRESET_DESCRIPTIONS)
    return ("可选的风格有：\n" + lines +
            "\n直接一句话描述你想要的就行，或者说\"原图就行\"不套滤镜")


def _schema_instruction(names: List[str]) -> str:
    return (
        "You are matching a user's free-text description of a desired photo color-grading "
        'style to one of a fixed set of presets. Respond with a single JSON object: '
        f'{{"recipe_name": <string, must be exactly one of {names}>}}.'
    )


def match_style_description(description: str, http_post: Optional[HttpPostFn] = None,
                             meta_provider: str = "local") -> str:
    names = [name for name, _ in _PRESET_DESCRIPTIONS]
    candidates_text = "\n".join(f"- {name}: {desc}" for name, desc in _PRESET_DESCRIPTIONS)
    user_prompt = (
        f"用户描述的想要的风格：{description}\n\n"
        f"从下面的候选风格里挑一个最匹配的：\n{candidates_text}"
    )
    decision = request_json(
        user_prompt=user_prompt,
        schema_instruction=_schema_instruction(names),
        provider=meta_provider,
        http_post=http_post,
    )
    recipe_name = decision.get("recipe_name")
    if recipe_name not in names:
        raise StyleMatchError("hallucinated", f"model picked {recipe_name!r}, not a real candidate")
    return recipe_name
