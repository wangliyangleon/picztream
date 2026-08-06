#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/menu/tag_menu.h"
#include "core/api.h"

namespace fs = std::filesystem;
using namespace pzt::cli::menu;

namespace {

// core 的门面(core/api.cpp)每次调用都 open_default(),库路径由
// XDG_CONFIG_HOME 决定(core/db/database.cpp::default_db_path)。这里把它指
// 到一次性临时目录,让这组用例跟开发者真实的 pzt.db 完全隔离-同一个
// cli_tests 二进制里 i18n_test.cpp 也在改这个变量,所以用完必须还原。
class ScopedConfigHome {
 public:
  explicit ScopedConfigHome(const std::string& tag) {
    const char* old = std::getenv("XDG_CONFIG_HOME");
    had_old_ = old != nullptr;
    if (had_old_) old_ = old;

    dir_ = fs::temp_directory_path() / "pzt_test" / ("cli_" + tag);
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
  }

  ~ScopedConfigHome() {
    if (had_old_) {
      setenv("XDG_CONFIG_HOME", old_.c_str(), 1);
    } else {
      unsetenv("XDG_CONFIG_HOME");
    }
  }

  ScopedConfigHome(const ScopedConfigHome&) = delete;
  ScopedConfigHome& operator=(const ScopedConfigHome&) = delete;

 private:
  fs::path dir_;
  std::string old_;
  bool had_old_ = false;
};

// 建一个只有一张图片的项目-这组用例只关心标签数量,图片本身不参与断言,
// 但 create_project 要求文件夹里至少扫得到一张。
pzt::core::ProjectId make_project(const std::string& tag) {
  auto photos = fs::temp_directory_path() / "pzt_test" / ("cli_photos_" + tag);
  fs::remove_all(photos);
  fs::create_directories(photos);
  std::ofstream(photos / "001.jpg", std::ios::binary) << std::string(10, 'x');

  auto created = pzt::core::create_project(tag, photos.string(), /*support_raw=*/false, nullptr);
  REQUIRE(created.ok());
  return created.value();
}

void create_tags(pzt::core::ProjectId project_id, int count) {
  for (int i = 0; i < count; ++i) {
    auto r = pzt::core::create_tag(project_id, "tag" + std::to_string(i), std::nullopt,
                                   /*is_ordered=*/false);
    REQUIRE(r.ok());
  }
}

}  // namespace

TEST_CASE("tags_for_menu 不到上限时全部显示,没有隐藏项,也不算满") {
  ScopedConfigHome guard("menu_tags_under");
  auto project_id = make_project("menu_tags_under");
  create_tags(project_id, 3);

  auto menu = tags_for_menu(project_id);
  CHECK(menu.shown.size() == 3);
  CHECK(menu.hidden == 0);
  CHECK(menu.at_limit == false);
}

TEST_CASE("tags_for_menu 正好到上限时全部显示,没有隐藏项,但已经算满") {
  ScopedConfigHome guard("menu_tags_at");
  auto project_id = make_project("menu_tags_at");
  create_tags(project_id, static_cast<int>(kMaxMenuTags));

  auto menu = tags_for_menu(project_id);
  CHECK(menu.shown.size() == kMaxMenuTags);
  CHECK(menu.hidden == 0);
  CHECK(menu.at_limit == true);
}

// T-24 的核心场景:老项目(在"建到上限就挡住"之前建的)可能已经有 8 个以上
// 标签。截断必须保留,但超出的数量要报出来,不能像以前那样静默丢掉。
TEST_CASE("tags_for_menu 超过上限时截断,并报出被藏起来的数量") {
  ScopedConfigHome guard("menu_tags_over");
  auto project_id = make_project("menu_tags_over");
  create_tags(project_id, static_cast<int>(kMaxMenuTags) + 3);

  auto menu = tags_for_menu(project_id);
  CHECK(menu.shown.size() == kMaxMenuTags);
  CHECK(menu.hidden == 3);
  CHECK(menu.at_limit == true);
}

// 编号必须按 tag id 升序固定:被截断掉的永远是最后建的那几个,而不是让
// 已有标签的数字悄悄错位(tags_for_menu 的既有约定)。
TEST_CASE("tags_for_menu 截断掉的是最后建的,前 8 个编号不动") {
  ScopedConfigHome guard("menu_tags_order");
  auto project_id = make_project("menu_tags_order");
  create_tags(project_id, static_cast<int>(kMaxMenuTags) + 2);

  auto menu = tags_for_menu(project_id);
  REQUIRE(menu.shown.size() == kMaxMenuTags);
  for (std::size_t i = 0; i < kMaxMenuTags; ++i) {
    CHECK(menu.shown[i].name == "tag" + std::to_string(i));
  }
}

// 系统标签(废片/重复)不占动态编号,也不该被算进上限-否则跑过一次
// /dedup 之后,能建的标签数会凭空少一个。
TEST_CASE("tags_for_menu 不把系统标签算进上限") {
  ScopedConfigHome guard("menu_tags_system");
  auto project_id = make_project("menu_tags_system");
  create_tags(project_id, static_cast<int>(kMaxMenuTags) - 1);

  auto menu = tags_for_menu(project_id);
  CHECK(menu.shown.size() == kMaxMenuTags - 1);
  CHECK(menu.at_limit == false);
  for (const auto& t : menu.shown) {
    CHECK(t.is_system == false);
  }
}
