find_package(SQLite3 REQUIRED)

find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

# M2: core/raw 对 LibRaw 的封装。Homebrew 的 libraw.pc 只声明了 lcms2 依赖，
# 没有把它内部用的 OpenMP(libomp,keg-only,不在默认头文件搜索路径)的 include
# 路径带出来，见 docs/M2_Eng_Design.md"技术选型"一节——spikes/libraw_probe
# 已经踩过这个坑，这里手动补上，不能假设 pkg-config 的输出已经完整。
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibRaw REQUIRED IMPORTED_TARGET libraw)
find_path(LIBOMP_INCLUDE_DIR omp.h PATHS /opt/homebrew/opt/libomp/include REQUIRED)
find_library(LIBOMP_LIBRARY omp PATHS /opt/homebrew/opt/libomp/lib REQUIRED)
target_include_directories(PkgConfig::LibRaw INTERFACE ${LIBOMP_INCLUDE_DIR})
target_link_libraries(PkgConfig::LibRaw INTERFACE ${LIBOMP_LIBRARY})

# M3: core/ai 调用 Claude/Gemini 的云端 API。libcurl 用的是 macOS 系统自带
# 的那份（Homebrew 只是给它包了一层合成的 pkg-config 文件，不是一个真的
# keg），实测直接可用，不需要像上面 libomp 那样手动补 include/lib 路径。
# nlohmann_json 走 Homebrew 的 CMake config package（`brew install
# nlohmann-json`）。见 docs/M3_Eng_Design.md"技术选型"一节。
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
find_package(nlohmann_json REQUIRED)

add_library(core STATIC
  api.cpp
  db/schema.cpp
  db/database.cpp
  project/project.cpp
  tagging/tagging.cpp
  browse/browse.cpp
  browse/prefetch.cpp
  export/export.cpp
  decode/decode.cpp
  media/media.cpp
  recipe/recipe.cpp
  color/color.cpp
  raw/raw.cpp
  ai/ai.cpp
  ai/evaluation.cpp
  ai/evaluation_worker.cpp
  ai/style.cpp
  ai/compare.cpp
  dedup/dedup.cpp
  curate/curate.cpp
  tournament/tournament.cpp
  settings/settings.cpp
)

target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(core PUBLIC SQLite3::SQLite3
  ${COREGRAPHICS_FRAMEWORK} ${IMAGEIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK}
  PkgConfig::LibRaw PkgConfig::CURL nlohmann_json::nlohmann_json)

add_subdirectory(tests)
