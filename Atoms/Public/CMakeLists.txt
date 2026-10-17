# ============================================================================
# Atoms/Public — 跨 ATOM 共享契约头（仅纯类型，无任何 Atom 依赖）
#
# 本目标必须保持为叶子节点：只含类型与 inline 工具，不链接任何 ATOM，
# 否则会与 AppCoordinator 形成依赖环。
#   允许：MarshlandTypes.hpp、ILifecycle.hpp、StateArbitrationTypes.hpp、
#         IMonotonicClock.hpp、AppInterfaces.hpp、IConfigView.hpp、
#         Sbus.hpp、Auto.hpp、Web.hpp、CanTypes.hpp、CanInterfaces.hpp、
#         Motor.hpp、
#         ForceTypes.hpp、ForceInterfaces.hpp、Log.hpp、
#         StateMachineTypes.hpp、IStateMachine.hpp
#   （ATOM-18 的 DiagnosticsTypes/ILogSink/IFaultRegistry/IDiagnosticsSnapshot
#     已于 2026-10-16 按用户裁决删除；同日起该模块对外接口收敛为
#     Log.hpp 中的 Init / Log 两个函数）
#   禁止：任何 AppCoordinator 相关源码（见 Atoms/Public/AppCoordinator/）
#
# 为什么有独立的 CMakeLists.txt（而不是留在顶层里）：
#   19 个 ATOM 模块的 CMakeLists 模板都形如
#       if(TARGET Marshland::PublicHeaders)
#           target_link_libraries(<模块> <scope> Marshland::PublicHeaders)
#       endif()
#   而顶层 CMake 必须按 "头契约 → ATOM → 装配层" 的顺序引入子目录
#   （AppCoordinator 需要 marshland_atoms 已存在）。若 PublicHeaders 与
#   AppCoordinator 在同一个 CMakeLists 中，二者无法拆分，上述条件判断在
#   ATOM 处理时恒为假，导致：
#     1. 依赖关系不进入 CMake 依赖图（仅靠各模块裸加 `../Public` 包含路径兜住）；
#     2. 任何只依赖 PublicHeaders 而不自带包含路径的模块会直接编译失败。
#   独立的 CMakeLists.txt 后，顶层可先引入它，使 19 个模块的链接声明真正生效。
# ============================================================================
add_library(marshland_public_headers INTERFACE)
add_library(Marshland::PublicHeaders ALIAS marshland_public_headers)

target_include_directories(marshland_public_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

target_sources(marshland_public_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/MarshlandTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ILifecycle.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StateArbitrationTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IMonotonicClock.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AppInterfaces.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IConfigView.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sbus.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Auto.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Web.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CanTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CanInterfaces.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Motor.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ForceTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ForceInterfaces.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Log.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StateMachineTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/IStateMachine.hpp")
