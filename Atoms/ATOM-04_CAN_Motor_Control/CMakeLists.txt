# ============================================================================
# ATOM-04_CAN_Motor_Control — CAN 电机闭环控制（SocketCAN + PID）
#   对外两个入口：start()（独立接收线程）/ setRpm()（目标转速，节点 0x201~0x206）
#   接收线程内每 20ms 执行 PID 并发送 0x200 / 0x1FF；另有 stop()/isRunning()/state()
#   翻译单元：Src/can_motor_control.cpp、Src/motor_frame_codec.cpp、Src/pid.cpp
#
# 统一模块契约（与上层 NewVersion/CMakeLists.txt 保持一致）：
#   1. 由上层工程通过 add_subdirectory() 组装；本文件不定义 project()。
#   2. 不产生可执行文件，不定义 main()。
#   3. 不设置编译器、目标平台、C++ 标准与编译选项，全部由上层工程决定。
#   4. 公开头文件放 Inc/，实现源码放 Src/。
#   5. 有 Src/ 源码时建 STATIC 库；上层统一使用 Marshland::<目标名> 链接。
#
# 目标名：atom_04_can_motor_control（用户裁决，与目录名 ATOM-04_CAN_Motor_Control 一致）
#
# 上层同步状态（2026-10-16 已完成）：
#   NewVersion/CMakeLists.txt 与 Atoms/CMakeLists.txt 均已引用
#   Marshland::atom_04_can_motor_control；ATOM-04 的测试目标已注销——现有
#   Tests/Atom04CanTests.cpp 针对的是已删除的公开面（CanCore/CanRuntimeConfig，
#   现已被 CanMotorControl 取代）。详见 PLAN.md §7.2。
# ============================================================================

set(ATOM_MODULE_TARGET atom_04_can_motor_control)
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

# 源码与头文件；CONFIGURE_DEPENDS 使新增文件在下一次 configure 时自动生效。
# 注意：已移除但尚未物理删除的文件只剩注释，作为空翻译单元参与编译是无害的。
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

if(TARGET Marshland::PublicHeaders)
    target_link_libraries(${ATOM_MODULE_TARGET} ${ATOM_MODULE_SCOPE}
        Marshland::PublicHeaders)
endif()

# SocketCAN 不需要任何第三方库：只依赖 libc 与内核头（<linux/can.h> 等）。
# 因此本模块不链接任何外部目标，也不定义编译/链接选项（全部由上层工程决定）。
