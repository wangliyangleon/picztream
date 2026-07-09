#include "core/ai/ai.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace pzt::core::ai {

namespace {

// 具体型号按需更新——只保证是一个支持图片输入的型号，不是这份代码要锁死
// 的东西。真实调通验证留给真机验收（没有 API key 的环境下测不出型号是
// 不是还有效）。
constexpr const char* kClaudeModel = "claude-sonnet-4-5-20250929";
constexpr const char* kGeminiModel = "gemini-2.0-flash";
constexpr const char* kClaudeUrl = "https://api.anthropic.com/v1/messages";

std::optional<std::string> get_api_key(Provider provider) {
  const char* env_name = provider == Provider::Claude ? "ANTHROPIC_API_KEY" : "GEMINI_API_KEY";
  const char* value = std::getenv(env_name);
  if (!value || value[0] == '\0') return std::nullopt;
  return std::string(value);
}

std::string gemini_url(const std::string& api_key) {
  return std::string("https://generativelanguage.googleapis.com/v1beta/models/") + kGeminiModel +
         ":generateContent?key=" + api_key;
}

// 标准 base64，无外部依赖——仓库里目前没有现成的可以复用。
std::string base64_encode(const std::vector<std::uint8_t>& data) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 3 <= data.size()) {
    std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                       (static_cast<std::uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += kTable[n & 0x3F];
    i += 3;
  }
  std::size_t remaining = data.size() - i;
  if (remaining == 1) {
    std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += "==";
  } else if (remaining == 2) {
    std::uint32_t n =
        (static_cast<std::uint32_t>(data[i]) << 16) | (static_cast<std::uint32_t>(data[i + 1]) << 8);
    out += kTable[(n >> 18) & 0x3F];
    out += kTable[(n >> 12) & 0x3F];
    out += kTable[(n >> 6) & 0x3F];
    out += "=";
  }
  return out;
}

// 固定的"只回 JSON"系统层模板——schema_instruction(字段形状)+ user_prompt
// (调用方的任务描述)+ 一句强制要求，具体措辞是实现细节，不是这份文档要
// 锁死的接口契约。
std::string build_instruction_text(const std::string& user_prompt,
                                    const std::string& schema_instruction) {
  return schema_instruction + "\n\n" + user_prompt +
         "\n\nRespond with ONLY a single JSON object matching the shape described above. Do "
         "not include any other text, explanation, or markdown formatting.";
}

// 模型有时会把 JSON 包在 ```json ... ``` 这样的 markdown 代码块里，即使
// 指令里明确要求不要——解析之前先剥掉首尾代码围栏，这是用自然语言而不是
// 原生结构化输出这条路线下一个便宜但有效的缓解措施，见
// docs/M3_Eng_Design.md"风险与待确认问题"一节。
std::string strip_markdown_json_fence(const std::string& text) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  std::string t = text;
  t.erase(t.begin(), std::find_if(t.begin(), t.end(), not_space));
  t.erase(std::find_if(t.rbegin(), t.rend(), not_space).base(), t.end());

  if (t.rfind("```", 0) != 0) return t;
  std::size_t newline = t.find('\n');
  if (newline == std::string::npos) return t;
  t = t.substr(newline + 1);
  std::size_t closing_fence = t.rfind("```");
  if (closing_fence != std::string::npos) t = t.substr(0, closing_fence);
  return t;
}

Result<nlohmann::json, RequestError> parse_inner_json(const std::string& text) {
  try {
    return Result<nlohmann::json, RequestError>::Ok(
        nlohmann::json::parse(strip_markdown_json_fence(text)));
  } catch (const nlohmann::json::parse_error&) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
}

nlohmann::json build_claude_request(const std::string& image_base64,
                                     const std::string& instruction_text) {
  return {
      {"model", kClaudeModel},
      {"max_tokens", 1024},
      {"messages",
       nlohmann::json::array({{{"role", "user"},
                                {"content", nlohmann::json::array({
                                                {{"type", "image"},
                                                 {"source",
                                                  {{"type", "base64"},
                                                   {"media_type", "image/jpeg"},
                                                   {"data", image_base64}}}},
                                                {{"type", "text"}, {"text", instruction_text}},
                                            })}}})},
  };
}

Result<nlohmann::json, RequestError> parse_claude_response(const std::string& body) {
  nlohmann::json outer;
  try {
    outer = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error&) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  if (!outer.contains("content") || !outer["content"].is_array() || outer["content"].empty()) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  const auto& first = outer["content"][0];
  if (!first.contains("text") || !first["text"].is_string()) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  return parse_inner_json(first["text"].get<std::string>());
}

nlohmann::json build_gemini_request(const std::string& image_base64,
                                     const std::string& instruction_text) {
  return {
      {"contents",
       nlohmann::json::array(
           {{{"parts", nlohmann::json::array({
                           {{"inline_data",
                             {{"mime_type", "image/jpeg"}, {"data", image_base64}}}},
                           {{"text", instruction_text}},
                       })}}})},
  };
}

Result<nlohmann::json, RequestError> parse_gemini_response(const std::string& body) {
  nlohmann::json outer;
  try {
    outer = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error&) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  if (!outer.contains("candidates") || !outer["candidates"].is_array() ||
      outer["candidates"].empty()) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  const auto& candidate = outer["candidates"][0];
  if (!candidate.contains("content") || !candidate["content"].contains("parts") ||
      !candidate["content"]["parts"].is_array() || candidate["content"]["parts"].empty()) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  const auto& part = candidate["content"]["parts"][0];
  if (!part.contains("text") || !part["text"].is_string()) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  return parse_inner_json(part["text"].get<std::string>());
}

// curl_global_init 不是线程安全的，ScoreWorker 的后台线程会调
// perform_curl_post，不能假设只有一个线程在用——用一个函数内 static 变
// 量的立即调用 lambda 触发一次，C++ 保证 magic static 的初始化本身是线
// 程安全的。
void ensure_curl_global_init() {
  static const int result = [] { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
  (void)result;
}

std::size_t write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

}  // namespace

Result<HttpResponse, RequestError> perform_curl_post(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body) {
  ensure_curl_global_init();

  CURL* curl = curl_easy_init();
  if (!curl) return Result<HttpResponse, RequestError>::Err(RequestError::NetworkError);

  curl_slist* header_list = nullptr;
  for (const auto& [key, value] : headers) {
    std::string line = key + ": " + value;
    header_list = curl_slist_append(header_list, line.c_str());
  }

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

  CURLcode res = curl_easy_perform(curl);
  if (header_list) curl_slist_free_all(header_list);

  if (res != CURLE_OK) {
    curl_easy_cleanup(curl);
    return Result<HttpResponse, RequestError>::Err(RequestError::NetworkError);
  }

  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_cleanup(curl);

  return Result<HttpResponse, RequestError>::Ok(HttpResponse{status_code, std::move(response_body)});
}

Result<nlohmann::json, RequestError> request_json(const decode::DecodedImage& image,
                                                    const std::string& user_prompt,
                                                    const std::string& schema_instruction,
                                                    Provider provider, HttpPostFn http_post) {
  auto api_key = get_api_key(provider);
  if (!api_key) return Result<nlohmann::json, RequestError>::Err(RequestError::MissingApiKey);

  std::string instruction_text = build_instruction_text(user_prompt, schema_instruction);
  // encode_jpeg_bytes 只在宽高非法时才会失败——image 是已经解码成功的预览
  // 图，宽高非法是编程错误而不是运行时该处理的结果，.value() 内部的
  // assert(ok()) 就是这个契约，跟项目里 Result<> 的既有约定一致。
  std::string image_base64 = base64_encode(decode::encode_jpeg_bytes(image).value());

  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  nlohmann::json request_body;

  if (provider == Provider::Claude) {
    url = kClaudeUrl;
    headers = {
        {"x-api-key", *api_key},
        {"anthropic-version", "2023-06-01"},
        {"content-type", "application/json"},
    };
    request_body = build_claude_request(image_base64, instruction_text);
  } else {
    url = gemini_url(*api_key);
    headers = {{"content-type", "application/json"}};
    request_body = build_gemini_request(image_base64, instruction_text);
  }

  // 跟 core/browse/prefetch.cpp 往 stderr 打 hit/miss/wait_ms 同一个惯
  // 例——`pzt open --debug` 会把 stderr 收进屏幕底部的 debug 面板，不开
  // --debug 时静默丢弃，不是新的日志机制。这里特意打 instruction_text
  // (人话 prompt)而不是 request_body.dump()（那个里面混着几十/几百 KB
  // 的 base64 图片数据，糊一屏没法看)。
  const char* provider_name = provider == Provider::Claude ? "claude" : "gemini";
  std::fprintf(stderr, "[pzt ai] request (%s) prompt:\n%s\n", provider_name,
               instruction_text.c_str());
  std::fflush(stderr);

  auto http_result = http_post(url, headers, request_body.dump());
  if (!http_result.ok()) return Result<nlohmann::json, RequestError>::Err(http_result.error());

  const auto& response = http_result.value();
  std::fprintf(stderr, "[pzt ai] response (%s) status=%ld body:\n%s\n", provider_name,
               response.status_code, response.body.c_str());
  std::fflush(stderr);

  if (response.status_code < 200 || response.status_code >= 300) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::HttpError);
  }

  return provider == Provider::Claude ? parse_claude_response(response.body)
                                       : parse_gemini_response(response.body);
}

}  // namespace pzt::core::ai
