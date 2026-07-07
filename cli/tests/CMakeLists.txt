add_executable(cli_tests
  kitty_test.cpp
  text_test.cpp
)

target_include_directories(cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest)
target_link_libraries(cli_tests PRIVATE cli_kitty cli_text)

include(CTest)
add_test(NAME cli_tests COMMAND cli_tests)
