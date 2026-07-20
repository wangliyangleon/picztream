cmake_minimum_required(VERSION 3.25)
project(picztream VERSION 2026.7.20 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

# Apple's libc++ gates std::jthread/std::stop_token behind this flag (not yet
# ABI-stable upstream). AGENTS.md mandates std::jthread for all concurrency,
# so this is required project-wide, not just for the scaffolding smoke test.
add_compile_options(-fexperimental-library)
add_link_options(-fexperimental-library)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

add_subdirectory(core)
add_subdirectory(cli)

# Symlink compile_commands.json to the repo root so clangd (LazyVim) picks it
# up without extra configuration.
if(CMAKE_EXPORT_COMPILE_COMMANDS)
  file(CREATE_LINK
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${CMAKE_SOURCE_DIR}/compile_commands.json"
    SYMBOLIC)
endif()
