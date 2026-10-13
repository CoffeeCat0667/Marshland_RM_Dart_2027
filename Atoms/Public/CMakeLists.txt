# ============================================================================
# Atoms/Public — 跨 ATOM 共享契约的**目标定义入口**
#
# 本文件不再定义 marshland_public_headers：
#   PublicHeaders 已移入 Atoms/Public/Headers/CMakeLists.txt，由顶层 CMake 在
#   `add_subdirectory(Atoms)` **之前**引入，使 19 个 ATOM 模块 CMakeLists 中的
#       if(TARGET Marshland::PublicHeaders) ... endif()
#   真正成立、依赖关系进入 CMake 依赖图。
#
# 本文件在 `add_subdirectory(Atoms)` **之后**由顶层引入，负责装配层与聚合目标：
#   1. AppCoordinator（桥接层，需要 marshland_atoms 已存在）
#   2. marshland_public（对外聚合：PublicHeaders + ATOM 集合 + 装配层）
#
# 顺序不可调换：AppCoordinator 通过 $<TARGET_NAME:...> 语义依赖 marshland_atoms。
# ============================================================================

# ---------------------------------------------------------------------------
# 顶层装配与桥接层（AppCoordinator）
#
# 位置在 Public/ 之下只是为了就近使用共享类型；它的依赖方向是**自上而下**的，
# 持有 RuntimeCoordinator 与 StateArbitrator，因此**不属于**公共契约。
# 其余 18 个 ATOM 禁止包含 AppCoordinator.hpp。
# ---------------------------------------------------------------------------
add_subdirectory(AppCoordinator)

add_library(marshland_public INTERFACE)
add_library(Marshland::Public ALIAS marshland_public)

target_link_libraries(marshland_public INTERFACE
    Marshland::PublicHeaders
    marshland_atoms
    marshland_app)
