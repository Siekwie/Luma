cmake_minimum_required(VERSION 3.20)
project(luma VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Lua.cmake)

set(LUMA_CORE_SOURCES
    src/graph/build_graph.cpp
    src/graph/cache_key.cpp
    src/filesystem/path_util.cpp
    src/filesystem/glob.cpp
    src/filesystem/hash.cpp
    src/filesystem/sha256.cpp
    src/config/graph_builder.cpp
    src/config/model.cpp
    src/lua/config_loader.cpp
    src/toolchain/toolchain.cpp
    src/cache/cache_store.cpp
    src/executor/process.cpp
    src/executor/executor.cpp
    src/cli/session.cpp
)

add_library(luma_core ${LUMA_CORE_SOURCES})
target_include_directories(luma_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(luma_core PUBLIC lua54)

add_executable(luma
    src/cli/main.cpp
    src/cli/commands.cpp
)
target_link_libraries(luma PRIVATE luma_core)

if(MSVC)
    target_compile_options(luma_core PRIVATE /W4 /utf-8)
    target_compile_options(luma PRIVATE /W4 /utf-8)
else()
    target_compile_options(luma_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(luma PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_subdirectory(tests)
