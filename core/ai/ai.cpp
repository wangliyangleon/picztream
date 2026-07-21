#include "core/ai/ai.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace pzt::core::ai {

const char* to_string(Provider provider) {
  switch (provider) {
    case Provider::Claude: return "claude";
    case Provider::Gemini: return "gemini";
    case Provider::Local: return "local";
  }
  return "gemini";  // 不可达，安抚 -Wreturn-type（同 evaluation.cpp::map_request_error 的写法）
}

namespace {

// 具体型号按需更新——只保证是一个支持图片输入的型号，不是这份代码要锁死
// 的东西。真实调通验证留给真机验收（没有 API key 的环境下测不出型号是
// 不是还有效）。
constexpr const char* kClaudeModel = "claude-sonnet-4-5-20250929";
constexpr const char* kGeminiModel = "gemini-3.1-flash-lite";
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

nlohmann::json build_claude_request(const std::vector<std::string>& image_base64s,
                                     const std::string& instruction_text) {
  nlohmann::json content = nlohmann::json::array();
  for (const auto& image_base64 : image_base64s) {
    content.push_back({{"type", "image"},
                        {"source",
                         {{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", image_base64}}}});
  }
  content.push_back({{"type", "text"}, {"text", instruction_text}});
  return {
      {"model", kClaudeModel},
      {"max_tokens", 1024},
      {"messages", nlohmann::json::array({{{"role", "user"}, {"content", std::move(content)}}})},
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

nlohmann::json build_gemini_request(const std::vector<std::string>& image_base64s,
                                     const std::string& instruction_text) {
  nlohmann::json parts = nlohmann::json::array();
  for (const auto& image_base64 : image_base64s) {
    parts.push_back({{"inline_data", {{"mime_type", "image/jpeg"}, {"data", image_base64}}}});
  }
  parts.push_back({{"text", instruction_text}});
  return {
      {"contents", nlohmann::json::array({{{"parts", std::move(parts)}}})},
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

nlohmann::json build_local_request(const std::vector<std::string>& image_base64s,
                                    const std::string& instruction_text,
                                    const std::string& model,
                                    const std::optional<nlohmann::json>& json_schema) {
  nlohmann::json request = {
      {"model", model},
      {"format", json_schema.has_value() ? *json_schema : nlohmann::json("json")},
      {"stream", false},
      {"messages", nlohmann::json::array({
          {{"role", "user"}, {"content", instruction_text}, {"images", image_base64s}}
      })},
  };
  if (json_schema.has_value()) {
    // Ollama 官方文档给结构化输出的建议——真机验证过：qwen2.5vl:3b 在
    // 宽松 "json" 模式下约 44% 请求失败(JSON 结构错误/不闭合)，加上
    // schema 约束 + temperature=0 后连续多次同一张图请求全部成功、内
    // 容逐字相同。
    request["options"] = {{"temperature", 0}};
  }
  return request;
}

Result<nlohmann::json, RequestError> parse_local_response(const std::string& body) {
  nlohmann::json outer;
  try {
    outer = nlohmann::json::parse(body);
  } catch (const nlohmann::json::parse_error&) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  if (!outer.contains("message") || !outer["message"].contains("content") ||
      !outer["message"]["content"].is_string()) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::ParseError);
  }
  return parse_inner_json(outer["message"]["content"].get<std::string>());
}

// curl_global_init 不是线程安全的，EvaluationWorker 的后台线程会调
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

namespace detail {

// debug 面板一次只滚动显示固定的最后几行(见 cli/commands/browse.cpp 的
// kDebugRows)——prompt/response 原文经常自带换行(模板本身用 "\n\n" 分
// 段)，一条日志展开成十几行会把整个面板刷屏，刷掉刚才 prefetch/按键那些
// 更想看的日志，等于"根本看不到什么"。这里把换行压成空格、长度砍到一
// 个上限，保证一条日志固定只占一行，还留有用信息，不是单纯截断到看不
// 出内容的程度。
std::string compact_for_debug_log(const std::string& text) {
  constexpr std::size_t kMaxLen = 240;
  std::string flat;
  flat.reserve(text.size());
  for (char c : text) {
    flat += (c == '\n' || c == '\r') ? ' ' : c;
  }
  if (flat.size() > kMaxLen) {
    flat.resize(kMaxLen);
    // 按字节数砍容易把多字节 UTF-8 字符砍在中间(比如目标三新增的中文
    // 预设摘要 prompt)，产生非法字节序列——agent 侧 Python 用 text=True
    // 读子进程 stderr 时会直接 UnicodeDecodeError 崩溃(真机复现)。往回
    // 退到一个完整的字符边界:先跳过末尾连续的续字节(0x80-0xBF)，再看
    // 剩下最后一个字节是不是一个"声明的序列长度"和"实际跟着的续字节
    // 数"对得上的完整前导字节，对不上(前导字节本身也被砍了一部分或整
    // 个丢了续字节)就连它一起退掉。
    std::size_t i = flat.size();
    std::size_t continuations = 0;
    while (i > 0 && (static_cast<unsigned char>(flat[i - 1]) & 0xC0) == 0x80) {
      --i;
      ++continuations;
    }
    if (i > 0) {
      unsigned char lead = static_cast<unsigned char>(flat[i - 1]);
      std::size_t expected = 1;
      if ((lead & 0xE0) == 0xC0) {
        expected = 2;
      } else if ((lead & 0xF0) == 0xE0) {
        expected = 3;
      } else if ((lead & 0xF8) == 0xF0) {
        expected = 4;
      }
      if (expected != continuations + 1) {
        flat.resize(i - 1);
      }
    }
    flat += "...";
  }
  return flat;
}

}  // namespace detail

namespace detail {

// F-02：发送前把图片降采样到长边不超过这个上限——`decode_preview_file`
// 拿到的预览图在纯 JPEG 项目里经常就是原图分辨率(24MP+)，未经缩放直
// 接 base64 编码上传，既让每次请求的 token 成本/延迟成倍膨胀，又可能
// 直接撞上 Claude 单张图片 5MB 的上限；视觉模型本身也会在输入端把图缩
// 到自己的工作分辨率，原图更大不会换来更准的判断。1024px 是 Claude 官
// 方文档给出的视觉输入常见尺寸下限之上、Gemini 同样能舒服处理的经验
// 值，不是精确调出来的最优值——真机测试如果发现评分质量明显下降，再
// 回头调大。
constexpr int kMaxUploadEdge = 1024;

decode::DecodedImage downscale_for_upload(const decode::DecodedImage& image) {
  if (image.width <= kMaxUploadEdge && image.height <= kMaxUploadEdge) return image;
  double scale = static_cast<double>(kMaxUploadEdge) / std::max(image.width, image.height);
  int target_w = std::max(1, static_cast<int>(image.width * scale));
  int target_h = std::max(1, static_cast<int>(image.height * scale));
  auto resized = decode::resize_rgba(image, target_w, target_h);
  // resize_rgba 只在宽高非法时才会失败——image 是已经解码成功的预览图，
  // 失败是"不该发生"的场景；退回原图而不是让整个请求失败，跟这个函数
  // "只是省流量/省成本的优化，不是正确性前提"的定位一致。
  return resized.ok() ? resized.value() : image;
}

}  // namespace detail

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
  // F-21：这个函数会在 EvaluationWorker 的后台线程上被调用，不是单线
  // 程场景——libcurl 默认可能用信号中断 DNS 解析超时，多线程进程里给
  // 一个不受自己管理的线程发信号是经典的崩溃/未定义行为来源，NOSIGNAL
  // 关掉这个行为(curl 自己的文档建议多线程程序始终设置这个选项)。
  // CONNECTTIMEOUT 单独给一个远小于总超时(60s)的上限——断网/DNS 卡住
  // 时不该让用户等一分钟才等到"网络失败"这个反馈，10s 对建立 TCP 连
  // 接这一步来说已经足够宽松。
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

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

Result<nlohmann::json, RequestError> request_json(const std::vector<decode::DecodedImage>& images,
                                                    const std::string& user_prompt,
                                                    const std::string& schema_instruction,
                                                    Provider provider, HttpPostFn http_post,
                                                    const LocalModelConfig& local_config,
                                                    const std::optional<nlohmann::json>& local_json_schema) {
  std::optional<std::string> api_key;
  if (provider != Provider::Local) {
    api_key = get_api_key(provider);
    if (!api_key) return Result<nlohmann::json, RequestError>::Err(RequestError::MissingApiKey);
  }

  std::string instruction_text = build_instruction_text(user_prompt, schema_instruction);
  // F-02：编码上传之前先降采样，见 detail::downscale_for_upload 的说明。多
  // 图(pairwise 比较)按顺序各编码一张——三个 provider 的请求体里图片都是
  // 按数组顺序排列的，顺序即调用方传入的顺序(compare 的 a 在前、b 在后)。
  // encode_jpeg_bytes 只在宽高非法时才会失败——upload_image 是已经解码成功
  // 的预览图(降采样失败时退回原图，同样合法)，宽高非法是编程错误而不是运
  // 行时该处理的结果，.value() 内部的 assert(ok()) 就是这个契约。
  std::vector<std::string> image_base64s;
  image_base64s.reserve(images.size());
  for (const auto& image : images) {
    decode::DecodedImage upload_image = detail::downscale_for_upload(image);
    image_base64s.push_back(base64_encode(decode::encode_jpeg_bytes(upload_image).value()));
  }

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
    request_body = build_claude_request(image_base64s, instruction_text);
  } else if (provider == Provider::Gemini) {
    url = gemini_url(*api_key);
    headers = {{"content-type", "application/json"}};
    request_body = build_gemini_request(image_base64s, instruction_text);
  } else {
    url = local_config.base_url + "/api/chat";
    headers = {{"content-type", "application/json"}};
    request_body = build_local_request(image_base64s, instruction_text, local_config.model, local_json_schema);
  }

  // 跟 core/browse/prefetch.cpp 往 stderr 打 hit/miss/wait_ms 同一个惯
  // 例——`pzt open --debug` 会把 stderr 收进屏幕底部的 debug 面板，不开
  // --debug 时静默丢弃，不是新的日志机制。这里特意打人话 prompt 而不是
  // request_body.dump()（那个里面混着几十/几百 KB 的 base64 图片数据，
  // 糊一屏没法看)。debug 面板一行的可见宽度(终端一行的显示列数)比
  // compact_for_debug_log 的 240 字符上限窄得多——instruction_text 是
  // schema_instruction 在前、user_prompt(实际任务描述)在后拼出来的(这
  // 个顺序是发给模型的真实 prompt 的一部分,不因为日志好不好看而改)，如
  // 果直接把 instruction_text 整个丢进日志，schema_instruction 这段几
  // 乎每次都一样的固定文案就能把这一行唯一可见的部分占满，真正随请求变
  // 化、更想看的 user_prompt 反而一个字都露不出来。日志里单独换个顺序
  // (user_prompt 在前)，不影响真实发给模型的 instruction_text。
  const char* provider_name = to_string(provider);
  std::string debug_prompt = "task: " + user_prompt + " | schema: " + schema_instruction;
  std::fprintf(stderr, "[pzt ai] request (%s) prompt: %s\n", provider_name,
               detail::compact_for_debug_log(debug_prompt).c_str());
  std::fflush(stderr);

  auto http_result = http_post(url, headers, request_body.dump());
  if (!http_result.ok()) return Result<nlohmann::json, RequestError>::Err(http_result.error());

  const auto& response = http_result.value();
  std::fprintf(stderr, "[pzt ai] response (%s) status=%ld body: %s\n", provider_name,
               response.status_code, detail::compact_for_debug_log(response.body).c_str());
  std::fflush(stderr);

  if (response.status_code < 200 || response.status_code >= 300) {
    return Result<nlohmann::json, RequestError>::Err(RequestError::HttpError);
  }

  if (provider == Provider::Claude) return parse_claude_response(response.body);
  if (provider == Provider::Gemini) return parse_gemini_response(response.body);
  return parse_local_response(response.body);
}

Result<nlohmann::json, RequestError> request_json(const decode::DecodedImage& image,
                                                    const std::string& user_prompt,
                                                    const std::string& schema_instruction,
                                                    Provider provider, HttpPostFn http_post,
                                                    const LocalModelConfig& local_config,
                                                    const std::optional<nlohmann::json>& local_json_schema) {
  return request_json(std::vector<decode::DecodedImage>{image}, user_prompt, schema_instruction,
                      provider, std::move(http_post), local_config, local_json_schema);
}

}  // namespace pzt::core::ai
