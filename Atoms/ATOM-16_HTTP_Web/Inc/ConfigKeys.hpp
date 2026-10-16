// ============================================================================
// ConfigKeys.hpp — ATOM-16 消费的配置键名（Rule/ConfigSchema.md §2.13 逐字）
//
// 权威来源：
//   Rule/ConfigSchema.md §2.12.1 服务、§2.12.2 路由、§2.12.3 动作集
//   Design/Config.md     §11.1～§11.4（数值与语义以该文档为准）
//
// 键名是 IConfigView 的公开 API（ConfigSchema.md §1.7 第 4 条），禁止改名。
// 本文件只放键名常量；数值默认值不在这里定义——缺失即初始化失败（fail-closed，
// 见 IConfigView.hpp「禁止返回默认值+无提示掩盖错误」与 Config.md §17.8）。
// ============================================================================
#pragma once

namespace marshland::http_web::config_keys {

// ---- 服务（http.*，ConfigSchema.md §2.12.1 / Config.md §11.1） -------------
inline constexpr const char* kListenAddress          = "http.listen_address";            // str，0.0.0.0
inline constexpr const char* kPort                   = "http.port";                      // int，10000
inline constexpr const char* kStaticResourceProvider = "http.static_resource_provider";  // str，MAIN_PROGRAM
inline constexpr const char* kCharset                = "http.charset";                   // str，UTF-8
inline constexpr const char* kRequestBodyLimitBytes  = "http.request_body_limit_bytes";  // int，64 KiB
inline constexpr const char* kResponseBodyLimitBytes = "http.response_body_limit_bytes"; // int，1 MiB
inline constexpr const char* kRequestTimeoutMs       = "http.request_timeout_ms";        // int，200

// ---- 路由与错误结构（http.routes[] 等，ConfigSchema.md §2.12.2） ------------
// 注意：http.routes[] / http.error_response_template / http.error_codes /
// http.status_mapping 为 object 或含嵌套结构的数组，IConfigView 目前只能查询
// 标量与「数组元素的标量字段」，对象查询路径待与 ATOM-15 对齐（PLAN §7.5）。
inline constexpr const char* kRoutes                = "http.routes";
inline constexpr const char* kErrorResponseTemplate = "http.error_response_template";
inline constexpr const char* kErrorCodes            = "http.error_codes";
inline constexpr const char* kStatusMapping         = "http.http_status_mapping";  // 以 schema.json 为准（ConfigSchema.md 文档名 status_mapping 不一致，BUG-20261015-16-02）

// ---- 手动控制动作集（http.manual_actions[]，ConfigSchema.md §2.12.3） -------
inline constexpr const char* kManualActions = "http.manual_actions";
// 元素键：action(str) / requires_target(bool) / target_unit(str?) / allowed_modes(array<enum>)

} // namespace marshland::http_web::config_keys
