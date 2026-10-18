cmake_minimum_required(VERSION 3.16)

project(Marshland
    VERSION 0.1.0
    DESCRIPTION "RoboMaster 2027 Dart System Gimbal Control Program"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Target platform guard
#
# AGENTS.md: the runtime target is an Ubuntu ARM host, and building with an
# x86 toolchain is forbidden. Only these two paths are permitted:
#   1. cross-compile inside WSL with an aarch64 toolchain file, or
#   2. build directly on the target Ubuntu ARM host.
# A native x86_64 configure is therefore rejected here.
# ---------------------------------------------------------------------------
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|armv8l|armv7l|armv7|arm)")
    message(FATAL_ERROR
        "Unsupported build architecture: CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'.\n"
        "This program targets Ubuntu ARM only; x86 toolchains are forbidden by AGENTS.md.\n"
        "Build inside WSL with an aarch64 cross toolchain, or build on the target ARM host.")
endif()

# ---------------------------------------------------------------------------
# C++ language settings
#
# C++23 is the project baseline: PRD 5.2, Interface 1, and Config 1 all specify
# C++23 as the development standard, so this must not lag behind the design
# documents. CMAKE_CXX_EXTENSIONS is OFF, so this requests strict -std=c++23
# (not gnu++23).
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Explicit C++23 toolchain guard
#
# CMAKE_CXX_STANDARD_REQUIRED only makes the *request* mandatory; when the
# compiler is too old CMake would otherwise decay the standard (23 -> 20 -> 17
# -> 14 -> 11) and the build would still succeed, silently compiling against a
# standard that contradicts the design baseline.
#
# Two independent checks are performed:
#   1. CMake's own feature table must know cxx_std_23 for this compiler.
#   2. A real probe compilation must confirm the standard is actually in
#      effect, by asserting the value of __cplusplus.
#
# Deliberately NOT used as the probe: the C++23 "deducing this" explicit
# object parameter. It is not a valid baseline test -- GCC 13.3 is a fully
# usable C++23 toolchain (and this project builds clean under it with
# -std=c++23) yet has not implemented that particular feature, which landed in
# GCC 14. Gating on it would produce false rejections. The probe below is
# limited to constructs that all C++23 toolchains accept.
#
# The temporary standard override does not leak: the normal values are
# snapshotted and restored afterwards.
# ---------------------------------------------------------------------------
include(CheckCXXSourceCompiles)

if(NOT "cxx_std_23" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    message(FATAL_ERROR
        "C++23 is the mandatory project baseline (PRD 5.2, Interface 1, Config 1), "
        "but the active compiler is not known to support it.\n"
        "  CMAKE_CXX_COMPILER = ${CMAKE_CXX_COMPILER}\n"
        "  compiler id/version = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
        "  compiler target    = '${CMAKE_CXX_COMPILER_TARGET}'\n"
        "Install GCC 13+ / Clang 17+, or point CMAKE_TOOLCHAIN_FILE at a WSL aarch64 "
        "toolchain that provides one.")
endif()

set(_marshland_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
set(_marshland_saved_cxx_standard_required "${CMAKE_CXX_STANDARD_REQUIRED}")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

check_cxx_source_compiles("
// C++23 introduces 'if consteval'; every C++23 toolchain accepts it.
constexpr int marshland_cxx23_probe()
{
    if consteval { return 23; }
    return 0;
}

// Verify the requested standard is genuinely in effect and was not silently
// decayed. GCC 13 in -std=c++23 mode reports 202100L. The threshold is set to
// accept 202100L and any later value; it intentionally does not use equality
// so that a toolchain publishing a later date is not rejected.
static_assert(__cplusplus >= 202100L, \"C++23 is not in effect\");

int main()
{
    return marshland_cxx23_probe() == 23 ? 0 : 1;
}
" CXX23_COMPILER_WORKS)

set(CMAKE_CXX_STANDARD "${_marshland_saved_cxx_standard}")
set(CMAKE_CXX_STANDARD_REQUIRED "${_marshland_saved_cxx_standard_required}")

if(NOT CXX23_COMPILER_WORKS)
    message(FATAL_ERROR
        "C++23 did not take effect in the probe build, so the requested standard was "
        "silently decayed to an earlier one.\n"
        "  CMAKE_CXX_COMPILER = ${CMAKE_CXX_COMPILER}\n"
        "  compiler id/version = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n"
        "  compiler target    = '${CMAKE_CXX_COMPILER_TARGET}'\n"
        "Install GCC 13+ / Clang 17+, or point CMAKE_TOOLCHAIN_FILE at a WSL aarch64 "
        "toolchain that provides one.")
endif()

message(STATUS
    "C++23 support confirmed: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# Default to Release for single-config generators (never override an explicit choice).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Module assembly
#
# Atoms/CMakeLists.txt lists the retained modules and aggregates their targets.
#
# This top-level project only assembles modules; it never implements atom
# functionality (see AGENTS.md).
# ---------------------------------------------------------------------------
add_subdirectory(ThirdParty)

# ---------------------------------------------------------------------------
# Config schema compile-time embedding
#
# Rule/ConfigSchema.schema.json is the authoritative contract for the runtime
# configuration file, but Rule/ is not deployed to the ARM host. Embed the
# schema text into the binary as marshland::atom15::kConfigSchemaJson so
# ATOM-15 can compile it into a schema validator at first load.
# Consumed by Atoms/ATOM-15_Config_Persistence via Marshland::ConfigSchemaEmbed.
# dart_control.json itself is runtime data (hot-updated and persisted on disk)
# and is deliberately NOT embedded.
# ---------------------------------------------------------------------------
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Rule/ConfigSchema.schema.json")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Rule/ConfigSchema.schema.json"
     MARSHLAND_CONFIG_SCHEMA_JSON)
set(MARSHLAND_SCHEMA_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${MARSHLAND_SCHEMA_GEN_DIR}/config_schema_embed.hpp"
"// Generated by CMake from Rule/ConfigSchema.schema.json — do not edit.
#pragma once
namespace marshland::atom15 {
inline constexpr char kConfigSchemaJson[] = R\"MARSHLAND_SCHEMA(${MARSHLAND_CONFIG_SCHEMA_JSON})MARSHLAND_SCHEMA\";
}
")
add_library(marshland_config_schema_embed INTERFACE)
add_library(Marshland::ConfigSchemaEmbed ALIAS marshland_config_schema_embed)
target_include_directories(marshland_config_schema_embed SYSTEM INTERFACE
    "${MARSHLAND_SCHEMA_GEN_DIR}")

# Public contracts must exist before ATOM targets are configured.
add_subdirectory(Atoms/Public)
add_subdirectory(Atoms)

# ---------------------------------------------------------------------------
# Executable
#
# Entry.cpp owns the process entry point and assembles the retained modules.
# ---------------------------------------------------------------------------
add_executable(marshland Entry.cpp)

target_compile_options(marshland PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

target_link_libraries(marshland PRIVATE
    Marshland::Atoms
    Marshland::PublicHeaders)

find_package(Threads REQUIRED)
target_link_libraries(marshland PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# 部署链接方式：全静态
#
# 目标机（Ubuntu ARM）的运行库比 WSL 交叉工具链旧：本机工具链是 Ubuntu 24.04
# （glibc 2.39 + GCC 13 的 libstdc++）。在 -std=c++23 下，glibc >= 2.38 的头文件
# 会把 strto*/sscanf 重定向到 ISO C23 变体（__isoc23_*），再加上 GCC 13 的
# libstdc++ 符号，二进制会要求 GLIBC_2.38 / GLIBCXX_3.4.32，在目标机上直接报
#   "./marshland: /lib/aarch64-linux-gnu/libc.so.6: version `GLIBC_2.38' not found"
# 这不是某一处代码的问题：Entry.cpp、ATOM-15/16 与 ThirdParty 的多个翻译单元
# 都会引入该符号。
#
# 全静态链接后二进制没有动态段，不再受目标机 glibc/libstdc++ 版本影响。
# 注意：静态链接下 getaddrinfo 需要目标机提供匹配的 NSS 库；本程序只把
# cpp-httplib 当服务器用（绑定固定地址），不经过主机名解析路径。
# ---------------------------------------------------------------------------
target_link_options(marshland PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-static>)

option(MARSHLAND_BUILD_TESTS "Expose current tests, excluded from the default build" OFF)
if(MARSHLAND_BUILD_TESTS)
    add_executable(marshland_state_machine_tests EXCLUDE_FROM_ALL Tests/StateMachineTests.cpp)
    add_executable(marshland_state_entry_tests EXCLUDE_FROM_ALL Tests/StateMachineEntryTests.cpp)
    foreach(test_target IN ITEMS marshland_state_machine_tests marshland_state_entry_tests)
        target_include_directories(${test_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(${test_target} PRIVATE
            Marshland::PublicHeaders
            Marshland::atom_01_runtime
            Marshland::atom_02_state_arbitration
            Threads::Threads)
        target_compile_options(${test_target} PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
    endforeach()
endif()
