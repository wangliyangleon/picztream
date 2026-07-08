#include "core/api.h"

#include <utility>

#include "core/db/database.h"

namespace pzt::core {

Result<ProjectId, CreateProjectError> create_project(const std::string& name,
                                                      const std::string& folder_path,
                                                      ScanProgressFn on_progress) {
  db::Database db = db::Database::open_default();
  return project::create_project(db, name, folder_path, std::move(on_progress));
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

std::optional<ImageId> find_image_by_path(ProjectId project_id, const std::string& relative_path) {
  db::Database db = db::Database::open_default();
  return project::find_image_by_path(db, project_id, relative_path);
}

std::optional<ImageInfo> get_image(ImageId image_id) {
  db::Database db = db::Database::open_default();
  return project::get_image(db, image_id);
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

Result<RescanSummary, ProjectNotFoundError> rescan_project(ProjectId project_id, bool prune,
                                                             ScanProgressFn on_progress) {
  db::Database db = db::Database::open_default();
  return project::rescan_project(db, project_id, prune, std::move(on_progress));
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
                                                 LinkMode link_mode) {
  db::Database db = db::Database::open_default();
  return exporting::export_tag(db, tag_id, output_folder, link_mode);
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
