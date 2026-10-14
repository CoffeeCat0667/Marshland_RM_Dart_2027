// ============================================================================
// ApiError.hpp — 统一错误码与 HTTP 状态映射（Config.md §11.3）
//
// 错误码集合逐字来自 Config.md:492（15 个），错误 JSON 结构逐字来自
// Config.md:491：{"request_id":"...","ok":false,"error":{"code","message","details"}}
//
// 注意（PLAN §7.5）：ConfigSchema.md §2.12.2 要求错误码表与状态映射经
// http.error_codes / http.status_mapping 配置下发、禁止代码硬编码；但
// IConfigView 目前无法查询 object 结构。在对象查询路径落地前，本表为
// 过渡实现，唯一依据是 Config.md:492「映射 400/408/409/413/422/500/503，
// 未知错误使用 500」加上 §11.1 的两条隐含映射（超限→413、超时→408）。
// 逐码对应关系是本模块的提议，待配置路径接通后以配置为准。
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland::http_web {

enum class ErrorCode : std::uint8_t {
    INVALID_JSON,
    INVALID_ARGUMENT,
    INVALID_STATE,
    CONTROL_PREEMPTED,
    STOP_FIRE_ACTIVE,
    ESTOP_ACTIVE,
    FAULT_ACTIVE,
    NOT_INITIALIZED,
    NOT_FOUND,
    VALUE_OUT_OF_RANGE,
    PAYLOAD_TOO_LARGE,
    CONFIGURATION_ERROR,
    DEPENDENCY_UNAVAILABLE,
    REQUEST_TIMEOUT,
    INTERNAL_ERROR
};

inline const char* ToString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::INVALID_JSON:           return "INVALID_JSON";
    case ErrorCode::INVALID_ARGUMENT:       return "INVALID_ARGUMENT";
    case ErrorCode::INVALID_STATE:          return "INVALID_STATE";
    case ErrorCode::CONTROL_PREEMPTED:      return "CONTROL_PREEMPTED";
    case ErrorCode::STOP_FIRE_ACTIVE:       return "STOP_FIRE_ACTIVE";
    case ErrorCode::ESTOP_ACTIVE:           return "ESTOP_ACTIVE";
    case ErrorCode::FAULT_ACTIVE:           return "FAULT_ACTIVE";
    case ErrorCode::NOT_INITIALIZED:        return "NOT_INITIALIZED";
    case ErrorCode::NOT_FOUND:              return "NOT_FOUND";
    case ErrorCode::VALUE_OUT_OF_RANGE:     return "VALUE_OUT_OF_RANGE";
    case ErrorCode::PAYLOAD_TOO_LARGE:      return "PAYLOAD_TOO_LARGE";
    case ErrorCode::CONFIGURATION_ERROR:    return "CONFIGURATION_ERROR";
    case ErrorCode::DEPENDENCY_UNAVAILABLE: return "DEPENDENCY_UNAVAILABLE";
    case ErrorCode::REQUEST_TIMEOUT:        return "REQUEST_TIMEOUT";
    case ErrorCode::INTERNAL_ERROR:         return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

// 错误码 -> HTTP 状态。集合封闭于 Config.md:492 列出的 400/408/409/413/422/
// 500/503；唯一例外是 NOT_FOUND -> 404（路由级未知路径遵循 HTTP 语义，
// Config.md 未列出 404，见 PLAN §7.5 待澄清项）。
inline int ToHttpStatus(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::INVALID_JSON:           return 400;
    case ErrorCode::INVALID_ARGUMENT:       return 400;
    case ErrorCode::INVALID_STATE:          return 409;
    case ErrorCode::CONTROL_PREEMPTED:      return 409;
    case ErrorCode::STOP_FIRE_ACTIVE:       return 409;
    case ErrorCode::ESTOP_ACTIVE:           return 409;
    case ErrorCode::FAULT_ACTIVE:           return 409;
    case ErrorCode::NOT_INITIALIZED:        return 503;
    case ErrorCode::NOT_FOUND:              return 404;
    case ErrorCode::VALUE_OUT_OF_RANGE:     return 422;
    case ErrorCode::PAYLOAD_TOO_LARGE:      return 413;
    case ErrorCode::CONFIGURATION_ERROR:    return 500;
    case ErrorCode::DEPENDENCY_UNAVAILABLE: return 503;
    case ErrorCode::REQUEST_TIMEOUT:        return 408;
    case ErrorCode::INTERNAL_ERROR:         return 500;
    }
    return 500;
}

} // namespace marshland::http_web
