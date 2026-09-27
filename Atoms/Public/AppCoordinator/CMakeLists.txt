# ============================================================================
# AppCoordinator — 顶层装配与桥接层
#
# 统一模块契约的例外说明：
#   本目标与 Atoms/ATOM-01 ~ ATOM-19 不同，它**允许**依赖其他 Atom 目标：
#   它的职责就是在底层（RuntimeCoordinator）与上层（StateArbitrator）之间做桥接。
#   依赖方向为 AppCoordinator -> Atom -> Public 契约，无环。
#
#   1. 由上层工程通过 add_subdirectory() 组装；本文件不定义 project()。
#   2. 不产生可执行文件，不定义 main()。
#   3. 不设置编译器、目标平台与 C++ 标准，全部由上层工程决定。
# ============================================================================
set(APP_COORDINATOR_TARGET marshland_app)
set(APP_COORDINATOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(TARGET ${APP_COORDINATOR_TARGET})
    return()
endif()

file(GLOB_RECURSE APP_COORDINATOR_SOURCES CONFIGURE_DEPENDS
    "${APP_COORDINATOR_DIR}/Src/*.c" "${APP_COORDINATOR_DIR}/Src/*.cc"
    "${APP_COORDINATOR_DIR}/Src/*.cpp" "${APP_COORDINATOR_DIR}/Src/*.cxx")
file(GLOB_RECURSE APP_COORDINATOR_HEADERS CONFIGURE_DEPENDS
    "${APP_COORDINATOR_DIR}/Inc/*.h" "${APP_COORDINATOR_DIR}/Inc/*.hh"
    "${APP_COORDINATOR_DIR}/Inc/*.hpp")

if(NOT APP_COORDINATOR_SOURCES)
    message(FATAL_ERROR
        "AppCoordinator is a layer with implementation; ${APP_COORDINATOR_DIR}/Src contains no sources.")
endif()

add_library(${APP_COORDINATOR_TARGET} STATIC
    ${APP_COORDINATOR_SOURCES} ${APP_COORDINATOR_HEADERS})
add_library(Marshland::${APP_COORDINATOR_TARGET} ALIAS ${APP_COORDINATOR_TARGET})

target_include_directories(${APP_COORDINATOR_TARGET} PUBLIC
    "$<BUILD_INTERFACE:${APP_COORDINATOR_DIR}/Inc>")

# 共享契约（MarshlandTypes.hpp / ILifecycle.hpp）
target_link_libraries(${APP_COORDINATOR_TARGET} PUBLIC
    Marshland::PublicHeaders)

# 底层与上层：仅本层允许链接 Atom 目标。
#
# 同时显式加入两个 Atom 的公开头目录：marshland_app 是唯一允许直接包含
# RuntimeCoordinator.hpp / StateArbitration.hpp 的目标，而这两个头不在
# PublicHeaders 的包含路径内（Atom 模板只把 ../Public 传给各自的使用者）。
target_link_libraries(${APP_COORDINATOR_TARGET} PRIVATE
    Marshland::atom_01_runtime
    Marshland::atom_02_state_arbitration)

target_include_directories(${APP_COORDINATOR_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ATOM-01_Runtime/Inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../ATOM-02_State_Arbitration/Inc")
