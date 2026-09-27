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
# Atoms/ holds the 19 ATOM modules listed in Design/Atoms.md. Every module
# exposes Marshland::atom_NN_<name> through one shared CMakeLists template,
# and Atoms/CMakeLists.txt aggregates them into the marshland_atoms target.
#
# This top-level project only assembles modules; it never implements atom
# functionality (see AGENTS.md).
#
# Order matters: Atoms must come first so that marshland_atoms exists before
# Atoms/Public/AppCoordinator links against it.
# ---------------------------------------------------------------------------
add_subdirectory(Atoms)

# ---------------------------------------------------------------------------
# Shared contracts and top-level assembly layer
#
# Atoms/Public/ holds the cross-ATOM contract headers (MarshlandTypes.hpp,
# ILifecycle.hpp) and the AppCoordinator bridge layer. The bridge is the only
# place allowed to depend on both the runtime coordinator (hardware bring-up)
# and the state arbitrator (business authority).
# ---------------------------------------------------------------------------
add_subdirectory(Atoms/Public)

# ---------------------------------------------------------------------------
# Executable
#
# Entry.cpp lives in the repository root by requirement and only provides the
# process entry point. Concretely wiring RuntimeCoordinator modules, driving
# the state machine and executing the PEF flow belong to per-ATOM adapters and
# to AppCoordinator; this project assembles them, it does not implement them.
# ---------------------------------------------------------------------------
add_executable(marshland Entry.cpp)

target_compile_options(marshland PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

target_link_libraries(marshland PRIVATE marshland_public)
