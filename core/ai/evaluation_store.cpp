#include "core/ai/evaluation_store.h"

#include <nlohmann/json.hpp>

#include "core/db/stmt.h"
#include "core/tagging/tagging.h"

namespace pzt::core::ai {

Result<void, EvaluationError> store_evaluation(db::Database& db, project::ImageId image_id,
                                                const EvaluationResult& result,
                                                const std::string& extra_guidance, Provider provider,
                                                bool auto_reject) {
  // result_json 存模型返回的原始形状(assessment/unusable/content)，不是拆开
  // 的列 - 以后再问模型要新的值,只用扩展 EvaluationResult + 这里的 json 字面
  // 量,不需要再来一次 core/db/schema.cpp 的破坏性表重建,见那边的注释。2026-08
  // 加 content 就是这个设计第一次兑现:没动 DDL、没 bump kSchemaVersion。
  std::string result_json = nlohmann::json{{"assessment", result.assessment},
                                            {"unusable", result.unusable},
                                            {"content", result.content}}
                                 .dump();
  db::Stmt stmt(db.handle(),
                "INSERT INTO image_evaluations (image_id, result_json, extra_guidance, provider) "
                "VALUES (?, ?, ?, ?) "
                "ON CONFLICT(image_id) DO UPDATE SET "
                "result_json = excluded.result_json, "
                "extra_guidance = excluded.extra_guidance, "
                "provider = excluded.provider;");
  sqlite3_bind_int64(stmt.get(), 1, image_id);
  sqlite3_bind_text(stmt.get(), 2, result_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 3, extra_guidance.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.get(), 4, to_string(provider), -1, SQLITE_TRANSIENT);
  // F-17：以前不检查这一步——AI 已经给出结果，但落库失败(磁盘满、库损
  // 坏)时会静默发生。不 throw 的理由见 evaluation_store.h。
  if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
    return Result<void, EvaluationError>::Err(EvaluationError::StorageFailed);
  }

  // auto_reject：结果落库之后，模型直接给的 unusable 为真时打废片标签
  // (W2026-07-21：判据从原来的 passes_gate 三项阈值改成读 unusable flag)。
  // 只在 unusable 时打标签，不做反向摘除(见 core/settings/settings.h 里的
  // 说明)。用已经打开的 db 连接直接调 tagging::，不经过 core/api.h 门面
  // (那边会各自开一条新连接，没必要)。
  if (auto_reject && result.unusable) {
    auto info = project::get_image(db, image_id);
    if (info) {
      auto reject_tag_id = tagging::ensure_reject_tag(db, info->project_id);
      (void)tagging::add_tag(db, image_id, reject_tag_id);
    }
  }
  return Result<void, EvaluationError>::Ok();
}

}  // namespace pzt::core::ai
