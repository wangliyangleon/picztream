add_library(cli_text STATIC
  text/text.cpp
)
target_include_directories(cli_text PUBLIC ${CMAKE_SOURCE_DIR})
# cli_text 是纯文本工具,不依赖 core,也不依赖任何终端渲染。

add_library(cli_ui STATIC
  ui/ui.cpp
)
target_include_directories(cli_ui PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(cli_ui PUBLIC cli_text)
# cli_ui 是终端 io 原语,依赖 cli_text,但不依赖 core。

add_library(cli_kitty STATIC
  kitty/kitty.cpp
)
target_include_directories(cli_kitty PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(cli_kitty PUBLIC core)

add_library(cli_i18n STATIC
  i18n/i18n.cpp
)
target_include_directories(cli_i18n PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(cli_i18n PUBLIC core cli_text)

add_library(cli_menu STATIC
  menu/tag_menu.cpp
  menu/filter_menu.cpp
  menu/recipe_menu.cpp
)
target_include_directories(cli_menu PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(cli_menu PUBLIC core cli_ui cli_text cli_i18n)
# cli_menu 是 space/g/r 交互菜单,依赖 core + cli_ui + cli_text + cli_i18n。

add_executable(pzt
  main.cpp
  commands/commands.cpp
  commands/browse.cpp
)

target_link_libraries(pzt PRIVATE core cli_kitty cli_menu cli_ui cli_text cli_i18n)

add_subdirectory(tests)

