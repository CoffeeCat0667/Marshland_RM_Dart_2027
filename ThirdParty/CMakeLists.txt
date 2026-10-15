# ThirdParty — 上层共享第三方头文件库
#
# 内容（均为 header-only，无编译单元）：
#   httplib/httplib.h    cpp-httplib 0.61.0  (MIT, https://github.com/yhirose/cpp-httplib)
#   nlohmann/json.hpp    nlohmann/json 3.12.0 (MIT, https://github.com/nlohmann/json)
# 另有一个带编译单元的库：
#   civetweb/            civetweb 1.16       (MIT, https://github.com/civetweb/civetweb)
#
# 约定：
#   - 消费方以 ThirdParty 根为 include 根，写 #include "httplib/httplib.h" 或
#     #include "nlohmann/json.hpp"。
#   - include 路径以 SYSTEM 方式导出，抑制第三方头告警。
#   - 各消费模块应 PRIVATE 链接，避免第三方头泄漏到对外接口。

cmake_minimum_required(VERSION 3.20)

# --- cpp-httplib -------------------------------------------------------------

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/httplib/httplib.h")
    message(FATAL_ERROR
        "ThirdParty: 缺少 httplib/httplib.h（cpp-httplib，要求 >= 0.40.0，"
        "请从 https://github.com/yhirose/cpp-httplib/releases 下载）")
endif()

add_library(marshland_thirdparty_httplib INTERFACE)
add_library(Marshland::ThirdPartyHttplib ALIAS marshland_thirdparty_httplib)
target_include_directories(marshland_thirdparty_httplib SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}")

# --- nlohmann/json -----------------------------------------------------------

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR
        "ThirdParty: 缺少 nlohmann/json.hpp（nlohmann/json，"
        "请从 https://github.com/nlohmann/json/releases 下载单头 json.hpp）")
endif()

add_library(marshland_thirdparty_nlohmann_json INTERFACE)
add_library(Marshland::ThirdPartyNlohmannJson ALIAS marshland_thirdparty_nlohmann_json)
target_include_directories(marshland_thirdparty_nlohmann_json SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}")

# --- json-schema-validator (pboettch) --------------------------------------
# 集成逻辑收敛在 pboettch/CMakeLists.txt（已为本工程重写，不使用上游构建文件；
# 该目标依赖上面的 Marshland::ThirdPartyNlohmannJson）

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pboettch/src/nlohmann/json-schema.hpp")
    message(FATAL_ERROR
        "ThirdParty: 缺少 pboettch/src/nlohmann/json-schema.hpp"
        "（pboettch/json-schema-validator 2.4.0，"
        "https://github.com/pboettch/json-schema-validator/releases）")
endif()

add_subdirectory(pboettch)

# --- civetweb ---------------------------------------------------------------
# 集成逻辑收敛在 civetweb/CMakeLists.txt（已为本工程重写，不使用上游构建文件）；
# 目标运行主机为 Linux ARM64，仅启用 WebSocket（ATOM-17 用），禁用 SSL/CGI。

add_subdirectory(civetweb)
