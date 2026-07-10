#include "core/dedup/dedup.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <optional>
#include <unordered_map>
#include <utility>

#include "core/ai/evaluation.h"
#include "core/db/stmt.h"
#include "core/raw/raw.h"
#include "core/tagging/tagging.h"

namespace pzt::core::dedup {

namespace {

// M2：跟 core/api.cpp 里 has_raw_extension 判断的是同一件事(扩展名在
// {.dng, .raf} 内，大小写不敏感)，但那份是 api.cpp 匿名命名空间里的实现
// 细节，不对外暴露；这里不能反过来 #include "core/api.h"(api.h 的
// find_and_tag_duplicates 签名要用到 dedup::DedupProgressFn，反向包含会
// 循环)。三处加起来也就几行逻辑，不值得为此单独抽一个跨模块共享的小工
// 具函数——跟 project.cpp/api.cpp 已经各自维护一份的先例一致。
bool has_raw_extension(const std::string& path) {
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                  [](unsigned char c) { return std::tolower(c); });
  return ext == ".dng" || ext == ".raf";
}

// 跟 core/api.cpp 的 decode_preview_file 是同一套分发逻辑(.dng/.raf 走
// LibRaw 内嵌预览提取，其它按 JPEG 解码)，作为 find_duplicates 默认的
// PreviewDecodeFn。
Result<decode::DecodedImage, decode::DecodeError> default_decode_preview(const std::string& path) {
  if (!has_raw_extension(path)) {
    return decode::decode_jpeg_file(path);
  }
  auto bytes = raw::extract_embedded_jpeg_bytes(path);
  if (!bytes.ok()) {
    auto err = bytes.error() == raw::RawError::FileNotFound ? decode::DecodeError::FileNotFound
                                                              : decode::DecodeError::DecodeFailed;
    return Result<decode::DecodedImage, decode::DecodeError>::Err(err);
  }
  return decode::decode_jpeg_bytes(bytes.value());
}

// 跟 core/ai/evaluation_worker.cpp 的 resolve_path 是同一个逻辑(kind=raw
// 且缓存已生成时直接用缓存文件绝对路径，否则拼 root_path + file_path)，
// 各自维护一份，不共享——见上面 has_raw_extension 的说明,同样的理由。
std::string resolve_path(const std::string& root_path, const std::string& file_path,
                          const std::string& kind, const std::optional<std::string>& preview_cache_path) {
  if (kind == "raw" && preview_cache_path) return *preview_cache_path;
  return root_path + "/" + file_path;
}

struct ImageMeta {
  project::ImageId id;
  std::int64_t captured_at;
  std::string file_path;
  std::string kind;
  std::optional<std::string> preview_cache_path;
};

std::vector<ImageMeta> load_metas(db::Database& db, const std::vector<project::ImageId>& image_ids) {
  if (image_ids.empty()) return {};

  std::string placeholders;
  for (std::size_t i = 0; i < image_ids.size(); ++i) {
    if (i) placeholders += ",";
    placeholders += "?";
  }
  std::string sql = "SELECT id, captured_at, file_path, kind, preview_cache_path FROM images "
                     "WHERE id IN (" +
                     placeholders + ") AND captured_at IS NOT NULL;";
  db::Stmt stmt(db.handle(), sql.c_str());
  for (std::size_t i = 0; i < image_ids.size(); ++i) {
    sqlite3_bind_int64(stmt.get(), static_cast<int>(i) + 1, image_ids[i]);
  }

  std::vector<ImageMeta> metas;
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

// members 是 cluster 内的下标集合(size >= 2)。全员评估过按 overall_score
// 最高选，否则退化成按 captured_at 最新选；tie-break 统一选最小
// image_id，保证确定性。
project::ImageId pick_keep_id(db::Database& db, const std::vector<ImageMeta>& cluster,
                               const std::vector<std::size_t>& members) {
  std::vector<std::optional<project::ImageInfo>> infos;
  infos.reserve(members.size());
  bool all_evaluated = true;
  for (auto idx : members) {
    auto info = project::get_image(db, cluster[idx].id);
    if (!info || !info->evaluation) all_evaluated = false;
    infos.push_back(std::move(info));
  }

  project::ImageId keep_id = 0;
  if (all_evaluated) {
    std::optional<int> best_score;
    for (std::size_t k = 0; k < members.size(); ++k) {
      int score = ai::overall_score(*infos[k]->evaluation);
      project::ImageId id = cluster[members[k]].id;
      if (!best_score || score > *best_score || (score == *best_score && id < keep_id)) {
        best_score = score;
        keep_id = id;
      }
    }
  } else {
    std::optional<std::int64_t> best_time;
    for (std::size_t k = 0; k < members.size(); ++k) {
      std::int64_t captured_at = cluster[members[k]].captured_at;
      project::ImageId id = cluster[members[k]].id;
      if (!best_time || captured_at > *best_time || (captured_at == *best_time && id < keep_id)) {
        best_time = captured_at;
        keep_id = id;
      }
    }
  }
  return keep_id;
}

}  // namespace

ImageHash compute_dhash(const decode::DecodedImage& image) {
  auto resized = decode::resize_rgba(image, 9, 8);
  if (!resized.ok()) return 0;
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

std::vector<DuplicateGroup> find_duplicates(db::Database& db, const std::string& root_path,
                                             const std::vector<project::ImageId>& image_ids,
                                             int time_window_seconds, int hash_threshold,
                                             DedupProgressFn on_progress) {
  return detail::find_duplicates_impl(db, root_path, image_ids, time_window_seconds, hash_threshold,
                                       std::move(on_progress), default_decode_preview);
}

Result<DedupSummary, project::ProjectNotFoundError> find_and_tag_duplicates(
    db::Database& db, project::ProjectId project_id, const std::vector<project::ImageId>& image_ids,
    DedupProgressFn on_progress) {
  auto project_summary = project::open_project(db, project_id);
  if (!project_summary.ok()) {
    return Result<DedupSummary, project::ProjectNotFoundError>::Err(project_summary.error());
  }

  int unevaluated_image_count = 0;
  for (project::ImageId id : image_ids) {
    auto info = project::get_image(db, id);
    if (!info || !info->evaluation) ++unevaluated_image_count;
  }

  // 先摘光再重新打：只清 image_ids 这个范围内的旧标记，范围外的图片(比
  // 如全项目扫描之后又单独对某个标签跑了一次)不受影响，见
  // docs/M3_Dedup_PRD.md"重新运行"一节。remove_tag 是幂等的，图片本来
  // 没打过标签也不算错，不需要先查一遍"这张图有没有这个标签"。
  tagging::TagId duplicate_tag_id = tagging::ensure_duplicate_tag(db, project_id);
  for (project::ImageId id : image_ids) {
    tagging::remove_tag(db, id, duplicate_tag_id);
  }

  auto groups = find_duplicates(db, project_summary.value().root_path, image_ids,
                                 /*time_window_seconds=*/10, /*hash_threshold=*/5,
                                 std::move(on_progress));

  int tagged_count = 0;
  for (const auto& group : groups) {
    for (project::ImageId id : group.image_ids) {
      if (id == group.keep_id) continue;
      tagging::add_tag(db, id, duplicate_tag_id);
      ++tagged_count;
    }
  }

  return Result<DedupSummary, project::ProjectNotFoundError>::Ok(
      DedupSummary{static_cast<int>(groups.size()), tagged_count, unevaluated_image_count});
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
      std::string path = resolve_path(root_path, cluster[i].file_path, cluster[i].kind,
                                       cluster[i].preview_cache_path);
      auto decoded = decode_fn(path);
      if (!decoded.ok()) {
        std::fprintf(stderr, "[pzt dedup] decode failed, skipping image_id=%lld path=%s\n",
                     static_cast<long long>(cluster[i].id), path.c_str());
        valid[i] = false;
        continue;
      }
      hashes[i] = compute_dhash(decoded.value());
    }

    UnionFind uf(static_cast<int>(cluster.size()));
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      if (!valid[i]) continue;
      for (std::size_t j = i + 1; j < cluster.size(); ++j) {
        if (!valid[j]) continue;
        if (hamming_distance(hashes[i], hashes[j]) <= hash_threshold) {
          uf.unite(static_cast<int>(i), static_cast<int>(j));
        }
      }
    }

    std::unordered_map<int, std::vector<std::size_t>> groups_by_root;
    for (std::size_t i = 0; i < cluster.size(); ++i) {
      if (!valid[i]) continue;
      groups_by_root[uf.find(static_cast<int>(i))].push_back(i);
    }

    for (auto& [root, members] : groups_by_root) {
      if (members.size() < 2) continue;

      std::vector<project::ImageId> group_ids;
      group_ids.reserve(members.size());
      for (auto idx : members) group_ids.push_back(cluster[idx].id);
      std::sort(group_ids.begin(), group_ids.end());

      project::ImageId keep_id = pick_keep_id(db, cluster, members);
      result.push_back(DuplicateGroup{std::move(group_ids), keep_id});
    }

    ++done;
    if (on_progress) on_progress(done, total);
  }

  return result;
}

}  // namespace detail

}  // namespace pzt::core::dedup
