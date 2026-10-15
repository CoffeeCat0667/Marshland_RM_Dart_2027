# ============================================================================
# ThirdParty/civetweb — civetweb/civetweb 1.16 (MIT)
#
# 本文件是为 Marshland 工程重写的最小集成文件，替代上游原 CMakeLists.txt：
#   - 只编译嵌入式库 src/civetweb.c，不构建上游独立服务器（main.c）、
#     C++ 包装（CivetServer）、示例、测试与安装逻辑（相关文件已裁剪）；
#   - 目标运行主机为 Linux ARM64，仅启用本工程需要的特性：
#       USE_WEBSOCKET  WebSocket 服务端支持（ATOM-17 必须；v1.16 默认关闭）
#       NO_SSL         不启用 TLS，避免 OpenSSL 运行时/链接依赖（设计为明文 ws://）
#       NO_CGI         不需要 CGI；
#   - 上游源码保持原样，不做任何修改。
# ============================================================================

if(TARGET marshland_thirdparty_civetweb)
    return()
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/civetweb.c"
        OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/civetweb.h")
    message(FATAL_ERROR
        "ThirdParty/civetweb: 缺少 src/civetweb.c 或 include/civetweb.h"
        "（civetweb 1.16，https://github.com/civetweb/civetweb/releases）")
endif()

# 上层工程 project() 只声明了 CXX；civetweb.c 是 C 源文件，需在本作用域启用 C。
# 交叉编译时 CMake 默认会探测到本机 x86 的 cc，因此先从 CXX 编译器推导
# 同前缀的 C 交叉编译器（aarch64-linux-gnu-g++ -> aarch64-linux-gnu-gcc）。
if(NOT DEFINED CMAKE_C_COMPILER AND DEFINED CMAKE_CXX_COMPILER)
    get_filename_component(_civetweb_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
    set(_civetweb_cc_name "")
    if(_civetweb_cxx_name MATCHES "^(.*)g\\+\\+(.*)$")
        set(_civetweb_cc_name "${CMAKE_MATCH_1}gcc${CMAKE_MATCH_2}")
    elseif(_civetweb_cxx_name MATCHES "^(.*)clang\\+\\+(.*)$")
        set(_civetweb_cc_name "${CMAKE_MATCH_1}clang${CMAKE_MATCH_2}")
    endif()
    if(_civetweb_cc_name)
        get_filename_component(_civetweb_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(EXISTS "${_civetweb_cxx_dir}/${_civetweb_cc_name}")
            set(CMAKE_C_COMPILER "${_civetweb_cxx_dir}/${_civetweb_cc_name}")
        else()
            find_program(_civetweb_cc_found NAMES "${_civetweb_cc_name}")
            if(_civetweb_cc_found)
                set(CMAKE_C_COMPILER "${_civetweb_cc_found}")
            endif()
        endif()
    endif()
endif()
enable_language(C)

add_library(marshland_thirdparty_civetweb STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/civetweb.c")
add_library(Marshland::ThirdPartyCivetweb ALIAS marshland_thirdparty_civetweb)

target_compile_definitions(marshland_thirdparty_civetweb PUBLIC
    USE_WEBSOCKET
    NO_SSL
    NO_CGI)

# 公开头为 include/civetweb.h；SYSTEM 抑制第三方头告警
target_include_directories(marshland_thirdparty_civetweb SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Linux 下需要 pthread；${CMAKE_DL_LIBS} 覆盖个别平台对 dl 的引用
find_package(Threads REQUIRED)
target_link_libraries(marshland_thirdparty_civetweb PUBLIC
    Threads::Threads ${CMAKE_DL_LIBS})
