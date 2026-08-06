#pragma once

#include <string>

#include "core/ai/ai.h"
#include "core/decode/decode.h"
#include "core/result.h"

// 选片辅助评估——core::ai::request_json 通用层的消费者。曝光、构图、对焦
// 三个技术维度各自打分，不涉及色彩、情绪表达这类风格判断，见
// docs/history/M3_PRD.md 与 docs/history/M3_Eng_Design.md。取代了 M3 增
// 量一最初那版"审美评分"
// （core/ai/score.h，已删除）。
namespace pzt::core::ai {

// ImageUnavailable：F-03 新增，覆盖 EvaluationWorker::process_request 里
// "还没走到真正发起 AI 请求这一步就失败"的几种情况(图片记录/项目找不
// 到、预览图解码失败)——这几种失败原来只打 stderr，用户完全看不到；跟
// request_evaluation 本身可能失败的几种原因（网络/key/解析）合并成同一
// 个错误类型，方便 EvaluationWorker::FailureReport 统一携带，不需要为
// "发请求之前" vs "发请求之后"两类失败分别设计上报通道。
// StorageFailed：F-17 新增，覆盖"AI 已经给出结果，但落库这一步失败"
// (磁盘满、库损坏这类不该发生的场景)——原来这里的 sqlite3_step 返回值
// 不检查，失败会静默发生，generation_ 照样 +1 触发一次什么都没变的空
// 重绘，用户毫无察觉。跟 ImageUnavailable 是不同性质的失败(那个是"请
// 求都没发出去"，这个是"请求成功了但结果丢了")，值得用不同的原因文
// 案区分。
// DatabaseUnavailable：T-7 新增，是 process_request 那一层 catch-all 的落
// 点，覆盖 worker 线程上任何逃逸的异常,库的 schema 版本比程序新
// (SchemaTooNewError)、库损坏、磁盘满导致 db::Stmt 构造失败等等。这些原来
// 一律是 std::terminate：process_request 跑在后台 jthread 上，而 open_at
// 和所有 DAO 风格的函数遇到 sqlite 失败都是 throw，没有任何一层接住。跟
// StorageFailed 分开是因为那个的契约是"AI 已经给出结果、只是落库失败"，
// 而这里多数情况连请求都还没发出去；跟 ImageUnavailable 分开是因为那个是
// 逐图的条件，而库的问题是全局的，复用它会让 N 张图各自报一句"这张图暂
// 时评估不了"，把一个装机级的问题伪装成 N 个图片级的问题。
enum class EvaluationError { MissingApiKey, NetworkError, HttpError, ParseError,
                              ImageUnavailable, StorageFailed, DatabaseUnavailable };

// assessment 用哪种语言写——W2026-07-21：assessment 是直接展示给 pzt 用户
// 看的文字，始终用界面语言写(cli 按当前界面语言 g_lang 映射后传进来)，不
// 跟随 extra_guidance 切换。core 不认识 cli::i18n，只接一个中性的语言枚举。
enum class Language { Chinese, English };

// 纯模型输出——不含 extra_guidance/provider，那两个不是模型返回的，是发
// 起请求时调用方已经知道的上下文。W2026-07-21：eval 从三维技术打分改成
// "一段客观文字描述(构图/色彩/对焦/摄影审美) + 一个是否存在硬伤而不可用
// 的 flag"，涉及质量比较的选择改走锦标赛，见 docs/W2026-07-21_*。
//
// content 是 2026-08 意图驱动跨簇选片增量新增的第三个字段(PRD 决策四)：
// assessment 覆盖的是"拍得怎么样"(构图/色彩/对焦/摄影审美)，回答不了"画
// 面里有什么"，而后者是跨簇选片与文案生成唯一的输入 - 那次 LLM 调用手里
// 只有文字描述、没有照片。两者**刻意不合并**，理由有二：(1) TUI 的"AI 点
// 评"面板按显示宽度硬换行且装不下的部分静默不画，合并会让超出的内容无声
// 消失；(2) 坐在 TUI 前的人正看着照片，不需要"这张照片里有什么"的文字复
// 述 - content 只为看不见照片的机器存在。因此 **content 不进 TUI**，
// assessment 的语义、长度与显示一字不变。
struct EvaluationResult {
  std::string assessment;  // 一段简练客观的文字评价
  bool unusable;           // 是否存在硬伤(严重失焦/欠过曝等)导致根本不可用
  std::string content;     // 画面内容：发生了什么、有谁、在哪、什么氛围
};

// 落库/读回用的完整形状——比 EvaluationResult 多两个字段(extra_guidance/
// provider)，见 core/project/project.h 的 ImageInfo::evaluation。
struct EvaluationInfo {
  std::string assessment;
  bool unusable;
  std::string extra_guidance;
  std::string provider;
  // 读老记录(2026-08 之前落库的那些)时退化为空串而不是报错 - 库里
  // result_json 是 TEXT，读取侧的守卫只认 assessment/unusable，见
  // core/project/project.cpp。这跟下面 request_evaluation 对模型响应的
  // **严格**要求是两回事，别把两者搞混。
  std::string content;
};

// 取代原来的 passes_gate——不再从三维分数算达标，直接读模型给的 unusable
// flag。auto-reject 打废片、curate 候选、cli 信息栏可用性标记都调它，不各
// 自写 !unusable。
inline bool is_usable(const EvaluationInfo& info) { return !info.unusable; }

// extra_guidance:用户在 `:` 里输入的原始文本，可能是空字符串。内部拼出
// 完整的评估任务描述——固定英文框架模板(要求模型产出一段客观文字，覆盖
// 构图/色彩/对焦/摄影审美，并判定 unusable)后面跟一段"Additional
// guidance: {extra_guidance}"(为空时省略)，作为 user_prompt 传给
// request_json；schema_instruction 描述 EvaluationResult 对应的 JSON 形
// 状(assessment + unusable + content)。assessment 与 content 的输出语言:始终
// 用 language 参数指定的界面语言(见 Language 枚举)，不跟随 guidance。取
// assessment(string)、unusable(bool) 和 content(string)，任一取不到或类型不对
// 整体算失败(ParseError)。
//
// content 与另外两个字段**同等严格**，不宽松退化为空串：
// image_evaluations 以 image_id 为主键，缓存判据是"有评估记录就跳过"(PRD
// 决策七，不做字段完整性检查也不加版本号)，所以一条 content 为空的记录
// 之后永远不会被刷新，是不报错的哑数据。整条算失败、不写库，下次 run 自
// 然会重跑这张图。
// RequestError 直接映射到同名的 EvaluationError。框架模板本身不展示给用户
// (是给模型的系统指令)，固定英文。
//
// 签名精确匹配 core/ai/evaluation_worker.h 里 EvaluationWorker::
// EvaluationFn 的类型(image/guidance/provider/language/local_config 5 个
// 参数)，可以直接当默认值用——带默认值不影响这一点：取函数地址转换成
// std::function 时看的是参数个数本身，不是能不能省略着调用。
Result<EvaluationResult, EvaluationError> request_evaluation(const decode::DecodedImage& image,
                                                               const std::string& extra_guidance,
                                                               Provider provider,
                                                               Language language = Language::Chinese,
                                                               const LocalModelConfig& local_config = LocalModelConfig{});

// 仅供单元测试使用——http_post 可注入，不需要真的连网络就能验证 prompt
// 拼接、字段提取、越界校验这些逻辑；上面的 request_evaluation 是这个函
// 数在默认 http_post 参数下的一层薄封装。放在 detail 里是为了在头文件层
// 面标出"这不是主 API，是给测试开的后门"——它不能直接当 EvaluationFn 的
// 默认值用(5 个参数，EvaluationFn 期望 4 个)。
namespace detail {

Result<EvaluationResult, EvaluationError> request_evaluation_impl(const decode::DecodedImage& image,
                                                                    const std::string& extra_guidance,
                                                                    Provider provider,
                                                                    HttpPostFn http_post,
                                                                    Language language = Language::Chinese,
                                                                    const LocalModelConfig& local_config = LocalModelConfig{});

}  // namespace detail

}  // namespace pzt::core::ai
