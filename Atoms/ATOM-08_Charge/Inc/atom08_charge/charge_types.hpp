// ============================================================================
// charge_types.hpp — ATOM-08 蓄力机构控制：领域类型
//
// 依据：Design/Atomic/ATOM-08_Charge.md（单一职责与内部原子操作 1~11）
//       Design/Config.md §4、§5.1、§5.5
//       Rule/AtomIntegration.md v1.4、Rule/ConfigSchema.md v1.4
//
// 只依赖 C++ 标准库与 Atoms/Public 契约（MarshlandTypes.hpp）。
// 禁止包含任何 ATOM-NN 私有头、AppCoordinator.hpp、SystemMonotonicClock.hpp。
// ============================================================================
#pragma once

#include <cstdint>

#include "MarshlandTypes.hpp"

namespace marshland::atom08 {

// ---------------------------------------------------------------------------
// 动作状态（对外查询；对应 Config.md §12.3 mechanism_state.charge 的 state）
// ---------------------------------------------------------------------------
enum class ChargeState : std::uint8_t {
    Uninitialized,     // 未配置/未初始化
    Idle,              // 空闲，可接受蓄力或回零请求
    Charging,          // 双电机恒速蓄力并累计 X 秒
    ChargeCompleted,   // X 秒到期，正常蓄力完成
    Returning,         // 双电机执行回零目标
    ReturnCompleted,   // 双限位同时有效且稳定为低，回零完成
    Stopped,           // 普通停止后空闲（不等价于回零完成）
    Fault,             // 故障，保留最近错误并停止相关动作
};

inline const char* ToString(ChargeState state) noexcept
{
    switch (state) {
    case ChargeState::Uninitialized:   return "UNINITIALIZED";
    case ChargeState::Idle:            return "IDLE";
    case ChargeState::Charging:        return "CHARGING";
    case ChargeState::ChargeCompleted: return "CHARGE_COMPLETED";
    case ChargeState::Returning:       return "RETURNING";
    case ChargeState::ReturnCompleted: return "RETURN_COMPLETED";
    case ChargeState::Stopped:         return "STOPPED";
    case ChargeState::Fault:           return "FAULT";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 当前/最近一次动作
// ---------------------------------------------------------------------------
enum class ChargeAction : std::uint8_t {
    None,
    Charge,
    Home,
};

inline const char* ToString(ChargeAction action) noexcept
{
    switch (action) {
    case ChargeAction::None:   return "NONE";
    case ChargeAction::Charge: return "CHARGE";
    case ChargeAction::Home:   return "HOME";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 速度目标来源
// ---------------------------------------------------------------------------
enum class ChargeTargetSource : std::uint8_t {
    // 按当前控制源解析：SBUS → CH2/CH8/CH9 通道快照；WEB/AUTO → 配置恒定速度
    FromControlSource,
    // 调用方显式给出左右目标（ATOM-13 自动流程、调试或测试替身）
    Explicit,
};

// ---------------------------------------------------------------------------
// 请求结果（不复用 RejectReason：本枚举同时覆盖本地校验失败与仲裁拒绝）
// ---------------------------------------------------------------------------
enum class ChargeRequestResult : std::uint8_t {
    Accepted,
    RejectedNotInitialized,      // 未配置/未初始化
    RejectedHardwareNotReady,    // 硬件未就绪（RunGate 语义的 NOT_INITIALIZED/HARDWARE_NOT_READY）
    RejectedEstopLocked,         // 急停锁定（SOFTWARE_ESTOP）
    RejectedFaultActive,         // 故障活动
    RejectedMode,                // 当前系统模式不允许该动作
    RejectedNoControlAuthority,  // 无控制权或优先级不足
    RejectedArbitration,         // 其他仲裁拒绝
    RejectedTargetUnavailable,   // 目标无法解析（SBUS 离线或通道未映射）
    RejectedTargetOutOfRange,    // 目标超出 charge.target_range
    RejectedMotorCommand,        // 电机目标提交失败
    RejectedBusy,                // 已有蓄力/回零动作在执行
};

inline const char* ToString(ChargeRequestResult result) noexcept
{
    switch (result) {
    case ChargeRequestResult::Accepted:                  return "ACCEPTED";
    case ChargeRequestResult::RejectedNotInitialized:    return "REJECTED_NOT_INITIALIZED";
    case ChargeRequestResult::RejectedHardwareNotReady:  return "REJECTED_HARDWARE_NOT_READY";
    case ChargeRequestResult::RejectedEstopLocked:       return "REJECTED_ESTOP_LOCKED";
    case ChargeRequestResult::RejectedFaultActive:       return "REJECTED_FAULT_ACTIVE";
    case ChargeRequestResult::RejectedMode:              return "REJECTED_MODE";
    case ChargeRequestResult::RejectedNoControlAuthority:return "REJECTED_NO_CONTROL_AUTHORITY";
    case ChargeRequestResult::RejectedArbitration:       return "REJECTED_ARBITRATION";
    case ChargeRequestResult::RejectedTargetUnavailable: return "REJECTED_TARGET_UNAVAILABLE";
    case ChargeRequestResult::RejectedTargetOutOfRange:  return "REJECTED_TARGET_OUT_OF_RANGE";
    case ChargeRequestResult::RejectedMotorCommand:      return "REJECTED_MOTOR_COMMAND";
    case ChargeRequestResult::RejectedBusy:              return "REJECTED_BUSY";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 本模块可上报的故障类别
//
// 注意：这不是 Config.md §14.2 的故障码本体，而是本模块归类后的上报项；
// 由装配层/ATOM-14 映射到故障表条目。ConfigSchema.md §2.15 禁止新增表外故障码。
// ---------------------------------------------------------------------------
enum class ChargeFaultCode : std::uint8_t {
    None,
    ChargeTimeout,        // 超过 timing.charge_max_action_ms 仍未完成蓄力
    HomeTimeout,          // 超过 timing.charge_home_max_action_ms 仍未回零完成
    MotorCommandFailed,   // 任一电机目标提交失败（联动停止另一侧）
    LimitInputInvalid,    // 回零期间限位输入无效（对应 F-GPIO-001 语义）
    ConfigReloadFailed,   // 热加载失败，保留上一份有效配置
};

inline const char* ToString(ChargeFaultCode code) noexcept
{
    switch (code) {
    case ChargeFaultCode::None:              return "NONE";
    case ChargeFaultCode::ChargeTimeout:     return "CHARGE_TIMEOUT";
    case ChargeFaultCode::HomeTimeout:       return "HOME_TIMEOUT";
    case ChargeFaultCode::MotorCommandFailed:return "MOTOR_COMMAND_FAILED";
    case ChargeFaultCode::LimitInputInvalid: return "LIMIT_INPUT_INVALID";
    case ChargeFaultCode::ConfigReloadFailed:return "CONFIG_RELOAD_FAILED";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 只读状态快照（供 mechanism_state.charge 等查询使用）
// ---------------------------------------------------------------------------
struct ChargeStatusSnapshot final {
    ChargeState state{ChargeState::Uninitialized};
    ChargeAction last_action{ChargeAction::None};   // 最近一次动作（完成后仍保留，非"当前动作"）
    bool charge_completed{false};
    bool home_completed{false};
    std::int32_t left_target_rpm{0};   // 电机 role=charge_left
    std::int32_t right_target_rpm{0};  // 电机 role=charge_right
    ChargeFaultCode last_fault{ChargeFaultCode::None};
    ChargeRequestResult last_result{ChargeRequestResult::RejectedNotInitialized};
    std::uint64_t last_fault_time_us{0};
};

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------
struct ChargeStartRequest final {
    ChargeTargetSource source{ChargeTargetSource::FromControlSource};
    std::int32_t left_rpm{0};                         // source == Explicit 时有效
    std::int32_t right_rpm{0};                        // source == Explicit 时有效
    marshland::ControlSource requester{marshland::ControlSource::WEB};
    std::uint32_t request_id{0};
    std::uint64_t timestamp_us{0};                    // 0 表示由本模块取单调时钟
};

struct ChargeHomeRequest final {
    ChargeTargetSource source{ChargeTargetSource::FromControlSource};
    std::int32_t left_rpm{0};                         // source == Explicit 时有效
    std::int32_t right_rpm{0};                        // source == Explicit 时有效
    marshland::ControlSource requester{marshland::ControlSource::WEB};
    std::uint32_t request_id{0};
    std::uint64_t timestamp_us{0};
};

} // namespace marshland::atom08