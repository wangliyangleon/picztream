add_executable(cli_tests
  debug_log_test.cpp
  i18n_test.cpp
  kitty_test.cpp
  recipe_menu_test.cpp
  signal_restore_test.cpp
  tag_menu_test.cpp
  text_test.cpp
  ui_test.cpp
)

target_include_directories(cli_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/doctest)
# T-15 票 E 的用词守卫要扫 i18n.cpp 的字符串字面量本身,而不是枚举几个函数
# 挨个调用 - 枚举拦不住"下一张票新写的那条文案"，而那正是这条守卫存在的
# 理由。所有 cli 显示文案的字面量都只在这一个文件里(已核实 cli/ 下其余
# 中文字面量全是注释)，所以扫它一个就够。
target_compile_definitions(cli_tests PRIVATE
  PZT_I18N_SOURCE="${CMAKE_SOURCE_DIR}/cli/i18n/i18n.cpp")
# cli_menu 进来是为了 tag_menu_test 能直接测 tags_for_menu(T-24 的截断/上限判
# 定)。这一层依赖 cli_ui 的终端 io 原语,但被测函数只查库不碰终端,链进来
# 不会让测试需要一个真 tty。
target_link_libraries(cli_tests PRIVATE cli_kitty cli_menu cli_text cli_i18n)

include(CTest)
add_test(NAME cli_tests COMMAND cli_tests)
