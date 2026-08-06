add_executable(cli_tests
  debug_log_test.cpp
  i18n_test.cpp
  kitty_test.cpp
  signal_restore_test.cpp
  tag_menu_test.cpp
  text_test.cpp
)

target_include_directories(cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest)
# cli_menu 进来是为了 tag_menu_test 能直接测 tags_for_menu(T-24 的截断/上限判
# 定)。这一层依赖 cli_ui 的终端 io 原语,但被测函数只查库不碰终端,链进来
# 不会让测试需要一个真 tty。
target_link_libraries(cli_tests PRIVATE cli_kitty cli_menu cli_text cli_i18n)

include(CTest)
add_test(NAME cli_tests COMMAND cli_tests)
