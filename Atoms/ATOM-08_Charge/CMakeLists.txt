# ============================================================================
# ATOM-08_Charge — 蓄力机构控制
#
# 统一模块契约（与上层 NewVersion/CMakeLists.txt 保持一致）：
#   1. 由上层工程通过 add_subdirectory() 组装；本文件不定义 project()。
#   2. 不产生可执行文件，不定义 main()。
#   3. 不设置编译器、目标平台、C++ 标准与编译选项，全部由上层工程决定。
#   4. 公开头文件放 Inc/，实现源码放 Src/；header-only 模块头文件直接放本目录。
#   5. 有 Src/ 源码时建 STATIC 库，否则建 INTERFACE 库；上层统一使用
#      Marshland::<目标名> 链接。
# ============================================================================

set(ATOM_MODULE_TARGET atom_08_charge)
set(ATOM_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

if(TARGET ${ATOM_MODULE_TARGET})
    return()
endif()

# 公开包含目录：本目录(header-only 布局) / Inc/ / 同级共享 Public/
set(ATOM_MODULE_INCLUDE_DIRS "")
foreach(ATOM_MODULE_INC
        "${ATOM_MODULE_DIR}"
        "${ATOM_MODULE_DIR}/Inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Public")
    if(IS_DIRECTORY "${ATOM_MODULE_INC}")
        list(APPEND ATOM_MODULE_INCLUDE_DIRS "$<BUILD_INTERFACE:${ATOM_MODULE_INC}>")
    endif()
endforeach()

# 源码与头文件；CONFIGURE_DEPENDS 使新增文件在下一次 configure 时自动生效
file(GLOB_RECURSE ATOM_MODULE_SOURCES CONFIGURE_DEPENDS
    "${ATOM_MODULE_DIR}/Src/*.c" "${ATOM_MODULE_DIR}/Src/*.cc"
    "${ATOM_MODULE_DIR}/Src/*.cpp" "${ATOM_MODULE_DIR}/Src/*.cxx")
file(GLOB_RECURSE ATOM_MODULE_HEADERS CONFIGURE_DEPENDS
    "${ATOM_MODULE_DIR}/*.h" "${ATOM_MODULE_DIR}/*.hh" "${ATOM_MODULE_DIR}/*.hpp"
    "${ATOM_MODULE_DIR}/Inc/*.h" "${ATOM_MODULE_DIR}/Inc/*.hh" "${ATOM_MODULE_DIR}/Inc/*.hpp")

# 有源码 -> STATIC（参与增量编译）；无源码 -> INTERFACE（占位且可正常链接）
if(ATOM_MODULE_SOURCES)
    add_library(${ATOM_MODULE_TARGET} STATIC ${ATOM_MODULE_SOURCES} ${ATOM_MODULE_HEADERS})
    set(ATOM_MODULE_SCOPE PUBLIC)
else()
    add_library(${ATOM_MODULE_TARGET} INTERFACE)
    set(ATOM_MODULE_SCOPE INTERFACE)
    if(ATOM_MODULE_HEADERS)
        target_sources(${ATOM_MODULE_TARGET} INTERFACE ${ATOM_MODULE_HEADERS})
    endif()
endif()

add_library(Marshland::${ATOM_MODULE_TARGET} ALIAS ${ATOM_MODULE_TARGET})

if(ATOM_MODULE_INCLUDE_DIRS)
    target_include_directories(${ATOM_MODULE_TARGET} ${ATOM_MODULE_SCOPE}
        ${ATOM_MODULE_INCLUDE_DIRS})
endif()
