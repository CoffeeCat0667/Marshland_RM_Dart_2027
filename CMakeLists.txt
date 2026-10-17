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
# Shared contract headers
#
# Must be introduced BEFORE Atoms/: the 19 atom modules expose a shared
# CMakeLists template that links Marshland::PublicHeaders behind
# `if(TARGET Marshland::PublicHeaders)`. That condition can only be true if the
# contract target already exists when each atom directory is processed —
# otherwise the dependency never enters the CMake dependency graph and each
# module silently relies on a hard-coded `../Public` include path instead.
#
# Atoms/Public/Headers/ contains ONLY the header-only interface target; the
# top-level assembly layer (AppCoordinator) stays in Atoms/Public/ and is added
# after Atoms because it links against marshland_atoms.
# ---------------------------------------------------------------------------
add_subdirectory(Atoms/Public/Headers)

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

add_subdirectory(Atoms)

# ---------------------------------------------------------------------------
# Top-level assembly and aggregation
#
# Atoms/Public/ holds the AppCoordinator bridge (the only place allowed to
# depend on both the runtime coordinator and the state arbitrator) plus the
# marshland_public aggregate target. It must come after Atoms/ so that
# marshland_atoms already exists.
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

find_package(Threads REQUIRED)
target_link_libraries(marshland_app PUBLIC Threads::Threads)
target_compile_options(marshland_app PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

option(MARSHLAND_BUILD_TESTS "Build ARM gateway and public-contract tests" OFF)
if(MARSHLAND_BUILD_TESTS)
    enable_testing()

    # This target intentionally receives no ATOM or AppCoordinator include paths.
    add_library(marshland_public_contracts_check OBJECT Tests/PublicContractsCompile.cpp)
    target_link_libraries(marshland_public_contracts_check PRIVATE Marshland::PublicHeaders)
    target_compile_options(marshland_public_contracts_check PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

    add_executable(marshland_gateway_tests Tests/AppCoordinatorTests.cpp)
    target_include_directories(marshland_gateway_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(marshland_gateway_tests PRIVATE marshland_public)
    target_compile_options(marshland_gateway_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

    # ATOM-03 SBUS tests
    add_executable(marshland_atom03_tests Tests/Atom03SbusTests.cpp)
    target_include_directories(marshland_atom03_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(marshland_atom03_tests PRIVATE
        Marshland::atom_03_sbus
        Marshland::PublicHeaders)
    target_compile_options(marshland_atom03_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

    # ATOM-15 Config tests
    add_executable(marshland_atom15_tests Tests/Atom15ConfigTests.cpp)
    target_include_directories(marshland_atom15_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(marshland_atom15_tests PRIVATE
        Marshland::atom_15_config_persistence
        Marshland::PublicHeaders)
    target_compile_options(marshland_atom15_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

    # ATOM-04 CAN tests —— 已注销（2026-10-16）。
    # 模块按用户裁决收缩为「一个阻塞接收线程 + 一个发送 API」，
    # CanCore / CanRuntimeConfig / LifecycleAdapter / ICanTransport 公开面已删除；
    # Tests/Atom04CanTests.cpp 仍 include atom04_can/can_core.hpp 与 can_config.hpp，
    # 无法编译，故不再注册该目标与 can.* 用例。
    # 依赖面：无（Entry.cpp 与 Atoms 其余模块都不消费本模块）。
    # 残留：该测试源文件本身尚未清理，是否裁剪为仅覆盖保留的 motor_frame_codec
    #       用例待用户裁决。详见 Atoms/ATOM-04_CAN_Motor_Control/PLAN.md §5。

    # Integration tests (Entry + all ATOMs)
    add_executable(marshland_integration_tests Tests/IntegrationTests.cpp)
    target_include_directories(marshland_integration_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(marshland_integration_tests PRIVATE marshland_public)
    target_compile_options(marshland_integration_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)

    set(MARSHLAND_CTEST_CASES
        public.types
        runtime.registration
        runtime.failure_exception
        state.policy_tables
        state.inputs_events
        state.intent_fire
        state.auto_flow
        app.coordinator_ports
        app.read_lease_concurrency
        process.exit_signals
        public.enum_defaults
        public.monotonic_clock
        runtime.context_and_order
        runtime.optional_failure
        runtime.fallback_names
        runtime.unknown_exception
        runtime.empty_and_destructor
        state.fire_matrix
        state.intent_safety_matrix
        state.stop_clear_matrix
        state.ordinary_intents
        state.mode_matrix
        state.auto_edge_matrix
        state.all_strings
        state.reset_and_update
        state.event_buffer
        state.four_shot_run
        state.auto_rejections
        state.manual_fire_modes
        app.startup_rejections
        app.startup_failures
        app.startup_interrupt
        app.gate_modes
        app.all_ports
        app.stop_flag_atomic_commit
        app.read_view_move
        app.clock_and_events)

    # ATOM-03 SBUS test cases
    set(MARSHLAND_ATOM03_CASES
        sbus.config_defaults
        sbus.snapshot_structure
        sbus.store_read_write
        sbus.store_concurrency
        sbus.uart_configure
        sbus.uart_read_data
        sbus.uart_read_timeout
        sbus.uart_cancel
        sbus.serial_parameters
        sbus.estop_state_enum
        sbus.frame_validity_enum
        sbus.control_source_mapping)

    # ATOM-15 Config test cases
    set(MARSHLAND_ATOM15_CASES
        config.store_default_state
        config.store_load_valid
        config.store_load_invalid_json
        config.store_load_missing_file
        config.store_get_string
        config.store_get_int
        config.store_get_int_wrong_type
        config.store_get_string_wrong_type
        config.store_missing_key
        config.store_motor_array
        config.store_nested_keys
        config.store_reload
        config.store_interface
        config.store_thread_safety
        config.store_schema_version)

    # ATOM-04 CAN test cases —— 已随 marshland_atom04_tests 一并移除（见上）。

    # Integration test cases
    set(MARSHLAND_INTEGRATION_CASES
        integration.full_startup
        integration.config_load_failure
        integration.atom15_before_atom03
        integration.motor_config_6_motors
        integration.can_zero_torque_frames
        integration.runtime_context_transfer
        integration.shutdown_cleanup)

    if(CMAKE_CROSSCOMPILING OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|arm)")
        find_program(MARSHLAND_QEMU_AARCH64 NAMES qemu-aarch64 qemu-aarch64-static)
        if(MARSHLAND_QEMU_AARCH64)
            set(MARSHLAND_ARM_SYSROOT "/usr/aarch64-linux-gnu" CACHE PATH "ARM emulator library root")

            foreach(case_name IN LISTS MARSHLAND_CTEST_CASES)
                add_test(NAME "marshland.${case_name}"
                    COMMAND "${MARSHLAND_QEMU_AARCH64}" -L "${MARSHLAND_ARM_SYSROOT}"
                            $<TARGET_FILE:marshland_gateway_tests> "${case_name}")
            endforeach()

            foreach(case_name IN LISTS MARSHLAND_ATOM03_CASES)
                add_test(NAME "marshland.${case_name}"
                    COMMAND "${MARSHLAND_QEMU_AARCH64}" -L "${MARSHLAND_ARM_SYSROOT}"
                            $<TARGET_FILE:marshland_atom03_tests> "${case_name}")
            endforeach()

            foreach(case_name IN LISTS MARSHLAND_ATOM15_CASES)
                add_test(NAME "marshland.${case_name}"
                    COMMAND "${MARSHLAND_QEMU_AARCH64}" -L "${MARSHLAND_ARM_SYSROOT}"
                            $<TARGET_FILE:marshland_atom15_tests> "${case_name}")
            endforeach()

        foreach(case_name IN LISTS MARSHLAND_INTEGRATION_CASES)
                add_test(NAME "marshland.${case_name}"
                    COMMAND "${MARSHLAND_QEMU_AARCH64}" -L "${MARSHLAND_ARM_SYSROOT}"
                            $<TARGET_FILE:marshland_integration_tests> "${case_name}")
            endforeach()
        else()
            message(STATUS "ARM tests will be built, but execution needs an ARM host or qemu-aarch64; no CTest cases registered.")
        endif()
    else()
        foreach(case_name IN LISTS MARSHLAND_CTEST_CASES)
            add_test(NAME "marshland.${case_name}" COMMAND marshland_gateway_tests "${case_name}")
        endforeach()

        foreach(case_name IN LISTS MARSHLAND_ATOM03_CASES)
            add_test(NAME "marshland.${case_name}" COMMAND marshland_atom03_tests "${case_name}")
        endforeach()

        foreach(case_name IN LISTS MARSHLAND_ATOM15_CASES)
            add_test(NAME "marshland.${case_name}" COMMAND marshland_atom15_tests "${case_name}")
        endforeach()

        foreach(case_name IN LISTS MARSHLAND_INTEGRATION_CASES)
            add_test(NAME "marshland.${case_name}" COMMAND marshland_integration_tests "${case_name}")
        endforeach()
    endif()

    # Set timeout for all tests
    foreach(case_name IN LISTS MARSHLAND_CTEST_CASES MARSHLAND_ATOM03_CASES MARSHLAND_ATOM15_CASES MARSHLAND_INTEGRATION_CASES)
        if(TEST "marshland.${case_name}")
            set_tests_properties("marshland.${case_name}" PROPERTIES TIMEOUT 30)
        endif()
    endforeach()
endif()
