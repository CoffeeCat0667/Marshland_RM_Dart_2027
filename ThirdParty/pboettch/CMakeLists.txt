# ============================================================================
# ThirdParty/pboettch — pboettch/json-schema-validator 2.4.0 (MIT)
#
# 本文件是为 Marshland 工程重写的最小集成文件，替代上游原 CMakeLists.txt：
#   - 上游通过 FetchContent 联网拉取 nlohmann/json，本工程改为复用
#     Marshland::ThirdPartyNlohmannJson（ThirdParty/nlohmann/json.hpp 3.12.0）；
#   - 上游的 install / test / example / shared-lib 逻辑全部不启用；
#   - 只产出一个静态库 Marshland::ThirdPartyJsonSchemaValidator。
#
# 上游源码（src/ 及本目录其余文件）保持原样，不做任何修改。
# ============================================================================

if(TARGET marshland_thirdparty_json_schema_validator)
    return()
endif()

if(NOT TARGET Marshland::ThirdPartyNlohmannJson)
    message(FATAL_ERROR
        "ThirdParty/pboettch: 需要先定义 Marshland::ThirdPartyNlohmannJson"
        "（ThirdParty/CMakeLists.txt 中 nlohmann 段必须先于 add_subdirectory(pboettch)）")
endif()

set(PBOETTCH_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# json-validator.cpp 链接时需要 json-patch / smtp-address-validator 的实现
add_library(marshland_thirdparty_json_schema_validator STATIC
    "${PBOETTCH_SRC}/json-uri.cpp"
    "${PBOETTCH_SRC}/json-validator.cpp"
    "${PBOETTCH_SRC}/json-schema-draft7.json.cpp"
    "${PBOETTCH_SRC}/json-patch.cpp"
    "${PBOETTCH_SRC}/string-format-check.cpp"
    "${PBOETTCH_SRC}/smtp-address-validator.cpp")
add_library(Marshland::ThirdPartyJsonSchemaValidator
    ALIAS marshland_thirdparty_json_schema_validator)

# 公开头为 src/nlohmann/json-schema.hpp；SYSTEM 抑制第三方头告警
target_include_directories(marshland_thirdparty_json_schema_validator SYSTEM PUBLIC
    "${PBOETTCH_SRC}")

target_link_libraries(marshland_thirdparty_json_schema_validator PUBLIC
    Marshland::ThirdPartyNlohmannJson)
