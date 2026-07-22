#include "core/dedup/dedup.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "core/db/stmt.h"
#include "core/media/media.h"
#include "core/tagging/tagging.h"

namespace pzt::core::dedup {

namespace {

struct ImageMeta {
  project::ImageId id;
  std::int64_t captured_at;
  std::string file_path;
  std::string kind;
  std::optional<std::string> preview_cache_path;
};

std::vector<ImageMeta> load_metas(db::Database& db, const std::vector<project::ImageId>& image_ids) {
  if (image_ids.empty()) return {};

  // F-40：按 500 一批分块绑定，跟 tagging::images_with_tag / project::
  // evaluated_image_ids 同一个惯例（500 是 SQLITE_MAX_VARIABLE_NUMBER 之下的
  // 保守值）。3 万+ 张的项目全项目扫一次 dedup 时，单条 IN 把全部 id 绑进一
  // 条语句会超绑定变量上限直接建语句失败，这里逐块查询、累积结果，最后统
  // 一按 captured_at 排序。
  constexpr std::size_t kChunkSize = 500;
  std::vector<ImageMeta> metas;
  for (std::size_t offset = 0; offset < image_ids.size(); offset += kChunkSize) {
    std::size_t count = std::min(kChunkSize, image_ids.size() - offset);
    std::string placeholders;
    for (std::size_t i = 0; i < count; ++i) {
      if (i) placeholders += ",";
      placeholders += "?";
    }
    std::string sql = "SELECT id, captured_at, file_path, kind, preview_cache_path FROM images "
                       "WHERE id IN (" +
                       placeholders + ") AND captured_at IS NOT NULL;";
    db::Stmt stmt(db.handle(), sql.c_str());
    for (std::size_t i = 0; i < count; ++i) {
      sqlite3_bind_int64(stmt.get(), static_cast<int>(i) + 1, image_ids[offset + i]);
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
      ImageMeta m;
      m.id = sqlite3_column_int64(stmt.get(), 0);
      m.captured_at = sqlite3_column_int64(stmt.get(), 1);
      m.file_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 2));
      m.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 3));
      if (sqlite3_column_type(stmt.get(), 4) != SQLITE_NULL) {
        m.preview_cache_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 4));
      }
      metas.push_back(std::move(m));
    }
  }
  std::sort(metas.begin(), metas.end(),
            [](const ImageMeta& a, const ImageMeta& b) { return a.captured_at < b.captured_at; });
  return metas;
}

std::vector<std::vector<ImageMeta>> cluster_by_time(const std::vector<ImageMeta>& metas,
                                                      int time_window_seconds) {
  std::vector<std::vector<ImageMeta>> clusters;
  for (const auto& m : metas) {
    if (clusters.empty() || m.captured_at - clusters.back().back().captured_at > time_window_seconds) {
      clusters.push_back({m});
    } else {
      clusters.back().push_back(m);
    }
  }
  return clusters;
}

// 朴素并查集，路径压缩，簇的规模(连拍/包围曝光量级)不需要按秩合并这类
// 进一步优化。
class UnionFind {
 public:
  explicit UnionFind(int n) : parent_(n) { std::iota(parent_.begin(), parent_.end(), 0); }

  int find(int x) {
    if (parent_[x] != x) parent_[x] = find(parent_[x]);
    return parent_[x];
  }

  void unite(int a, int b) {
    a = find(a);
    b = find(b);
    if (a != b) parent_[a] = b;
  }

 private:
  std::vector<int> parent_;
};

// members 是 cluster 内的下标集合(size >= 2)。keep 选 captured_at 最新的
// 那张——涉及质量比较的选择统一走锦标赛(见 docs/W2026-07-21_*)，dedup 只
// 做"留最新"这个廉价的确定性基线，不再依赖选片评估分数。captured_at 也
// 相等的极端情况兜底选最小 image_id，保证确定性。
project::ImageId pick_keep_id(const std::vector<ImageMeta>& cluster,
                               const std::vector<std::size_t>& members) {
  project::ImageId keep_id = 0;
  std::optional<std::int64_t> best_time;
  for (auto idx : members) {
    std::int64_t captured_at = cluster[idx].captured_at;
    project::ImageId id = cluster[idx].id;
    if (!best_time || captured_at > *best_time || (captured_at == *best_time && id < keep_id)) {
      best_time = captured_at;
      keep_id = id;
    }
  }
  return keep_id;
}

}  // namespace

std::optional<ImageHash> compute_dhash(const decode::DecodedImage& image) {
  auto resized = decode::resize_rgba(image, 9, 8);
  // F-36：resize 失败以前返回哈希 0——一个完全合法的哈希值(全黑/全均匀图片
  // 就是 0),会被当成"跟其它 0 哈希的图重复"错误分组。现路径不可达(9x8 目
  // 标尺寸下 resize_rgba 只会直接返回原图拷贝,不会失败),纯防御:返回
  // nullopt,让调用方跟解码失败同路径跳过这张,而不是伪造一个哈希。
  if (!resized.ok()) return std::nullopt;
  const decode::DecodedImage& small = resized.value();

  auto luminance = [&](int x, int y) -> int {
    std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(small.width) +
                        static_cast<std::size_t>(x)) *
                       4;
    int r = small.rgba[idx];
    int g = small.rgba[idx + 1];
    int b = small.rgba[idx + 2];
    return (r * 299 + g * 587 + b * 114) / 1000;
  };

  ImageHash hash = 0;
  int bit = 0;
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      if (luminance(x, y) > luminance(x + 1, y)) hash |= (ImageHash{1} << bit);
      ++bit;
    }
  }
  return hash;
}

int hamming_distance(ImageHash a, ImageHash b) { return std::popcount(a ^ b); }

std::vector<project::ImageId> images_with_capture_time(db::Database& db,
                                                         const std::vector<project::ImageId>& image_ids) {
  std::vector<project::ImageId> result;
  result.reserve(image_ids.size());
  for (const auto& m : load_metas(db, image_ids)) result.push_back(m.id);
  return result;
}

std::vector<DuplicateGroup> find_duplicates(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& image_ids,
                                             int time_window_seconds, int hash_threshold,
                                             DedupProgressFn on_progress) {
  return detail::find_duplicates_impl(db, root_path, image_ids, time_window_seconds, hash_threshold,
                                       std::move(on_progress), media::decode_preview_file);
}

Result<DedupSummary, project::ProjectNotFoundError> find_and_tag_duplicates(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    int time_window_seconds, int hash_threshold, DedupProgressFn on_progress) {
  auto project_summary = project::open_project(db, project_id);
  if (!project_summary.ok()) {
    return Result<DedupSummary, project::ProjectNotFoundError>::Err(project_summary.error());
  }

  // W2026-07-21：聚类前排除废片。keep 改成"留最新"后，一张废片若
  // captured_at 最新会在簇内成为 keep、把它的好邻居打成重复；先摘掉带
  // 废片标签的图，只在剩余子集 dedup_ids 上聚类。清旧重复标记仍作用于
  // 完整的 image_ids(避免废片上残留旧的重复标签)，只有"聚类"这一步走
  // 排废片后的子集。跟 core/curate 的废片排除同一套(find_tag_by_name +
  // images_with_tag)。
  std::unordered_set<project::ImageId> reject_set;
  if (auto reject_tag = tagging::find_tag_by_name(db, project_id, tagging::kRejectTagName)) {
    auto tagged = tagging::images_with_tag(db, image_ids, *reject_tag);
    reject_set.insert(tagged.begin(), tagged.end());
  }
  std::vector<project::ImageId> dedup_ids;
  dedup_ids.reserve(image_ids.size());
  for (project::ImageId id : image_ids) {
    if (!reject_set.count(id)) dedup_ids.push_back(id);
  }

  // F-08：跟 find_duplicates 内部(经 find_duplicates_impl)会做的查询重
  // 复一遍——load_metas 只是一条按 id IN (...) 的索引查询，不是 N+1，
  // 多查一次换来"结果为什么比预期少"这个问题有观测手段，值得。
  // skipped_no_capture_time 只统计参与聚类的子集(废片不算)。
  int skipped_no_capture_time =
      static_cast<int>(dedup_ids.size()) - static_cast<int>(load_metas(db, dedup_ids).size());

  // 先摘光再重新打：只清 image_ids 这个范围内的旧标记，范围外的图片(比
  // 如全项目扫描之后又单独对某个标签跑了一次)不受影响，见
  // docs/M3_Dedup_PRD.md"重新运行"一节。remove_tag 是幂等的，图片本来
  // 没打过标签也不算错，不需要先查一遍"这张图有没有这个标签"。
  tagging::TagId duplicate_tag_id = tagging::ensure_duplicate_tag(db, project_id);
  for (project::ImageId id : image_ids) {
    // F-19：remove_tag 幂等，图片本来没这个标签也算成功(见上面的说
    // 明)；唯一的失败原因(TagNotFound/ImageNotFound)在这里结构上不会
    // 发生——tag_id 刚被 ensure_duplicate_tag 确认存在，id 来自调用方
    // 已经解析好的图片列表。显式 (void) 丢弃，而不是让 [[nodiscard]]
    // 警告一直挂在这里没人处理。
    (void)tagging::remove_tag(db, id, duplicate_tag_id);
  }

  auto groups = find_duplicates(db, project_summary.value().root_path, dedup_ids, time_window_seconds,
                                 hash_threshold, std::move(on_progress));

  int tagged_count = 0;
  for (const auto& group : groups) {
    for (project::ImageId id : group.image_ids) {
      if (id == group.keep_id) continue;
      // F-18：以前不检查 add_tag 的返回值，tagged_count 无条件自增。如
      // 果用户在这个功能之前就手动建过一个带 cap 的同名"重复"标签
      // (ensure_duplicate_tag 会直接复用它，不区分是不是系统创建的，
      // 见该函数的说明)，超出 cap 的图实际没打上标签，汇总却照样报"打
      // 了 N 张"——只在真的打上时才计数。
      if (tagging::add_tag(db, id, duplicate_tag_id).ok()) ++tagged_count;
    }
  }

  return Result<DedupSummary, project::ProjectNotFoundError>::Ok(
      DedupSummary{static_cast<int>(groups.size()), tagged_count, skipped_no_capture_time});
}

namespace detail {

std::vector<DuplicateGroup> find_duplicates_impl(db::Database& db, const std::string& root_path,
                                                  const std::vector<project::ImageId>& image_ids,
                                                  int time_window_seconds, int hash_threshold,
                                                  DedupProgressFn on_progress,
                                                  PreviewDecodeFn decode_fn) {
  auto metas = load_metas(db, image_ids);
  auto clusters = cluster_by_time(metas, time_window_seconds);

  std::vector<DuplicateGroup> result;
  int total = static_cast<int>(clusters.size());
  int done = 0;

  for (auto& cluster : clusters) {
    if (cluster.size() < 2) {
      ++done;
      if (on_progress) on_progress(done, total);
      continue;
    }

    std::vector<ImageHash> hashes(cluster.size());
    std::vector<bool> valid(cluster.size(), true);
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      std::string path = media::resolve_preview_path(root_path, cluster[i].file_path,
                                                      cluster[i].kind, cluster[i].preview_cache_path);
      auto decoded = decode_fn(path);
      if (!decoded.ok()) {
        std::fprintf(stderr, "[pzt dedup] decode failed, skipping image_id=%lld path=%s\n",
                     static_cast<long long>(cluster[i].id), path.c_str());
        valid[i] = false;
        continue;
      }
      // F-36：compute_dhash 现返回 optional,resize 失败(现路径不可达,纯防
      // 御)跟解码失败同路径跳过,不再伪造哈希 0 参与分组。
      auto hash = compute_dhash(decoded.value());
      if (!hash) {
        std::fprintf(stderr, "[pzt dedup] dhash failed, skipping image_id=%lld path=%s\n",
                     static_cast<long long>(cluster[i].id), path.c_str());
        valid[i] = false;
        continue;
      }
      hashes[i] = *hash;
    }

    UnionFind uf(static_cast<int>(cluster.size()));
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      if (!valid[i]) continue;
      for (std::size_t j = i + 1; j < cluster.size(); ++j) {
        if (!valid[j]) continue;
        int distance = hamming_distance(hashes[i], hashes[j]);
        // F-08：候选簇内每一对比较都打一行明细，不管有没有结成组——调
        // 参(时间窗/哈希阈值)时唯一能看到"差多少"的地方。走跟
        // core/browse/prefetch.cpp 同一个先例：无条件写 stderr，`pzt
        // open --debug` 才会把它路由进调试面板(cli/term/debug_log.h)，
        // 默认路径下 stderr 整个被重定向到 /dev/null，不产生任何可见
        // 开销。
        std::fprintf(stderr, "[pzt dedup] compare image_id=%lld image_id=%lld distance=%d threshold=%d\n",
                     static_cast<long long>(cluster[i].id), static_cast<long long>(cluster[j].id),
                     distance, hash_threshold);
        if (distance <= hash_threshold) {
          uf.unite(static_cast<int>(i), static_cast<int>(j));
        }
      }
    }

    std::unordered_map<int, std::vector<std::size_t>> groups_by_root;
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      if (!valid[i]) continue;
      groups_by_root[uf.find(static_cast<int>(i))].push_back(i);
    }

    // F-39：groups_by_root 是 unordered_map，遍历序不稳定，直接灌进 result
    // 会让 DuplicateGroup 的顺序跨进程运行不确定，违反 Dedup PRD 的确定性
    // NFR 字面（打标签集合本身一致，但输出顺序不定）。先把本簇的组收进局部
    // vector，按组内最小 id（group_ids 已升序，即 front）排序后再 append。
    // cluster 本身已按 captured_at 有序，整体输出即完全确定。
    std::vector<DuplicateGroup> cluster_groups;
    for (auto& [root, members] : groups_by_root) {
      if (members.size() < 2) continue;

      std::vector<project::ImageId> group_ids;
      group_ids.reserve(members.size());
      for (auto idx : members) group_ids.push_back(cluster[idx].id);
      std::sort(group_ids.begin(), group_ids.end());

      project::ImageId keep_id = pick_keep_id(cluster, members);
      cluster_groups.push_back(DuplicateGroup{std::move(group_ids), keep_id});
    }
    std::sort(cluster_groups.begin(), cluster_groups.end(),
              [](const DuplicateGroup& a, const DuplicateGroup& b) {
                return a.image_ids.front() < b.image_ids.front();
              });
    for (auto& g : cluster_groups) result.push_back(std::move(g));

    ++done;
    if (on_progress) on_progress(done, total);
  }

  return result;
}

}  // namespace detail

}  // namespace pzt::core::dedup
