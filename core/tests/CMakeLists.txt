add_executable(core_tests
  scaffolding_test.cpp
  result_test.cpp
  db_test.cpp
  project_test.cpp
  tagging_test.cpp
  browse_test.cpp
  prefetch_test.cpp
  export_test.cpp
  decode_test.cpp
  recipe_test.cpp
  color_test.cpp
  ai_test.cpp
  evaluation_test.cpp
  evaluation_worker_test.cpp
)

target_include_directories(core_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest)
target_link_libraries(core_tests PRIVATE core)

include(CTest)
add_test(NAME core_tests COMMAND core_tests)
