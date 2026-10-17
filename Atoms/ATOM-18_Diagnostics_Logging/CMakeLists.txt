# ============================================================================
# ATOM-18_Diagnostics_Logging — 日志系统（唯二对外接口：Public/Log.hpp）
#
# 2026-10-16 用户裁决（见 PLAN.md §0）：
#   * 对外接口收敛为两个函数：marshland::log::Init / marshland::log::Log
#     （声明在 ../Public/Log.hpp，已注册进 Marshland::PublicHeaders）；
#   * 不再依赖装配层或其他 ATOM 的任何公开接口（无 IConfigView /
#     IMonotonicClock / ILifecycle 注入，配置为模块内编译期常量）；
#   * 无第三方依赖、无后台线程；日志实现全部在 Src/，Inc/ 不再含私有头。
#
# 仍与上层约定一致：由上层 add_subdirectory() 组装；不定义 project()/main()；
# 不设置编译器、目标平台与 C++ 标准；STATIC 库，别名 Marshland::<目标名>。
# ============================================================================

set(ATOM_MODULE_TARGET atom_18_diagnostics_logging)
set(ATOM_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(TARGET ${ATOM_MODULE_TARGET})
    return()
endif()

# CONFIGURE_DEPENDS 使新增文件在下一次 configure 时自动生效
file(GLOB_RECURSE ATOM_MODULE_SOURCES CONFIGURE_DEPENDS
    "${ATOM_MODULE_DIR}/Src/*.cpp")
file(GLOB_RECURSE ATOM_MODULE_HEADERS CONFIGURE_DEPENDS
    "${ATOM_MODULE_DIR}/Inc/*.hpp")

add_library(${ATOM_MODULE_TARGET} STATIC ${ATOM_MODULE_SOURCES} ${ATOM_MODULE_HEADERS})
add_library(Marshland::${ATOM_MODULE_TARGET} ALIAS ${ATOM_MODULE_TARGET})

# 本模块私有包含目录（当前为空）+ 公共契约头所在目录（Log.hpp）
# 包含目录为 PRIVATE：不向其他模块传播本模块内部结构
target_include_directories(${ATOM_MODULE_TARGET} PRIVATE
    "$<BUILD_INTERFACE:${ATOM_MODULE_DIR}/Inc>"
    "$<BUILD_INTERFACE:${ATOM_MODULE_DIR}/../Public>")

# 以公开契约头目标为准建立依赖关系（顶层在 Atoms/ 之前引入 PublicHeaders）
if(TARGET Marshland::PublicHeaders)
    target_link_libraries(${ATOM_MODULE_TARGET} PRIVATE Marshland::PublicHeaders)
endif()
