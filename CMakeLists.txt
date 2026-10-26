# ── VFS ───────────────────────────────────────────────────────────────────────
# C++23: Skyrim's Data folder as the game sees it — loose files over BSA archives, assembled from a game Data folder,
# a Mod Organizer 2 instance + profile, or plain folders.
#
# Consumed via FetchContent; consumers link `vfs::vfs` and `#include <vfs/Vfs.h>`. Layout:
#   API/vfs/  — public headers
#   src/      — implementations
#   tests/    — CTest suite
#
# Needs lz4 (LZ4 frames inside SSE archives): find_package(lz4 CONFIG) from vcpkg, or an `lz4::lz4` target the
# consumer already defines. The MSVC runtime is NOT set here: it inherits the consumer's (SKSE plugins and Qt apps).

cmake_minimum_required(VERSION 3.28)

project(VFS
    VERSION 0.1.0
    DESCRIPTION "Skyrim's Data folder as the game sees it: loose files over BSAs, MO2-aware"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

option(VFS_BUILD_TESTS "Build VFS tests" ${PROJECT_IS_TOP_LEVEL})

if(NOT TARGET lz4::lz4)
    find_package(lz4 CONFIG REQUIRED)
endif()

file(GLOB_RECURSE VFS_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE VFS_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/API/vfs/*.h")

add_library(vfs STATIC ${VFS_SOURCES} ${VFS_HEADERS})
add_library(vfs::vfs ALIAS vfs)
target_include_directories(vfs PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/API>)
target_compile_features(vfs PUBLIC cxx_std_23)
target_link_libraries(vfs PRIVATE lz4::lz4)
if(MSVC)
    target_compile_options(vfs PRIVATE /W4 /wd4100 /utf-8)
endif()
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${VFS_SOURCES} ${VFS_HEADERS})

if(VFS_BUILD_TESTS)
    enable_testing()
    add_executable(vfs-test tests/vfs_test.cpp)
    target_link_libraries(vfs-test PRIVATE vfs::vfs)
    add_test(NAME vfs-test COMMAND vfs-test)
endif()
