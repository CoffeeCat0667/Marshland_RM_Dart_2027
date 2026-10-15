// ============================================================================
// fire_action.cpp — ATOM-11 一次发射动作状态机实现
//
// 状态机（Design/Atomic/ATOM-11_Fire.md 原子步骤 + PLAN.md §6.2）：
//   IDLE -> 校验 -> 写发射角 -> HOLDING -> 保持到期写回待机角 -> COMPLETED
//   任何普通阶段失败 -> FAILED（不写新目标、不发完成事件）
// 完成判据：待机角目标写回成功；一次动作恰好一次完成事件。
// ============================================================================
#include "atom11_fire/fire_action.hpp"

namespace marshland::atom11 {
namespace {

constexpr std::int64_t kAngleMin = 0;
constexpr std::int64_t kAngleMax = 180;

bool angleInRange(int deg) noexcept
{
    return deg >= kAngleMin && deg <= kAngleMax;
}

constexpr std::uint64_t kMsToUs = 1000ULL;

} // namespace

bool FireActionCore::configure(const FireConfig& config) noexcept
{
    if (!angleInRange(config.standby_deg) || !angleInRange(config.fire_deg) ||
        config.hold_ms <= 0 || config.max_action_ms <= 0) {
        configured_ = false;
        return false;
    }
    config_ = config;
    hold_us_ = static_cast<std::uint64_t>(config.hold_ms) * kMsToUs;
    max_action_us_ = static_cast<std::uint64_t>(config.max_action_ms) * kMsToUs;
    configured_ = true;
    state_ = FireActionState::IDLE;
    completion_emitted_ = false;
    return true;
}

FireStepOutcome FireActionCore::failWith(FireStepOutcome outcome,
                                         const char* fault_code,
                                         std::uint64_t now_us) noexcept
{
    state_ = FireActionState::FAILED;
    if (fault_code != nullptr && events_ != nullptr) {
        events_->onFireFault(fault_code, now_us);
    }
    return outcome;
}

void FireActionCore::emitCompleted(std::uint64_t now_us) noexcept
{
    if (events_ != nullptr && !completion_emitted_) {
        completion_emitted_ = true;
        events_->onFireCompleted(now_us);
    }
}

FireStepOutcome FireActionCore::begin(const FireFacts& facts,
                                      std::uint64_t now_us) noexcept
{
    if (!configured_ || state_ == FireActionState::MOVING_TO_FIRE ||
        state_ == FireActionState::HOLDING || state_ == FireActionState::RETURNING) {
        return FireStepOutcome::REJECTED_NOT_READY;
    }

    // 校验失败不写舵机、不发完成事件（急停期间拒绝任何发射舵机命令）。
    if (facts.software_estop) {
        return failWith(FireStepOutcome::REJECTED_ESTOP, nullptr, now_us);
    }
    if (facts.fault_active) {
        return failWith(FireStepOutcome::REJECTED_FAULT, nullptr, now_us);
    }
    if (!facts.authorization.allowed) {
        return failWith(FireStepOutcome::REJECTED_NOT_AUTHORIZED, nullptr, now_us);
    }
    if (!angleInRange(config_.fire_deg)) {
        return failWith(FireStepOutcome::ANGLE_REJECTED, kFaultAngleRejected, now_us);
    }

    state_ = FireActionState::MOVING_TO_FIRE;
    if (!writer_.writeFireTarget(static_cast<double>(config_.fire_deg))) {
        return failWith(FireStepOutcome::SERVO_WRITE_FAILED, kFaultServoFailure,
                        now_us);
    }

    start_us_ = now_us;
    completion_emitted_ = false;
    state_ = FireActionState::HOLDING;
    return FireStepOutcome::OK;
}

FireStepOutcome FireActionCore::tick(const FireFacts& facts,
                                     std::uint64_t now_us) noexcept
{
    if (state_ != FireActionState::HOLDING && state_ != FireActionState::RETURNING) {
        return FireStepOutcome::OK;
    }

    // 动作进行中复检安全事实：急停 / 故障出现时不写任何新目标。
    if (facts.software_estop) {
        return failWith(FireStepOutcome::REJECTED_ESTOP, nullptr, now_us);
    }
    if (facts.fault_active) {
        return failWith(FireStepOutcome::REJECTED_FAULT, nullptr, now_us);
    }

    const std::uint64_t elapsed_us = now_us - start_us_;
    if (elapsed_us > max_action_us_) {
        return failWith(FireStepOutcome::ACTION_TIMEOUT, kFaultServoFailure, now_us);
    }

    if (state_ == FireActionState::HOLDING && elapsed_us >= hold_us_) {
        state_ = FireActionState::RETURNING;
        if (!writer_.writeFireTarget(static_cast<double>(config_.standby_deg))) {
            return failWith(FireStepOutcome::SERVO_WRITE_FAILED, kFaultServoFailure,
                            now_us);
        }
        state_ = FireActionState::COMPLETED;
        emitCompleted(now_us);
    }
    return FireStepOutcome::OK;
}

} // namespace marshland::atom11
