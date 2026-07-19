#include "core/api.h"

#include <utility>

#include "core/db/database.h"
#include "core/media/media.h"
#include "core/raw/raw.h"

namespace pzt::core {

Settings load_settings() { return settings::load(); }

Result<ProjectId, CreateProjectError> create_project(const std::string& name,
                                                      const std::string& folder_path,
                                                      bool support_raw,
                                                      ScanProgressFn on_progress) {
  db::Database db = db::Database::open_default();
  return project::create_project(db, name, folder_path, support_raw, std::move(on_progress));
}

std::vector<ProjectSummary> list_projects() {
  db::Database db = db::Database::open_default();
  return project::list_projects(db);
}

std::optional<ProjectId> find_project_by_name(const std::string& name) {
  db::Database db = db::Database::open_default();
  return project::find_project_by_name(db, name);
}

std::optional<ProjectId> find_project_by_root_path(const std::string& path) {
  db::Database db = db::Database::open_default();
  return project::find_project_by_root_path(db, path);
}

Result<ProjectSummary, ProjectNotFoundError> open_project(ProjectId id) {
  db::Database db = db::Database::open_default();
  return project::open_project(db, id);
}

Result<void, ProjectNotFoundError> archive_project(ProjectId id) {
  db::Database db = db::Database::open_default();
  return project::archive_project(db, id);
}

Result<void, ProjectNotFoundError> delete_project(ProjectId id) {
  db::Database db = db::Database::open_default();
  return project::delete_project(db, id);
}

std::optional<ImageInfo> get_image(ImageId image_id) {
  db::Database db = db::Database::open_default();
  return project::get_image(db, image_id);
}

std::optional<ImageId> find_image_by_path(ProjectId project_id, const std::string& relative_path) {
  db::Database db = db::Database::open_default();
  return project::find_image_by_path(db, project_id, relative_path);
}

std::unordered_set<ImageId> evaluated_image_ids(const std::vector<ImageId>& image_ids) {
  db::Database db = db::Database::open_default();
  return project::evaluated_image_ids(db, image_ids);
}

Result<TagId, CreateTagError> create_tag(ProjectId project_id, const std::string& name,
                                          std::optional<std::int64_t> cap, bool is_ordered) {
  db::Database db = db::Database::open_default();
  return tagging::create_tag(db, project_id, name, cap, is_ordered);
}

std::vector<TagSummary> list_tags(ProjectId project_id) {
  db::Database db = db::Database::open_default();
  return tagging::list_tags(db, project_id);
}

std::optional<TagId> find_tag_by_name(ProjectId project_id, const std::string& name) {
  db::Database db = db::Database::open_default();
  return tagging::find_tag_by_name(db, project_id, name);
}

std::vector<TagSummary> tags_for_image(ImageId image_id) {
  db::Database db = db::Database::open_default();
  return tagging::tags_for_image(db, image_id);
}

std::unordered_set<ImageId> images_with_tag(const std::vector<ImageId>& image_ids, TagId tag_id) {
  db::Database db = db::Database::open_default();
  return tagging::images_with_tag(db, image_ids, tag_id);
}

Result<void, AddTagError> add_tag(ImageId image_id, TagId tag_id) {
  db::Database db = db::Database::open_default();
  return tagging::add_tag(db, image_id, tag_id);
}

Result<void, RemoveTagError> remove_tag(ImageId image_id, TagId tag_id) {
  db::Database db = db::Database::open_default();
  return tagging::remove_tag(db, image_id, tag_id);
}

Result<void, ReplaceTagError> replace_tag_entry(TagId tag_id, ImageId old_image, ImageId new_image) {
  db::Database db = db::Database::open_default();
  return tagging::replace_tag_entry(db, tag_id, old_image, new_image);
}

Result<void, DeleteTagError> delete_tag(TagId tag_id) {
  db::Database db = db::Database::open_default();
  return tagging::delete_tag(db, tag_id);
}

TagId ensure_reject_tag(ProjectId project_id) {
  db::Database db = db::Database::open_default();
  return tagging::ensure_reject_tag(db, project_id);
}

Result<DedupSummary, ProjectNotFoundError> find_and_tag_duplicates(
    ProjectId project_id, const std::vector<ImageId>& image_ids, int time_window_seconds,
    int hash_threshold, dedup::DedupProgressFn on_progress) {
  db::Database db = db::Database::open_default();
  return dedup::find_and_tag_duplicates(db, project_id, image_ids, time_window_seconds, hash_threshold,
                                         std::move(on_progress));
}

CurateResult curate_images(ProjectId project_id, std::optional<TagId> candidate_scope, int count,
                            int time_window_seconds, int hash_threshold) {
  db::Database db = db::Database::open_default();
  return curate::curate(db, project_id, candidate_scope, count, time_window_seconds, hash_threshold);
}

Result<RescanSummary, ProjectNotFoundError> rescan_project(ProjectId project_id, bool prune,
                                                             bool support_raw,
                                                             ScanProgressFn on_progress) {
  db::Database db = db::Database::open_default();
  return project::rescan_project(db, project_id, prune, support_raw, std::move(on_progress));
}

std::vector<ImageRef> list_images(ProjectId project_id) {
  db::Database db = db::Database::open_default();
  return browse::list_images(db, project_id);
}

std::optional<ImageId> next_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id) {
  return browse::next_image(images, current_id);
}

std::optional<ImageId> prev_image(const std::vector<ImageRef>& images,
                                   std::optional<ImageId> current_id) {
  return browse::prev_image(images, current_id);
}

std::optional<ImageId> next_untagged(const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id) {
  db::Database db = db::Database::open_default();
  return browse::next_untagged(db, images, current_id);
}

std::optional<ImageId> prev_untagged(const std::vector<ImageRef>& images,
                                      std::optional<ImageId> current_id) {
  db::Database db = db::Database::open_default();
  return browse::prev_untagged(db, images, current_id);
}

Result<std::vector<ImageRef>, BrowseTagError> filter_by_tag(TagId tag_id) {
  db::Database db = db::Database::open_default();
  return browse::filter_by_tag(db, tag_id);
}

Result<ExportResult, ExportTagError> export_tag(TagId tag_id, const std::string& output_folder,
                                                 ExportProgressFn on_progress, bool include_reject,
                                                 bool include_dup) {
  db::Database db = db::Database::open_default();
  return exporting::export_tag(db, tag_id, output_folder, std::move(on_progress), raw::decode_full,
                                include_reject, include_dup);
}

Result<ExportResult, ExportImagesError> export_images(ProjectId project_id,
                                                        const std::vector<ImageId>& image_ids,
                                                        const std::string& output_folder,
                                                        ExportProgressFn on_progress, bool include_reject,
                                                        bool include_dup) {
  db::Database db = db::Database::open_default();
  return exporting::export_images(db, project_id, image_ids, output_folder, std::move(on_progress),
                                   raw::decode_full, include_reject, include_dup);
}

Result<ExportImageResult, ExportImageError> export_image(ImageId image_id,
                                                           const std::string& output_folder,
                                                           ExportProgressFn on_progress) {
  db::Database db = db::Database::open_default();
  return exporting::export_image(db, image_id, output_folder, std::move(on_progress));
}

Result<DecodedImage, DecodeError> decode_jpeg_file(const std::string& path) {
  return decode::decode_jpeg_file(path);
}

Result<DecodedImage, DecodeError> resize_rgba(const DecodedImage& src, int target_width,
                                               int target_height) {
  return decode::resize_rgba(src, target_width, target_height);
}

Result<void, EncodeError> encode_jpeg_file(const DecodedImage& img, const std::string& path,
                                            double quality) {
  return decode::encode_jpeg_file(img, path, quality);
}

Result<DecodedImage, DecodeError> decode_preview_file(const std::string& path) {
  return media::decode_preview_file(path);
}

std::vector<PresetSummary> list_presets() {
  db::Database db = db::Database::open_default();
  recipe::ensure_default_presets(db);
  return recipe::list_presets(db);
}

std::vector<VersionSummary> list_versions(RecipeId preset_id) {
  db::Database db = db::Database::open_default();
  recipe::ensure_default_presets(db);
  return recipe::list_versions(db, preset_id);
}

Result<RecipeId, CreateVersionError> create_version(RecipeId preset_id,
                                                     std::optional<std::string> name,
                                                     VersionParams params) {
  db::Database db = db::Database::open_default();
  return recipe::create_version(db, preset_id, std::move(name), params);
}

Result<void, RecipeOpError> rename_version(RecipeId version_id, const std::string& new_name) {
  db::Database db = db::Database::open_default();
  return recipe::rename_version(db, version_id, new_name);
}

Result<void, RecipeOpError> delete_version(RecipeId version_id) {
  db::Database db = db::Database::open_default();
  return recipe::delete_version(db, version_id);
}

Result<void, SetImageRecipeError> set_image_recipe(ImageId image_id,
                                                    std::optional<RecipeId> recipe_id) {
  db::Database db = db::Database::open_default();
  return recipe::set_image_recipe(db, image_id, recipe_id);
}

std::optional<RecipeId> get_image_recipe(ImageId image_id) {
  db::Database db = db::Database::open_default();
  return recipe::get_image_recipe(db, image_id);
}

std::optional<RecipeDescription> describe_recipe(RecipeId recipe_id) {
  db::Database db = db::Database::open_default();
  return recipe::describe_recipe(db, recipe_id);
}

Result<DecodedImage, RenderRecipeError> render(const DecodedImage& src, RecipeId recipe_id,
                                                unsigned thread_count) {
  db::Database db = db::Database::open_default();
  return recipe::render(db, src, recipe_id, thread_count);
}

}  // namespace pzt::core
