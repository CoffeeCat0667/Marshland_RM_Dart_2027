// ============================================================================
// charge_controller.cpp — ATOM-08 纯逻辑内核实现
//
// 安全门控（Rule/AtomIntegration.md §4.3 / §4.3.1）：
//   * 普通蓄力/停止 = IntentKind::CONTROL；回零 = IntentKind::SAFETY_RECOVERY。
//   * 急停锁定阶段（SystemMode == SOFTWARE_ESTOP，而非"锁存位"——人工确认后的
//     恢复阶段锁存位仍可能为真）与 FAULT 阶段：本模块自身拒绝普通命令。
//   * 不把安全寄托于调用方查询 RunGate()。
//   * StateReadView 为持锁租约：取快照后立即销毁，绝不在租约存活期间调用其他端口
//     （同线程 std::mutex 自锁）。
// ============================================================================
#include "atom08_charge/charge_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace marshland::atom08 {
namespace {

using marshland::ControlSource;
using marshland::FaultSeverity;
using marshland::SystemMode;
using marshland::state_arbitration::IntentKind;
using marshland::state_arbitration::RejectReason;

FaultSeverity SeverityFor(ChargeFaultCode code) noexcept
{
    switch (code) {
    case ChargeFaultCode::None:
        return FaultSeverity::INFO;
    case ChargeFaultCode::LimitInputInvalid:
        return FaultSeverity::SEVERE;
    case ChargeFaultCode::ChargeTimeout:
    case ChargeFaultCode::HomeTimeout:
    case ChargeFaultCode::MotorCommandFailed:
    case ChargeFaultCode::ConfigReloadFailed:
        return FaultSeverity::SEVERE;
    }
    return FaultSeverity::SEVERE;
}

std::int64_t Magnitude(std::int32_t value) noexcept
{
    return value < 0 ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}

} // namespace

ChargeController::ChargeController(const marshland::config::IConfigView& config,
                                   const marshland::app::IMonotonicClock& clock,
                                   IMotorCommandSink& motors,
                                   const ILimitSwitchState& limits,
                                   marshland::app::IControlAuthority& authority,
                                   marshland::app::IStateReader& state_reader,
                                   marshland::sbus::ISbusSnapshotReader& sbus,
                                   IFaultReporter& faults) noexcept
    : config_view_(config)
    , clock_(clock)
    , motors_(motors)
    , limits_(limits)
    , authority_(authority)
    , state_reader_(state_reader)
    , sbus_(sbus)
    , faults_(faults)
{
}

bool ChargeController::configure(const ChargeConfig& config) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (config.left_motor_id == 0U || config.right_motor_id == 0U ||
        config.left_motor_id == config.right_motor_id) {
        return false;
    }
    if (config.charge_time_us <= 0 || config.charge_max_action_us <= 0 ||
        config.home_max_action_us <= 0) {
        return false;
    }
    if (config.target_range_rpm < 0) {
        return false;
    }
    config_ = config;
    config_valid_ = true;
    config_version_ = config_view_.configVersion();
    state_ = ChargeState::Idle;
    action_ = ChargeAction::None;
    charge_completed_ = false;
    home_completed_ = false;
    limit_invalid_reported_ = false;
    left_target_rpm_ = 0;
    right_target_rpm_ = 0;
    return true;
}

void ChargeController::reset() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_valid_) {
        clearBothMotorsLocked();
    }
    config_valid_ = false;
    state_ = ChargeState::Uninitialized;
    action_ = ChargeAction::None;
    charge_completed_ = false;
    home_completed_ = false;
    limit_invalid_reported_ = false;
    action_start_us_ = 0;
}

ChargeController::ArbitrationSnapshot ChargeController::readArbitration() noexcept
{
    ArbitrationSnapshot snapshot{};
    // 该 view 持有 AppCoordinator 的互斥锁；离开作用域即释放，
    // 之后才允许调用 SubmitIntent 等其他端口。
    auto view = state_reader_.ReadState();
    snapshot.mode = view.context().system_mode;
    snapshot.control_source = view.context().control_source;
    snapshot.fault_active = view.inputs().fault_active;
    snapshot.hardware_ready = view.hardware_ready();
    return snapshot;
}

bool ChargeController::reloadConfigLocked() noexcept
{
    const std::uint64_t version = config_view_.configVersion();
    if (version == config_version_) {
        return false;
    }
    // 无论成败都记录版本，避免同一版本每周期重复上报
    config_version_ = version;

    ChargeConfig next{};
    const ChargeConfigLoadResult loaded = LoadChargeConfig(config_view_, next);
    if (loaded.success) {
        config_ = next;
        return false;
    }
    // 热加载失败：保留上一份有效配置，仅登记并上报
    last_fault_ = ChargeFaultCode::ConfigReloadFailed;
    return true;
}

bool ChargeController::isModeAllowed(SystemMode mode, IntentKind kind) noexcept
{
    switch (mode) {
    case SystemMode::MANUAL:
    case SystemMode::DEBUG:
        return true;
    case SystemMode::AUTO_FIRE:
        return kind == IntentKind::CONTROL;
    case SystemMode::RECOVERY:
        return kind == IntentKind::SAFETY_RECOVERY;
    default:
        return false;
    }
}

ChargeRequestResult ChargeController::mapRejectReason(RejectReason reason) noexcept
{
    switch (reason) {
    case RejectReason::SOFTWARE_ESTOP:
    case RejectReason::ESTOP_NOT_CONFIRMED:
        return ChargeRequestResult::RejectedEstopLocked;
    case RejectReason::FAULT_ACTIVE:
        return ChargeRequestResult::RejectedFaultActive;
    case RejectReason::NO_CONTROL_AUTHORITY:
    case RejectReason::LOWER_PRIORITY_CONFLICT:
        return ChargeRequestResult::RejectedNoControlAuthority;
    case RejectReason::NOT_READY:
        return ChargeRequestResult::RejectedHardwareNotReady;
    default:
        return ChargeRequestResult::RejectedArbitration;
    }
}

bool ChargeController::submitIntentLocked(IntentKind kind, ControlSource source,
                                          std::uint32_t request_id,
                                          std::uint64_t timestamp_us) noexcept
{
    marshland::state_arbitration::ControlIntent intent{};
    intent.valid = true;
    intent.source = source;
    intent.kind = kind;
    intent.request_id = request_id;
    intent.timestamp_us = timestamp_us;

    const marshland::state_arbitration::IntentDecision decision = authority_.SubmitIntent(intent);
    if (decision.accepted) {
        return true;
    }
    last_result_ = mapRejectReason(decision.reason);
    return false;
}

bool ChargeController::submitBothMotorsLocked(std::int32_t left_rpm, std::int32_t right_rpm) noexcept
{
    // 两台命令分别提交，不承诺原子回滚：任一失败即联动撤销另一侧
    if (!motors_.submitVelocityTarget(config_.left_motor_id, left_rpm)) {
        (void)motors_.clearVelocityTarget(config_.right_motor_id);
        (void)motors_.clearVelocityTarget(config_.left_motor_id);
        return false;
    }
    if (!motors_.submitVelocityTarget(config_.right_motor_id, right_rpm)) {
        (void)motors_.clearVelocityTarget(config_.left_motor_id);
        (void)motors_.clearVelocityTarget(config_.right_motor_id);
        return false;
    }
    return true;
}

void ChargeController::clearBothMotorsLocked() noexcept
{
    if (config_.left_motor_id != 0U) {
        (void)motors_.clearVelocityTarget(config_.left_motor_id);
    }
    if (config_.right_motor_id != 0U) {
        (void)motors_.clearVelocityTarget(config_.right_motor_id);
    }
    left_target_rpm_ = 0;
    right_target_rpm_ = 0;
}

void ChargeController::enterFaultLocked(ChargeFaultCode code, std::int64_t now_us,
                                        ChargeFaultCode& report) noexcept
{
    clearBothMotorsLocked();
    state_ = ChargeState::Fault;
    action_ = ChargeAction::None;
    last_fault_ = code;
    last_fault_time_us_ = now_us > 0 ? static_cast<std::uint64_t>(now_us) : 0U;
    report = code;
}

bool ChargeController::targetsWithinRange(std::int32_t left_rpm, std::int32_t right_rpm) const noexcept
{
    if (config_.target_range_rpm <= 0) {
        return true;
    }
    const std::int64_t limit = static_cast<std::int64_t>(config_.target_range_rpm);
    return Magnitude(left_rpm) <= limit && Magnitude(right_rpm) <= limit;
}

bool ChargeController::resolveChargeTargetsLocked(const ChargeStartRequest& request,
                                                  ControlSource effective_source,
                                                  std::int32_t& left_rpm,
                                                  std::int32_t& right_rpm) const noexcept
{
    // effective_source 由调用方在 Public 内确定（requester 优先，NONE 时回落当前控制源），
    // 且同一值会用于 SubmitIntent，避免"解析用 A、提交用 B"的不一致。
    if (effective_source == ControlSource::NONE) {
        return false;
    }
    if (request.source == ChargeTargetSource::Explicit) {
        left_rpm = request.left_rpm;
        right_rpm = request.right_rpm;
        return true;
    }
    if (effective_source != ControlSource::SBUS) {
        // WEB / AUTO：无逐电机下发时使用配置恒定速度（两电机同值）
        left_rpm = config_.charge_speed_rpm;
        right_rpm = config_.charge_speed_rpm;
        return true;
    }

    const marshland::sbus::SbusChannelSnapshot channels = sbus_.readSbusSnapshot();
    if (!channels.online || channels.frame_validity != marshland::sbus::FrameValidity::VALID) {
        return false;
    }
    const auto channelValue = [&channels](std::int32_t index, std::int32_t& out) noexcept -> bool {
        if (index < 0 || static_cast<std::size_t>(index) >= marshland::sbus::kChannelCount) {
            return false;
        }
        out = channels.scaled_channels[static_cast<std::size_t>(index)];
        return true;
    };

    std::int32_t both = 0;
    std::int32_t left = 0;
    std::int32_t right = 0;
    const bool has_both = channelValue(config_.sbus_manual_charge_channel, both);
    const bool has_left = channelValue(config_.sbus_manual_charge_left_channel, left);
    const bool has_right = channelValue(config_.sbus_manual_charge_right_channel, right);

    // R1 裁决：整体通道非零时优先级更高，覆盖左右通道
    if (has_both && both != 0) {
        left_rpm = both;
        right_rpm = both;
        return true;
    }
    if (has_left && has_right) {
        left_rpm = left;
        right_rpm = right;
        return true;
    }
    return false;
}

ChargeRequestResult ChargeController::requestCharge(const ChargeStartRequest& request) noexcept
{
    ChargeFaultCode report = ChargeFaultCode::None;
    ChargeRequestResult result = ChargeRequestResult::RejectedNotInitialized;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!config_valid_) {
            result = ChargeRequestResult::RejectedNotInitialized;
        } else if (state_ == ChargeState::Charging || state_ == ChargeState::Returning) {
            result = ChargeRequestResult::RejectedBusy;
        } else {
            if (reloadConfigLocked()) {
                report = ChargeFaultCode::ConfigReloadFailed;
            }
            const ArbitrationSnapshot snapshot = readArbitration();
            const ControlSource effective_source =
                request.requester != ControlSource::NONE
                    ? request.requester
                    : snapshot.control_source;
            if (snapshot.mode == SystemMode::SOFTWARE_ESTOP) {
                result = ChargeRequestResult::RejectedEstopLocked;
            } else if (snapshot.fault_active || snapshot.mode == SystemMode::FAULT) {
                result = ChargeRequestResult::RejectedFaultActive;
            } else if (!snapshot.hardware_ready) {
                result = ChargeRequestResult::RejectedHardwareNotReady;
            } else if (effective_source == ControlSource::NONE) {
                result = ChargeRequestResult::RejectedNoControlAuthority;
            } else if (!isModeAllowed(snapshot.mode, IntentKind::CONTROL)) {
                result = ChargeRequestResult::RejectedMode;
            } else {
                std::int32_t left = 0;
                std::int32_t right = 0;
                // 全零目标不得当作"成功蓄力"：否则会空转 X 秒后误报完成
                if (!resolveChargeTargetsLocked(request, effective_source, left, right) ||
                    (left == 0 && right == 0)) {
                    result = ChargeRequestResult::RejectedTargetUnavailable;
                } else if (!targetsWithinRange(left, right)) {
                    result = ChargeRequestResult::RejectedTargetOutOfRange;
                } else {
                    const std::int64_t now_us = request.timestamp_us != 0U
                        ? static_cast<std::int64_t>(request.timestamp_us)
                        : clock_.nowMicros();
                    if (!submitIntentLocked(IntentKind::CONTROL, effective_source,
                                            request.request_id,
                                            static_cast<std::uint64_t>(now_us))) {
                        result = last_result_;
                    } else if (!submitBothMotorsLocked(left, right)) {
                        enterFaultLocked(ChargeFaultCode::MotorCommandFailed, now_us, report);
                        result = ChargeRequestResult::RejectedMotorCommand;
                    } else {
                        left_target_rpm_ = left;
                        right_target_rpm_ = right;
                        action_ = ChargeAction::Charge;
                        action_start_us_ = now_us;
                        state_ = ChargeState::Charging;
                        charge_completed_ = false;
                        limit_invalid_reported_ = false;
                        result = ChargeRequestResult::Accepted;
                    }
                }
            }
        }
        last_result_ = result;
    }

    if (report != ChargeFaultCode::None) {
        faults_.reportChargeFault(report, SeverityFor(report));
    }
    return result;
}

ChargeRequestResult ChargeController::requestHome(const ChargeHomeRequest& request) noexcept
{
    ChargeFaultCode report = ChargeFaultCode::None;
    ChargeRequestResult result = ChargeRequestResult::RejectedNotInitialized;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!config_valid_) {
            result = ChargeRequestResult::RejectedNotInitialized;
        } else if (state_ == ChargeState::Charging || state_ == ChargeState::Returning) {
            result = ChargeRequestResult::RejectedBusy;
        } else {
            if (reloadConfigLocked()) {
                report = ChargeFaultCode::ConfigReloadFailed;
            }
            const ArbitrationSnapshot snapshot = readArbitration();
            const ControlSource effective_source =
                request.requester != ControlSource::NONE
                    ? request.requester
                    : snapshot.control_source;
            if (snapshot.mode == SystemMode::SOFTWARE_ESTOP) {
                result = ChargeRequestResult::RejectedEstopLocked;
            } else if (!snapshot.hardware_ready) {
                result = ChargeRequestResult::RejectedHardwareNotReady;
            } else if (effective_source == ControlSource::NONE) {
                result = ChargeRequestResult::RejectedNoControlAuthority;
            } else if (!isModeAllowed(snapshot.mode, IntentKind::SAFETY_RECOVERY)) {
                result = ChargeRequestResult::RejectedMode;
            } else {
                // 是否处于允许恢复的阶段与机构由仲裁/ATOM-14 决定，本模块不代替其授权
                std::int32_t left = 0;
                std::int32_t right = 0;
                if (request.source == ChargeTargetSource::Explicit) {
                    left = request.left_rpm;
                    right = request.right_rpm;
                } else {
                    left = config_.home_speed_rpm;
                    right = config_.home_speed_rpm;
                }
                if (left == 0 && right == 0) {
                    result = ChargeRequestResult::RejectedTargetUnavailable;
                } else if (!targetsWithinRange(left, right)) {
                    result = ChargeRequestResult::RejectedTargetOutOfRange;
                } else {
                    const std::int64_t now_us = request.timestamp_us != 0U
                        ? static_cast<std::int64_t>(request.timestamp_us)
                        : clock_.nowMicros();
                    if (!submitIntentLocked(IntentKind::SAFETY_RECOVERY, effective_source,
                                            request.request_id,
                                            static_cast<std::uint64_t>(now_us))) {
                        result = last_result_;
                    } else if (!submitBothMotorsLocked(left, right)) {
                        enterFaultLocked(ChargeFaultCode::MotorCommandFailed, now_us, report);
                        result = ChargeRequestResult::RejectedMotorCommand;
                    } else {
                        left_target_rpm_ = left;
                        right_target_rpm_ = right;
                        action_ = ChargeAction::Home;
                        action_start_us_ = now_us;
                        state_ = ChargeState::Returning;
                        home_completed_ = false;
                        limit_invalid_reported_ = false;
                        result = ChargeRequestResult::Accepted;
                    }
                }
            }
        }
        last_result_ = result;
    }

    if (report != ChargeFaultCode::None) {
        faults_.reportChargeFault(report, SeverityFor(report));
    }
    return result;
}

ChargeRequestResult ChargeController::requestStopCharge() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_valid_) {
        last_result_ = ChargeRequestResult::RejectedNotInitialized;
        return ChargeRequestResult::RejectedNotInitialized;
    }

    // 普通停止只撤销正常蓄力目标，不提交仲裁意图、也不声称回零完成：
    // 撤销出力不属于需要授权的动作，急停/故障期间同样必须允许。
    clearBothMotorsLocked();
    action_ = ChargeAction::None;
    switch (state_) {
    case ChargeState::Charging:
    case ChargeState::ChargeCompleted:
    case ChargeState::Returning:
    case ChargeState::ReturnCompleted:
        state_ = ChargeState::Stopped;
        break;
    default:
        break;
    }
    last_result_ = ChargeRequestResult::Accepted;
    return ChargeRequestResult::Accepted;
}

void ChargeController::tick(std::int64_t now_us) noexcept
{
    ChargeFaultCode report = ChargeFaultCode::None;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        report = doTickLocked(now_us);
    }
    if (report != ChargeFaultCode::None) {
        faults_.reportChargeFault(report, SeverityFor(report));
    }
}

ChargeFaultCode ChargeController::doTickLocked(std::int64_t now_us) noexcept
{
    if (!config_valid_) {
        return ChargeFaultCode::None;
    }
    if (reloadConfigLocked()) {
        return ChargeFaultCode::ConfigReloadFailed;
    }
    if (state_ != ChargeState::Charging && state_ != ChargeState::Returning) {
        return ChargeFaultCode::None;
    }

    const ArbitrationSnapshot snapshot = readArbitration();
    // 急停锁定、FAULT 阶段，或"故障已激活且不处于恢复阶段"时撤销出力；
    // 恢复阶段（RECOVERY）不撤销，否则会打断 ATOM-14 的蓄力回零。
    const bool must_revoke = snapshot.mode == SystemMode::SOFTWARE_ESTOP ||
        snapshot.mode == SystemMode::FAULT ||
        (snapshot.fault_active && snapshot.mode != SystemMode::RECOVERY);
    if (must_revoke) {
        // 撤销出力；急停与故障的编排归 ATOM-14，本模块不重复上报
        clearBothMotorsLocked();
        action_ = ChargeAction::None;
        state_ = ChargeState::Stopped;
        last_result_ = snapshot.mode == SystemMode::SOFTWARE_ESTOP
            ? ChargeRequestResult::RejectedEstopLocked
            : ChargeRequestResult::RejectedFaultActive;
        return ChargeFaultCode::None;
    }

    const std::int64_t elapsed_us = now_us - action_start_us_;

    if (state_ == ChargeState::Charging) {
        // 完成判定优先于超时（X 与最大动作时间数值相同）
        if (elapsed_us >= config_.charge_time_us) {
            clearBothMotorsLocked();
            state_ = ChargeState::ChargeCompleted;
            charge_completed_ = true;
            last_result_ = ChargeRequestResult::Accepted;
            return ChargeFaultCode::None;
        }
        if (elapsed_us >= config_.charge_max_action_us) {
            ChargeFaultCode report = ChargeFaultCode::None;
            enterFaultLocked(ChargeFaultCode::ChargeTimeout, now_us, report);
            return report;
        }
        return ChargeFaultCode::None;
    }

    // 回零阶段：仅当两侧限位均"有效且稳定为低"才停止回零并标记完成
    LimitSwitchSample left{};
    LimitSwitchSample right{};
    const bool left_ok = limits_.readChargeLimit(config_.left_motor_id, left);
    const bool right_ok = limits_.readChargeLimit(config_.right_motor_id, right);
    const bool inputs_invalid = !left_ok || !right_ok || !left.valid || !right.valid;
    if (inputs_invalid) {
        if (!limit_invalid_reported_) {
            limit_invalid_reported_ = true;
            return ChargeFaultCode::LimitInputInvalid;
        }
    } else if (left.low && right.low) {
        clearBothMotorsLocked();
        state_ = ChargeState::ReturnCompleted;
        home_completed_ = true;
        last_result_ = ChargeRequestResult::Accepted;
        return ChargeFaultCode::None;
    }

    if (elapsed_us >= config_.home_max_action_us) {
        ChargeFaultCode report = ChargeFaultCode::None;
        enterFaultLocked(ChargeFaultCode::HomeTimeout, now_us, report);
        return report;
    }
    return ChargeFaultCode::None;
}

ChargeStatusSnapshot ChargeController::status() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ChargeStatusSnapshot snapshot{};
    snapshot.state = state_;
    snapshot.last_action = action_;
    snapshot.charge_completed = charge_completed_;
    snapshot.home_completed = home_completed_;
    snapshot.left_target_rpm = left_target_rpm_;
    snapshot.right_target_rpm = right_target_rpm_;
    snapshot.last_fault = last_fault_;
    snapshot.last_result = last_result_;
    snapshot.last_fault_time_us = last_fault_time_us_;
    return snapshot;
}

bool ChargeController::initialized() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return config_valid_;
}

} // namespace marshland::atom08