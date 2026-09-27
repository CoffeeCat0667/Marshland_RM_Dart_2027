# ============================================================================
# Atoms/Public — 跨 ATOM 共享契约（仅纯类型，无任何 Atom 依赖）
#
# 本目标必须保持为叶子节点：只含类型与 inline 工具，不链接任何 ATOM，
# 否则会与 AppCoordinator 形成依赖环。
#   允许：MarshlandTypes.hpp、ILifecycle.hpp、StateArbitrationTypes.hpp
#   禁止：任何 AppCoordinator 相关源码（见 Atoms/Public/AppCoordinator/）
# ============================================================================
add_library(marshland_public_headers INTERFACE)
add_library(Marshland::PublicHeaders ALIAS marshland_public_headers)

target_include_directories(marshland_public_headers INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>")

target_sources(marshland_public_headers INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/MarshlandTypes.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ILifecycle.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StateArbitrationTypes.hpp")

# ---------------------------------------------------------------------------
# 顶层装配与桥接层（AppCoordinator）
#
# 位置在 Public/ 之下只是为了就近使用共享类型；它的依赖方向是**自上而下**的，
# 持有 RuntimeCoordinator 与 StateArbitrator，因此**不属于**上面的公共契约。
# 其余 18 个 ATOM 禁止包含 AppCoordinator.hpp。
#
# 必须在 add_subdirectory(Atoms) 之后引入，因为需要 marshland_atoms 已存在。
# ---------------------------------------------------------------------------
add_subdirectory(AppCoordinator)

add_library(marshland_public INTERFACE)
add_library(Marshland::Public ALIAS marshland_public)

target_link_libraries(marshland_public INTERFACE
    Marshland::PublicHeaders
    marshland_atoms
    marshland_app)
