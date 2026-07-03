find_package(SQLite3 REQUIRED)

add_library(core STATIC
  api.cpp
  db/schema.cpp
  db/database.cpp
  project/project.cpp
  tagging/tagging.cpp
  browse/browse.cpp
  export/export.cpp
)

target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(core PUBLIC SQLite3::SQLite3)

add_subdirectory(tests)
