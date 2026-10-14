// ============================================================================
// ErrorMapping.hpp — 下游结果类型 -> 统一错误码（Config.md §11.3）
//
// 把 IControlAuthority / IStateReader / IConfigView 返回的拒绝原因集中映射为
// ApiError 的 ErrorCode，全模块只有这一处映射表，避免各路由各自判断。
// ============================================================================
#pragma once

#include "ApiError.hpp"
#include "AppInterfaces.hpp"
#include "IConfigView.hpp"
#include "StateArbitrationTypes.hpp"

namespace marshland::http_web {

// RejectReason::NONE 表示无拒绝；调用方只在 accepted==false 时映射。
inline ErrorCode MapRejectReason(state_arbitration::RejectReason reason) noexcept
{
    using state_arbitration::RejectReason;
    switch (reason) {
    case RejectReason::INVALID_INTENT:          return ErrorCode::INVALID_ARGUMENT;
    case RejectReason::UNKNOWN_TRANSITION:      return ErrorCode::INVALID_STATE;
    case RejectReason::ALREADY_IN_TARGET:       return ErrorCode::INVALID_STATE;
    case RejectReason::NOT_READY:               return ErrorCode::INVALID_STATE;
    case RejectReason::NOT_AUTO_FIRE_MODE:      return ErrorCode::INVALID_STATE;
    case RejectReason::AUTO_FLOW_ACTIVE:        return ErrorCode::INVALID_STATE;
    case RejectReason::AUTO_FLOW_TERMINATED:    return ErrorCode::INVALID_STATE;
    case RejectReason::AUTO_FLOW_NOT_STARTED:   return ErrorCode::INVALID_STATE;
    case RejectReason::NOT_SBUS_TAKEOVER:       return ErrorCode::INVALID_STATE;
    case RejectReason::SHOT_STAGE_MISMATCH:     return ErrorCode::INVALID_STATE;
    case RejectReason::SOFTWARE_ESTOP:          return ErrorCode::ESTOP_ACTIVE;
    case RejectReason::ESTOP_NOT_CONFIRMED:     return ErrorCode::ESTOP_ACTIVE;
    case RejectReason::FAULT_ACTIVE:            return ErrorCode::FAULT_ACTIVE;
    case RejectReason::STOP_FIRE_SET:           return ErrorCode::STOP_FIRE_ACTIVE;
    case RejectReason::YAW_NOT_ARRIVED:         return ErrorCode::INVALID_STATE;
    case RejectReason::LOWER_PRIORITY_CONFLICT: return ErrorCode::CONTROL_PREEMPTED;
    case RejectReason::NO_CONTROL_AUTHORITY:    return ErrorCode::CONTROL_PREEMPTED;
    case RejectReason::FIRE_NOT_AUTHORIZED:     return ErrorCode::CONTROL_PREEMPTED;
    case RejectReason::NONE:                    break;
    }
    return ErrorCode::INTERNAL_ERROR;
}

// RunGateReason::ALLOWED 表示放行；调用方只在非 ALLOWED 时映射。
inline ErrorCode MapRunGateReason(app::RunGateReason reason) noexcept
{
    using app::RunGateReason;
    switch (reason) {
    case RunGateReason::HARDWARE_NOT_READY:    return ErrorCode::INVALID_STATE;
    case RunGateReason::ESTOP_LATCHED:         return ErrorCode::ESTOP_ACTIVE;
    case RunGateReason::FAULT_ACTIVE:          return ErrorCode::FAULT_ACTIVE;
    case RunGateReason::MODE_NOT_OPERATIONAL:  return ErrorCode::INVALID_STATE;
    case RunGateReason::NOT_INITIALIZED:       return ErrorCode::NOT_INITIALIZED;
    case RunGateReason::ALLOWED:               break;
    }
    return ErrorCode::INTERNAL_ERROR;
}

inline ErrorCode MapConfigStatus(config::ConfigStatus status) noexcept
{
    using config::ConfigStatus;
    switch (status) {
    case ConfigStatus::NOT_LOADED:   return ErrorCode::NOT_INITIALIZED;
    case ConfigStatus::MISSING:      return ErrorCode::CONFIGURATION_ERROR;
    case ConfigStatus::INVALID_TYPE: return ErrorCode::CONFIGURATION_ERROR;
    case ConfigStatus::OUT_OF_RANGE: return ErrorCode::VALUE_OUT_OF_RANGE;
    case ConfigStatus::OK:           break;
    }
    return ErrorCode::INTERNAL_ERROR;
}

} // namespace marshland::http_web
