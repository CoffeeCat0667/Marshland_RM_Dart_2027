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
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release for single-config generators (never override an explicit choice).
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Executable
#
# Entry.cpp lives in the repository root by requirement and only provides the
# process entry point. Startup coordination (config loading, module
# initialisation, STANDBY/INIT_FAILED) is defined by Design/Atomic/ATOM-01.md
# and must not be implemented by this project.
# ---------------------------------------------------------------------------
add_executable(marshland Entry.cpp)

target_compile_options(marshland PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

# ---------------------------------------------------------------------------
# Module assembly
#
# Atoms/ holds the 19 ATOM modules listed in Design/Atoms.md. Every module
# exposes Marshland::atom_NN_<name> through one shared CMakeLists template,
# and Atoms/CMakeLists.txt aggregates them into the marshland_atoms target.
#
# This top-level project only assembles modules; it never implements atom
# functionality (see AGENTS.md).
# ---------------------------------------------------------------------------
add_subdirectory(Atoms)

target_link_libraries(marshland PRIVATE marshland_atoms)
