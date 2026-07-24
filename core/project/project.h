#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/ai/evaluation.h"
#include "core/db/database.h"
#include "core/result.h"

// 项目导入模块。见 docs/M0_Eng_Design.md "core/project" 模块划分与
// core/api 接口设计。这些函数接收一个已经打开的 Database&，方便单元测试
// 指向临时测试库而不碰真实的 ~/.config/pzt/pzt.db；cli 实际使用的入口是
// core/api.h 里委托到这里、内部自己打开默认库的门面函数。
namespace pzt::core::project {

using ProjectId = std::int64_t;
using ImageId = std::int64_t;

enum class CreateProjectError {
  NameAlreadyExists,
  NoImagesFound,
};

struct ProjectSummary {
  ProjectId id;
  std::string name;
  std::string root_path;
  std::int64_t image_count;
  bool archived;
  bool support_raw;  // 见 docs/RAW_Support.md：默认关闭的 opt-in 标记，一旦打开不会自动关闭
  std::optional<ImageId> last_image_id;  // F-24 会话续点：上次浏览到的那张图,无则 nullopt
};

// 每处理完一张需要生成 RAW 预览缓存的图片调用一次(done, total)。只有这次
// 扫描/rescan 确实有 RAW 图片需要生成缓存时才会被调用——纯 JPEG 项目、或
// 者所有 RAW 图片本来就已经有缓存，不触发任何调用。见 M2_Eng_Design.md
// "RAW 预览缓存"。
using ScanProgressFn = std::function<void(int done, int total)>;

// 递归扫描 folder_path 下所有 .jpg/.jpeg，support_raw=true 时额外识别
// .dng/.raf（大小写不敏感），写入 images 表。support_raw=false（默认）时
// RAW 文件完全不参与扫描——见 docs/RAW_Support.md，这是这个功能默认关闭
// 的核心：不传这个参数时代码路径跟 M0/M1 时代完全一样，同名 JPEG 不会因
// 为文件夹里有 RAW 而被忽略。support_raw=true 时，同一目录下文件名主干相
// 同的 JPEG + RAW 同时存在只认 RAW，那份 JPEG 被忽略、不生成记录
// （M2_Eng_Design.md"RAW+JPEG 同名"）。每张 RAW 图片会额外触发一次预览缓
// 存生成（half_size LibRaw 解码 + 编码 JPEG，写进 PZT 自己的数据目录，不
// 进用户照片文件夹），耗时不再是纯文件系统扫描那么快，on_progress 非空时
// 会汇报进度。名字已存在或扫描不到任何图片时返回对应错误，不创建项目。
Result<ProjectId, CreateProjectError> create_project(db::Database& db,
                                                      const std::string& name,
                                                      const std::string& folder_path,
                                                      bool support_raw = false,
                                                      ScanProgressFn on_progress = nullptr);

// 未归档项目在前，归档项目排最后；同组内按名字排序。
std::vector<ProjectSummary> list_projects(db::Database& db);

enum class ProjectNotFoundError {
  NotFound,
};

std::optional<ProjectId> find_project_by_name(db::Database& db, const std::string& name);
std::optional<ProjectId> find_project_by_root_path(db::Database& db, const std::string& path);

// "打开"这个 increment 里只是重新查一遍摘要返回给 cli 打印，不产生真正的
// 浏览会话状态——那是浏览模块（increment 4）要引入的东西。
Result<ProjectSummary, ProjectNotFoundError> open_project(db::Database& db, ProjectId id);

// 幂等：对已归档项目重复调用只是更新时间戳，不当错误处理。
Result<void, ProjectNotFoundError> archive_project(db::Database& db, ProjectId id);

// archive_project 的对称逆操作：把 archived_at 清回 NULL。幂等（对未归档项
// 目重复调用也是成功的 no-op），id 不存在时返回 NotFound——判据跟
// archive_project 一致（UPDATE 命中 0 行）。
Result<void, ProjectNotFoundError> unarchive_project(db::Database& db, ProjectId id);

// 级联清除该项目的 images/tags/image_tags（靠 schema 的 ON DELETE CASCADE），
// 不触碰磁盘上的原始文件。
Result<void, ProjectNotFoundError> delete_project(db::Database& db, ProjectId id);

// F-24 会话续点：记住这个项目上次浏览到的那张图(cmd_open 退出时写),重开时
// 若该 id 仍在图片列表里就从它起步。只是一条 UPDATE,不校验 image_id 是否
// 存在——成员检查交给读取方(打开时用当前图片列表兜)。
void set_last_image_id(db::Database& db, ProjectId id, ImageId image_id);

struct ImageInfo {
  ImageId id;
  ProjectId project_id;
  std::string file_path;
  std::string file_name;
  std::int64_t file_size;
  std::string kind;                              // "jpeg" | "raw"
  std::optional<std::string> preview_cache_path;  // kind="raw" 且缓存已生成时有值(绝对路径)
  std::optional<std::int64_t> captured_at;        // 拍摄时间(Unix 秒数)，提取失败/没有这个信息时为空
  // M3 选片辅助评估（曝光/构图/对焦），见 docs/M3_Eng_Design.md"数据库
  // Schema 设计"一节——存在 image_evaluations 表（一对一，LEFT JOIN 进
  // 这次查询），不是这张表自己的列。要么整个有值（评估过）要么整个是
  // nullopt（没评估过/评估失败），不是"部分字段有值部分没有"的语义。
  std::optional<ai::EvaluationInfo> evaluation;
};

// 给 cli 调试命令把"图片相对路径"翻译成内部 id 用。
std::optional<ImageId> find_image_by_path(db::Database& db, ProjectId project_id,
                                           const std::string& relative_path);

// 给标签模块校验"这张图属于哪个项目"用。
std::optional<ImageInfo> get_image(db::Database& db, ImageId id);

// F-07：批量查询 image_ids 里哪些已经有评估结果——一条 `WHERE image_id
// IN (...)` 查询，不是对每张图各调一次 get_image()。`/ai_eval`/`/dedup`
// 批量命令原来各自逐张 get_image 判断"评估过没有"，大项目(几百到几千
// 张)按一次键就是几百到几千次数据库往返。按 500 张分块(SQLite 变量
// 数上限保守值，同时呼应 F-40)，返回值只包含 image_ids 里真的有评估
// 结果的那些 id，调用方用 `count()` 判断某张图评估过没有。
std::unordered_set<ImageId> evaluated_image_ids(db::Database& db,
                                                 const std::vector<ImageId>& image_ids);

struct RescanSummary {
  std::int64_t added_count;
  std::int64_t removed_count;
  std::int64_t total_count;
  std::int64_t upgraded_count;  // M2：把已有的 kind="jpeg" 记录原地升级成 kind="raw" 的数量
};

// 复用 create_project 内部的扫描逻辑，把磁盘上有、但 images 表里还没有的
// 文件插进去。prune（默认 true）时还会清掉 images 表里有、但磁盘上已经找
// 不到对应文件的记录——级联清掉这些图片打过的标签（ON DELETE CASCADE），
// 如果该行有预览缓存文件也一并删除，不留孤儿文件。这条清理默认打开是刻
// 意的决定：早期版本"只增不减"是为了防止外置硬盘没挂载时把整个项目的标
// 签悄悄清空，但实际使用中发现"删了照片、标签却清不掉"更让人困惑。
// prune=false 保留旧行为，留给明确知道自己在对一个可能暂时不完整的存储
// 位置跑 rescan 的场景用。
//
// support_raw（默认 false）：这次 rescan 要不要处理 RAW 文件，语义跟
// create_project 的同名参数一致，只看这次调用传入的值，不读项目持久化的
// 状态——见 docs/RAW_Support.md。false 时新出现的 RAW 文件不会被发现，但
// 已经存在的 kind="raw" 记录也不会被当成"磁盘上消失了"误删（prune 阶段
// 会跳过它们）。support_raw=true 时，rescan 结束会把项目的 support_raw
// 持久化标记置为 1（一旦打开不会自动关闭）。
//
// M2：如果磁盘上给一条已有的 kind="jpeg" 记录补上了同名 RAW 文件，这条记
// 录会被原地升级成 kind="raw"（file_path/file_name 换成 RAW 的），不会插
// 入重复记录（否则原来那条 JPEG 记录会在 prune 时被误判成"磁盘上已消
// 失"，连带丢失已经打过的标签/recipe），计入 upgraded_count——但这只在项
// 目此前已经是 support_raw 状态时成立。如果这次 rescan 是这个项目第一次
// 打开 RAW 支持（项目此前是 support_raw=false，这条 JPEG 记录是在完全不
// 知道 RAW 概念的情况下打的标签/建的 recipe），则不做原地升级，改成插入
// 一条独立的新记录（文件名带 "_raw" 后缀区分），原 JPEG 记录不受影响，计
// 入 added_count，见 docs/RAW_Support.md"Edge case"一节。反过来"先有 RAW
// 后补 JPEG"不需要处理——扫描阶段从一开始就会忽略新出现的同名 JPEG，见
// create_project 的说明。升级/新增的 RAW 图片都会触发预览缓存生成，
// on_progress 非空时汇报进度。
Result<RescanSummary, ProjectNotFoundError> rescan_project(db::Database& db, ProjectId id,
                                                             bool prune = true,
                                                             bool support_raw = false,
                                                             ScanProgressFn on_progress = nullptr);

}  // namespace pzt::core::project
