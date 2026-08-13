#include <doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/db/database.h"
#include "core/project/project.h"
#include "core/scope/scope.h"
#include "core/tagging/tagging.h"

namespace fs = std::filesystem;
using pzt::core::db::Database;
using pzt::core::project::create_project;
using pzt::core::project::find_image_by_path;
using pzt::core::project::ImageId;
using pzt::core::project::ProjectId;
using namespace pzt::core::tagging;
using namespace pzt::core::scope;

namespace {

// 跟 tournament_test.cpp / dedup_test.cpp 的同名 helper 一致，各测试文件
// 独立一份是这个代码库既有的惯例。
std::string fresh_db_path(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test";
  fs::create_directories(dir);
  auto path = (dir / ("scope_" + tag + ".db")).string();
  fs::remove(path);
  fs::remove(path + "-wal");
  fs::remove(path + "-shm");
  return path;
}

fs::path fresh_photo_dir(const std::string& tag) {
  auto dir = fs::temp_directory_path() / "pzt_test" / ("scope_" + tag);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f << "x";
}

struct Fixture {
  Database db;
  ProjectId project_id;
  std::vector<ImageId> images;  // images[0]="a.jpg", images[1]="b.jpg", ...
};

Fixture make_fixture(const std::string& tag, int image_count) {
  auto db = Database::open_at(fresh_db_path(tag));
  auto photos = fresh_photo_dir(tag);
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    touch(photos / (name + ".jpg"));
  }
  auto created = create_project(db, "proj", photos.string());
  REQUIRE(created.ok());

  std::vector<ImageId> images;
  for (int i = 0; i < image_count; ++i) {
    std::string name(1, static_cast<char>('a' + i));
    auto id = find_image_by_path(db, created.value(), name + ".jpg");
    REQUIRE(id.has_value());
    images.push_back(*id);
  }
  return Fixture{std::move(db), created.value(), std::move(images)};
}

TagId make_tag(Database& db, ProjectId project_id, const std::string& name) {
  auto created = create_tag(db, project_id, name, std::nullopt, /*is_ordered=*/false);
  REQUIRE(created.ok());
  return created.value();
}

bool contains(const std::vector<ImageId>& ids, ImageId id) {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST_CASE("resolve('*') covers the whole project and reports no scope tag") {
  auto fx = make_fixture("star", 3);

  auto result = resolve(fx.db, fx.project_id, "*");
  REQUIRE(result.ok());
  CHECK(result.value().image_ids.size() == 3);
  CHECK(!result.value().scope_tag.has_value());
}

TEST_CASE("resolve('#tag') covers only tagged images and reports the scope tag") {
  auto fx = make_fixture("tag", 3);
  auto keep = make_tag(fx.db, fx.project_id, "精选");
  REQUIRE(add_tag(fx.db, fx.images[0], keep).ok());
  REQUIRE(add_tag(fx.db, fx.images[2], keep).ok());

  auto result = resolve(fx.db, fx.project_id, "#精选");
  REQUIRE(result.ok());
  CHECK(result.value().image_ids.size() == 2);
  CHECK(contains(result.value().image_ids, fx.images[0]));
  CHECK(contains(result.value().image_ids, fx.images[2]));
  REQUIRE(result.value().scope_tag.has_value());
  CHECK(*result.value().scope_tag == keep);
}

TEST_CASE("resolve unquotes #\"tag with spaces\"") {
  auto fx = make_fixture("quoted", 2);
  auto tag = make_tag(fx.db, fx.project_id, "foo bar");
  REQUIRE(add_tag(fx.db, fx.images[1], tag).ok());

  auto result = resolve(fx.db, fx.project_id, "#\"foo bar\"");
  REQUIRE(result.ok());
  CHECK(result.value().image_ids.size() == 1);
  CHECK(contains(result.value().image_ids, fx.images[1]));
}

TEST_CASE("resolve matches tag names case-insensitively") {
  auto fx = make_fixture("nocase", 2);
  auto tag = make_tag(fx.db, fx.project_id, "Keeper");
  REQUIRE(add_tag(fx.db, fx.images[0], tag).ok());

  for (const auto& written : {"#Keeper", "#keeper", "#KEEPER"}) {
    auto result = resolve(fx.db, fx.project_id, written);
    REQUIRE(result.ok());
    CHECK(result.value().image_ids.size() == 1);
  }
}

// D-3：系统标签的英文别名是标识符，不是显示文案 - 不读任何界面语言状态，
// 中英两种拼法在任何语言设置下都认。
TEST_CASE("resolve accepts stable ASCII aliases for system tags") {
  auto fx = make_fixture("alias", 3);
  auto reject = ensure_reject_tag(fx.db, fx.project_id);
  auto duplicate = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[0], reject).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], duplicate).ok());

  for (const auto& written : {"#废片", "#Reject", "#reject", "#REJECT"}) {
    auto result = resolve(fx.db, fx.project_id, written);
    REQUIRE(result.ok());
    REQUIRE(result.value().scope_tag.has_value());
    CHECK(*result.value().scope_tag == reject);
    CHECK(result.value().image_ids.size() == 1);
    CHECK(contains(result.value().image_ids, fx.images[0]));
  }

  for (const auto& written : {"#重复", "#Duplicate", "#duplicate", "#DUPLICATE"}) {
    auto result = resolve(fx.db, fx.project_id, written);
    REQUIRE(result.ok());
    REQUIRE(result.value().scope_tag.has_value());
    CHECK(*result.value().scope_tag == duplicate);
  }
}

TEST_CASE("resolve rejects a scope that is neither * nor #tag") {
  auto fx = make_fixture("syntax", 1);

  for (const auto& written : {"", "精选", "**", "tag"}) {
    auto result = resolve(fx.db, fx.project_id, written);
    REQUIRE(!result.ok());
    CHECK(result.error().error == ScopeError::InvalidSyntax);
  }
}

TEST_CASE("resolve reports a missing tag distinctly from bad syntax") {
  auto fx = make_fixture("missing", 1);

  auto result = resolve(fx.db, fx.project_id, "#不存在的标签");
  REQUIRE(!result.ok());
  CHECK(result.error().error == ScopeError::TagNotFound);
  // 两个表现层的文案都要这个名字，不该为了拿它再解析一遍 scope 字符串。
  CHECK(result.error().tag_name == "不存在的标签");
}

TEST_CASE("failure carries the canonical tag name, not what the user typed") {
  auto fx = make_fixture("failname", 1);

  // 引号被剥掉
  CHECK(resolve(fx.db, fx.project_id, "#\"foo bar\"").error().tag_name == "foo bar");
  // 别名归一到存储名 - headless 的 error_msg 因此说的是库里真实的名字
  CHECK(resolve(fx.db, fx.project_id, "#Reject", SystemTagPolicy::Reject).error().tag_name ==
        kRejectTagName);
  // 语法错误时没有标签名可言
  CHECK(resolve(fx.db, fx.project_id, "乱写").error().tag_name.empty());
}

// #27 (D-2) 会用到：dedup 要拒绝系统标签范围，eval/export 仍然允许。参数
// 在本票就位，默认 Allow 保证现有调用点行为不变。
TEST_CASE("resolve can be told to refuse a system tag as the scope") {
  auto fx = make_fixture("policy", 2);
  auto reject = ensure_reject_tag(fx.db, fx.project_id);
  auto duplicate = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[0], reject).ok());
  auto keep = make_tag(fx.db, fx.project_id, "精选");
  REQUIRE(add_tag(fx.db, fx.images[1], keep).ok());
  (void)duplicate;

  SUBCASE("default policy allows it") {
    auto result = resolve(fx.db, fx.project_id, "#废片");
    CHECK(result.ok());
  }

  SUBCASE("Reject policy refuses both the stored name and the alias") {
    for (const auto& written : {"#废片", "#Reject", "#重复", "#Duplicate"}) {
      auto result = resolve(fx.db, fx.project_id, written, SystemTagPolicy::Reject);
      REQUIRE(!result.ok());
      CHECK(result.error().error == ScopeError::SystemTagNotAllowed);
    }
  }

  SUBCASE("Reject policy leaves ordinary tags and * alone") {
    CHECK(resolve(fx.db, fx.project_id, "#精选", SystemTagPolicy::Reject).ok());
    CHECK(resolve(fx.db, fx.project_id, "*", SystemTagPolicy::Reject).ok());
  }
}

TEST_CASE("exclude_by_tags drops images carrying any of the named tags") {
  auto fx = make_fixture("exclude", 4);
  auto reject = ensure_reject_tag(fx.db, fx.project_id);
  auto duplicate = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[0], reject).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], duplicate).ok());

  SUBCASE("single tag") {
    auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {kRejectTagName});
    CHECK(kept.size() == 3);
    CHECK(!contains(kept, fx.images[0]));
  }

  SUBCASE("multiple tags") {
    auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {kRejectTagName, kDuplicateTagName});
    CHECK(kept.size() == 2);
    CHECK(!contains(kept, fx.images[0]));
    CHECK(!contains(kept, fx.images[1]));
  }

  SUBCASE("empty exclude list keeps everything") {
    auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {});
    CHECK(kept.size() == 4);
  }

  SUBCASE("input order is preserved") {
    auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {kRejectTagName});
    CHECK(kept[0] == fx.images[1]);
    CHECK(kept[1] == fx.images[2]);
    CHECK(kept[2] == fx.images[3]);
  }
}

// F-26 的对称例外：范围本身就是这个标签时，用户已经显式要求处理它，不再排除。
TEST_CASE("exclude_by_tags honours the symmetric exception for the scope tag") {
  auto fx = make_fixture("symmetric", 3);
  auto reject = ensure_reject_tag(fx.db, fx.project_id);
  auto duplicate = ensure_duplicate_tag(fx.db, fx.project_id);
  REQUIRE(add_tag(fx.db, fx.images[0], reject).ok());
  REQUIRE(add_tag(fx.db, fx.images[1], duplicate).ok());

  SUBCASE("scope tag is excluded from the exclusion") {
    auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {kRejectTagName, kDuplicateTagName},
                                 reject);
    CHECK(contains(kept, fx.images[0]));   // 范围标签本身，保留
    CHECK(!contains(kept, fx.images[1]));  // 另一个排除标签，照排
  }

  SUBCASE("an unrelated scope tag does not shield anything") {
    auto keep = make_tag(fx.db, fx.project_id, "精选");
    auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {kRejectTagName}, keep);
    CHECK(!contains(kept, fx.images[0]));
  }
}

// 标签在项目里还不存在时按"不排除任何东西"处理，不是错误 - 项目还没跑过
// /dedup 时"重复"标签根本不存在。
TEST_CASE("exclude_by_tags treats a missing tag as nothing to exclude") {
  auto fx = make_fixture("notag", 2);

  auto kept = exclude_by_tags(fx.db, fx.project_id, fx.images, {kRejectTagName, kDuplicateTagName});
  CHECK(kept.size() == 2);
}
