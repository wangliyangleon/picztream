import json

import pytest

from compose.llm_client import LlmRequestError
from compose.style_matcher import (
    describe_presets,
    match_style_description,
    StyleMatchError,
)


def test_describe_presets_lists_all_nine_with_skip_hint():
    text = describe_presets()
    for name in ("Havana 1959", "Tokyo 1966", "Paris 1974", "Miami 1986",
                 "New York 1994", "Shanghai 2010", "Munich 1951", "Rome 1960",
                 "Berlin 1989"):
        assert name in text
    assert "原图就行" in text


def _ollama_response(recipe_name: str) -> str:
    body = json.dumps({"recipe_name": recipe_name, "reasoning": "fits the mood"})
    return json.dumps({"message": {"content": body}})


def test_match_style_description_returns_a_valid_preset_name():
    captured_prompts = []

    def fake_http_post(url, headers, body):
        del url, headers
        captured_prompts.append(body)
        return 200, _ollama_response("Havana 1959")

    result = match_style_description("暖色调怀旧", http_post=fake_http_post)

    assert result == "Havana 1959"
    # 候选列表必须完整地丢给模型，不能漏掉任何一个 preset 名字
    prompt = captured_prompts[0]
    for name in ("Havana 1959", "Tokyo 1966", "Paris 1974", "Miami 1986", "New York 1994",
                 "Shanghai 2010", "Munich 1951", "Rome 1960", "Berlin 1989"):
        assert name in prompt


def test_match_style_description_rejects_a_hallucinated_recipe_name():
    def fake_http_post(url, headers, body):
        del url, headers, body
        return 200, _ollama_response("Not A Real Preset")

    with pytest.raises(StyleMatchError) as exc_info:
        match_style_description("暖色调怀旧", http_post=fake_http_post)

    assert exc_info.value.code == "hallucinated"


def test_match_style_description_propagates_llm_request_error():
    def fake_http_post(url, headers, body):
        del url, headers, body
        return 200, "not valid json"

    with pytest.raises(LlmRequestError):
        match_style_description("暖色调怀旧", http_post=fake_http_post)
