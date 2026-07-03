add_library(cli_kitty STATIC
  kitty/kitty.cpp
)
target_include_directories(cli_kitty PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(cli_kitty PUBLIC core)

add_executable(pzt
  main.cpp
)

target_link_libraries(pzt PRIVATE core cli_kitty)

add_subdirectory(tests)
