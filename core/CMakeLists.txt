find_package(SQLite3 REQUIRED)

find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(IMAGEIO_FRAMEWORK ImageIO REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

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
  recipe/recipe.cpp
  color/color.cpp
)

target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(core PUBLIC SQLite3::SQLite3
  ${COREGRAPHICS_FRAMEWORK} ${IMAGEIO_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})

add_subdirectory(tests)
