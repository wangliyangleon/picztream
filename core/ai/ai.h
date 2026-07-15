#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/decode/decode.h"
#include "core/result.h"

// 通用层：发起一次带图片的 AI 请求，按调用方描述的形状要求 JSON，把解析
// 出来的 JSON 原样交还。这一层不知道"score"是什么，也不假设、不校验结
// 果里有什么字段——审美评分（core/ai/score.h，下一个 increment）是这一
// 层的第一个消费者，不是这一层本身。见 docs/M3_Eng_Design.md"core/api
// 接口设计"一节。
namespace pzt::core::ai {

enum class Provider { Claude, Gemini, Local };

// 本地 Provider（Ollama）的连接信息——base_url/model 是"可调行为参数"，
// 不是 get_api_key() 那种"秘密"，走 Settings（core/settings/settings.h
// 新增的 ollama_base_url/ollama_model 字段）而不是环境变量。这里只放
// 默认值当占位，真正生效的值由调用方（cmd_eval/resolve_ai_provider）
// 读一次 Settings 后显式构造传入，core::ai 内部不直接读 Settings。
struct LocalModelConfig {
  std::string base_url = "http://localhost:11434";
  std::string model = "moondream";
};

// "claude" | "gemini" | "local"——落库(image_evaluations.provider)和 debug
// 日志共用同一份映射，不在两个地方各写一遍容易失步的三元表达式。
const char* to_string(Provider provider);

enum class RequestError {
  MissingApiKey,  // 对应的环境变量没设(ANTHROPIC_API_KEY / GEMINI_API_KEY)
  NetworkError,   // curl 请求本身失败(超时/连不上)
  HttpError,      // HTTP 状态码非 2xx
  ParseError,     // 响应体不是预期结构,或者模型没按要求只回 JSON
};

struct HttpResponse {
  long status_code = 0;
  std::string body;
};

// 真正发起 HTTP 请求这一步可注入——只对"这次连接本身有没有打通"负责,返回
// (status_code, body);curl 层面的失败(连不上、超时)才是 NetworkError,
// HTTP 状态码是不是 2xx 由 request_json 自己判断,不是这个函数的职责。默
// 认指向下面真实的 curl 实现,单元测试注入假函数,不需要真的连网络——照抄
// core::raw::RawDecodeFn/core::ai::EvaluationWorker::EvaluationFn 的依赖
// 注入先例。
using HttpPostFn = std::function<Result<HttpResponse, RequestError>(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body)>;

Result<HttpResponse, RequestError> perform_curl_post(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body);

// schema_instruction:调用方用自然语言描述这次要模型回什么样的 JSON(字段
// 名、类型、取值范围);user_prompt:调用方自己的任务描述。两者会被拼进一
// 段固定的"只回 JSON"系统层指令模板里发给模型,返回值是解析出来的 JSON
// 对象本身,调用方自己按需要的字段名去取。
Result<nlohmann::json, RequestError> request_json(const decode::DecodedImage& image,
                                                    const std::string& user_prompt,
                                                    const std::string& schema_instruction,
                                                    Provider provider,
                                                    HttpPostFn http_post = perform_curl_post,
                                                    const LocalModelConfig& local_config = LocalModelConfig{});

// 仅供单元测试使用——request_json 内部在编码上传之前会调用这个函数把图
// 片降采样到一个合理的上限(见 ai.cpp 里的说明:纯色测试图片压缩后几乎
// 不随分辨率变化，没法从 base64 载荷大小反推有没有真的缩小，直接测这
// 个函数的输出宽高更准确)。
namespace detail {

decode::DecodedImage downscale_for_upload(const decode::DecodedImage& image);

}  // namespace detail

}  // namespace pzt::core::ai
