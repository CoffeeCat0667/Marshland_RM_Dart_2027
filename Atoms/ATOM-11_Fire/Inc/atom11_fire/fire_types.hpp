// ============================================================================
// fire_types.hpp — ATOM-11 领域类型与状态枚举
//
// 权威来源：
//   FireActionState  逐字对齐 Design/Interface.md §6.5，禁止增删成员或改名。
//   FireStepOutcome  每个原子步骤的独立结果（Design/Atomic/ATOM-11_Fire.md：
//                    每步独立返回结果，任何普通阶段失败都不得虚报完成）。
//
// 约束：只依赖 C++ 标准库；不含任何 Atom 私有头。
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland::atom11 {

// Design/Interface.md §6.5（字面量，禁止改名/增删/调序）
enum class FireActionState : std::uint8_t {
    IDLE,
    MOVING_TO_FIRE,
    HOLDING,
    RETURNING,
    COMPLETED,
    FAILED
};

inline const char* ToString(FireActionState state) noexcept
{
    switch (state) {
    case FireActionState::IDLE:           return "IDLE";
    case FireActionState::MOVING_TO_FIRE: return "MOVING_TO_FIRE";
    case FireActionState::HOLDING:        return "HOLDING";
    case FireActionState::RETURNING:      return "RETURNING";
    case FireActionState::COMPLETED:      return "COMPLETED";
    case FireActionState::FAILED:         return "FAILED";
    }
    return "UNKNOWN";
}

// 每个原子步骤的独立结果，供调用方逐次确认
enum class FireStepOutcome : std::uint8_t {
    OK,
    REJECTED_ESTOP,          // 软件急停锁存：拒绝任何发射舵机命令
    REJECTED_FAULT,          // 故障激活：不写发射目标
    REJECTED_NOT_AUTHORIZED, // 授权结论 allowed == false
    REJECTED_NOT_READY,      // 未配置 / 上一次动作仍在进行
    ANGLE_REJECTED,          // 角度越界（W-CMD-001）
    SERVO_WRITE_FAILED,      // 舵机目标写入失败（F-SERVO-001）
    ACTION_TIMEOUT           // 动作总时长超 timing.fire_max_action_ms（F-SERVO-001）
};

inline const char* ToString(FireStepOutcome outcome) noexcept
{
    switch (outcome) {
    case FireStepOutcome::OK:                      return "OK";
    case FireStepOutcome::REJECTED_ESTOP:          return "REJECTED_ESTOP";
    case FireStepOutcome::REJECTED_FAULT:          return "REJECTED_FAULT";
    case FireStepOutcome::REJECTED_NOT_AUTHORIZED: return "REJECTED_NOT_AUTHORIZED";
    case FireStepOutcome::REJECTED_NOT_READY:      return "REJECTED_NOT_READY";
    case FireStepOutcome::ANGLE_REJECTED:          return "ANGLE_REJECTED";
    case FireStepOutcome::SERVO_WRITE_FAILED:      return "SERVO_WRITE_FAILED";
    case FireStepOutcome::ACTION_TIMEOUT:          return "ACTION_TIMEOUT";
    }
    return "UNKNOWN";
}

} // namespace marshland::atom11
