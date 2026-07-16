#include "core/ai/style.h"

#include <algorithm>

namespace pzt::core::ai {

namespace {

// 9 个内置预设的一句话风格摘要，内容取自
// docs/W2026-07-15_RecipeExpansion_Eng_Design.md 第五节——写死在这里而不
// 是给 core::recipe::PresetSummary 加 description 字段，因为这是"给 AI
// 看的 prompt 内容"，跟"预设本身的数据模型"是不同性质的东西。这是跟
// core/recipe/recipe.cpp::builtin_presets() 数值表的第二份手写副本，有
// 漂移风险——以后加/改预设要记得两边一起改。查不到的名字直接退回裸名
// 字，不阻塞（见 describe_preset）。
const std::vector<std::pair<std::string, std::string>>& preset_descriptions() {
  static const std::vector<std::pair<std::string, std::string>> kDescriptions = {
      {"Havana 1959", "暖调、高饱和、古巴风情"},
      {"Tokyo 1966", "昭和时代暖调、低饱和、柔亮怀旧"},
      {"Paris 1974", "暖调、中低饱和、低对比"},
      {"Miami 1986", "中性白平衡、高饱和高对比、glossy 商业感"},
      {"New York 1994", "冷调、低饱和、高对比、粗颗粒"},
      {"Shanghai 2010", "中冷调、高饱和、glossy 商业感、极细颗粒"},
      {"Munich 1951", "黑白，极高对比、深邃影调、粗颗粒"},
      {"Rome 1960", "黑白，中低对比、柔亮影调"},
      {"Berlin 1989", "黑白，高对比、偏暗、情绪紧迫"},
  };
  return kDescriptions;
}

std::string describe_preset(const std::string& name) {
  for (const auto& [n, desc] : preset_descriptions()) {
    if (n == name) return desc;
  }
  return "";
}

std::string build_style_prompt(const std::vector<std::string>& preset_names) {
  std::string prompt =
      "Look at this photo and pick the single best-fitting color-grading style from the "
      "candidates below, based on the photo's content, mood, and lighting.\n\nCandidates:\n";
  for (const auto& name : preset_names) {
    std::string desc = describe_preset(name);
    prompt += "- " + name;
    if (!desc.empty()) prompt += " (" + desc + ")";
    prompt += "\n";
  }
  return prompt;
}

std::string build_style_schema_instruction() {
  return "Return a JSON object with this exact shape: {\"recipe_name\": <string, must exactly "
         "match one of the candidate names listed above>, \"reasoning\": <one short sentence "
         "explaining the pick>}.";
}

nlohmann::json build_style_json_schema(const std::vector<std::string>& preset_names) {
  return {
      {"type", "object"},
      {"properties",
       {
           {"recipe_name", {{"type", "string"}, {"enum", preset_names}}},
           {"reasoning", {{"type", "string"}}},
       }},
      {"required", nlohmann::json::array({"recipe_name", "reasoning"})},
  };
}

StyleError map_request_error(RequestError error) {
  switch (error) {
    case RequestError::MissingApiKey:
      return StyleError::MissingApiKey;
    case RequestError::NetworkError:
      return StyleError::NetworkError;
    case RequestError::HttpError:
      return StyleError::HttpError;
    case RequestError::ParseError:
      return StyleError::ParseError;
  }
  return StyleError::ParseError;  // 不可达，安抚 -Wreturn-type
}

}  // namespace

namespace detail {

Result<StyleSuggestion, StyleError> request_style_suggestion_impl(
    const decode::DecodedImage& image, const std::vector<std::string>& preset_names,
    Provider provider, HttpPostFn http_post, const LocalModelConfig& local_config) {
  std::string user_prompt = build_style_prompt(preset_names);
  std::string schema_instruction = build_style_schema_instruction();

  auto json_result = request_json(image, user_prompt, schema_instruction, provider, http_post,
                                   local_config, build_style_json_schema(preset_names));
  if (!json_result.ok()) {
    return Result<StyleSuggestion, StyleError>::Err(map_request_error(json_result.error()));
  }

  const auto& j = json_result.value();
  if (!j.contains("recipe_name") || !j["recipe_name"].is_string() || !j.contains("reasoning") ||
      !j["reasoning"].is_string()) {
    return Result<StyleSuggestion, StyleError>::Err(StyleError::ParseError);
  }

  std::string recipe_name = j["recipe_name"].get<std::string>();
  // 模型可能编造一个不在候选列表里的名字——Provider::Local 有 JSON Schema
  // 的 enum 约束帮忙,但 Claude/Gemini 没有这层结构化保证(local_json_schema
  // 只有 Local 分支消费),这一步校验对三个 provider 一视同仁,不能只依赖
  // schema 约束。跟 evaluation.cpp::parse_dimension 的"分数越界"校验是同
  // 一个"信任但要验证"精神。
  if (std::find(preset_names.begin(), preset_names.end(), recipe_name) == preset_names.end()) {
    return Result<StyleSuggestion, StyleError>::Err(StyleError::Hallucinated);
  }

  return Result<StyleSuggestion, StyleError>::Ok(
      StyleSuggestion{recipe_name, j["reasoning"].get<std::string>()});
}

}  // namespace detail

Result<StyleSuggestion, StyleError> request_style_suggestion(
    const decode::DecodedImage& image, const std::vector<std::string>& preset_names,
    Provider provider, const LocalModelConfig& local_config) {
  return detail::request_style_suggestion_impl(image, preset_names, provider, perform_curl_post,
                                                local_config);
}

}  // namespace pzt::core::ai
