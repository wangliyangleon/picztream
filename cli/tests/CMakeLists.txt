add_executable(cli_tests
  i18n_test.cpp
  kitty_test.cpp
  text_test.cpp
)

target_include_directories(cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest)
target_link_libraries(cli_tests PRIVATE cli_kitty cli_text cli_i18n)

include(CTest)
add_test(NAME cli_tests COMMAND cli_tests)
