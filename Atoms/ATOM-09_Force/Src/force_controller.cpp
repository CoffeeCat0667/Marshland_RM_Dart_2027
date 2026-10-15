#include "atom09_force/force_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "MarshlandTypes.hpp"
#include "SbusTypes.hpp"

namespace marshland::atom09 {
namespace {

// CH12 -> 0-based 索引 11（Q-09 已确认 scaled_channels[11] 即 CH12 专用 rpm）。
constexpr std::size_t kForceChannelIndex = 11U;

std::int64_t MagnitudeOf(std::int32_t value) noexcept
{
    return value < 0 ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}

} // namespace

ForceController::ForceController(const marshland::config::IConfigView& config_view,
                                 const marshland::app::IMonotonicClock& clock,
                                 const marshland::sbus::ISbusSnapshotReader& sbus,
                                 marshland::app::IStateReader& state,
                                 IMotorCommandSink& motor,
                                 IForceFaultSink* fault_sink) noexcept
    : config_view_(config_view)
    , clock_(clock)
    , sbus_(sbus)
    , state_(state)
    , motor_(motor)
    , fault_sink_(fault_sink)
{
    ForceConfig loaded{};
    const ForceConfigLoadResult result = LoadForceConfig(config_view_, loaded);
    if (result.success) {
        config_ = loaded;
        config_ok_ = true;
        init_error_ = "";
        config_version_ = config_view_.configVersion();
    } else {
        init_error_ = result.error_message;
    }
}

bool ForceController::ready() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return config_ok_;
}

const char* ForceController::initializationError() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return init_error_;
}

bool ForceController::safetyAllows() noexcept
{
    const marshland::app::StateReadView view = state_.ReadState();
    if (view.estop_latched()) {
        return false;
    }
    return view.run_gate() == marshland::app::RunGateReason::ALLOWED;
}

bool ForceController::controlHolderIsSbus() noexcept
{
    const marshland::app::StateReadView view = state_.ReadState();
    return view.context().control_source == marshland::ControlSource::SBUS;
}

void ForceController::latchFault(const char* code, const char* detail) noexcept
{
    snapshot_.state = marshland::force::ForceActionState::FAULT;
    snapshot_.error_code = code;
    timed_active_ = false;
    if (fault_sink_ != nullptr) {
        fault_sink_->onForceActionFailed(detail);
    }
}

void ForceController::refreshConfigIfChanged() noexcept
{
    const std::uint64_t version = config_view_.configVersion();
    if (version == config_version_) {
        return;
    }
    ForceConfig loaded{};
    const ForceConfigLoadResult result = LoadForceConfig(config_view_, loaded);
    if (!result.success) {
        config_ok_ = false;
        init_error_ = result.error_message;
        config_version_ = version;
        if (config_.motor_id > 0) {
            (void)motor_.stopMotor(config_.motor_id);
        }
        latchFault("CONFIG_RELOAD", "force configuration became unusable after hot reload");
        return;
    }
    config_ = loaded;
    config_ok_ = true;
    init_error_ = "";
    config_version_ = version;
}

marshland::force::ForceSubmitResult ForceController::submitTarget(
    const marshland::force::ForceTarget& target,
    marshland::force::ForceMode mode) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!config_ok_) {
        snapshot_.state = marshland::force::ForceActionState::REJECTED;
        snapshot_.error_code = "CONFIG";
        return marshland::force::ForceSubmitResult::REJECTED_CONFIG;
    }
    if (snapshot_.state == marshland::force::ForceActionState::FAULT) {
        return marshland::force::ForceSubmitResult::REJECTED_SAFETY;
    }
    if (mode == marshland::force::ForceMode::TIMED && timed_active_) {
        return marshland::force::ForceSubmitResult::REJECTED_BUSY;
    }
    if (MagnitudeOf(target.target_rpm) > static_cast<std::int64_t>(config_.target_range_rpm)) {
        snapshot_.state = marshland::force::ForceActionState::REJECTED;
        snapshot_.error_code = "RANGE";
        return marshland::force::ForceSubmitResult::REJECTED_RANGE;
    }
    if (!safetyAllows()) {
        snapshot_.state = marshland::force::ForceActionState::REJECTED;
        snapshot_.error_code = "SAFETY";
        return marshland::force::ForceSubmitResult::REJECTED_SAFETY;
    }
    if (!motor_.submitVelocityTarget(config_.motor_id, target.target_rpm)) {
        latchFault("MOTOR", "force motor rejected the submitted velocity target");
        return marshland::force::ForceSubmitResult::MOTOR_COMMIT_FAILED;
    }

    const std::int64_t now_us = clock_.nowMicros();
    snapshot_.mode = mode;
    snapshot_.state = marshland::force::ForceActionState::RUNNING;
    snapshot_.target_rpm = target.target_rpm;
    snapshot_.source = target.source;
    snapshot_.deadline_us = 0;
    snapshot_.error_code = "";
    snapshot_.generation += 1U;

    if (mode == marshland::force::ForceMode::TIMED) {
        timed_active_ = true;
        timed_start_us_ = now_us;
        const std::uint64_t relative =
            static_cast<std::uint64_t>(config_.timed_action_ms) * 1000ULL;
        if (now_us > 0) {
            snapshot_.deadline_us = static_cast<std::uint64_t>(now_us) + relative;
        } else {
            snapshot_.deadline_us = relative;
        }
    } else {
        timed_active_ = false;
    }
    return marshland::force::ForceSubmitResult::ACCEPTED;
}

void ForceController::update(std::int64_t now_us) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);

    refreshConfigIfChanged();
    if (!config_ok_) {
        return;
    }
    if (snapshot_.state == marshland::force::ForceActionState::FAULT) {
        return;
    }

    marshland::force::ForceMotorFeedback feedback{};
    if (motor_.readFeedback(config_.motor_id, feedback)) {
        snapshot_.feedback = feedback;
    }
    if (motor_.faultLatched(config_.motor_id)) {
        latchFault("MOTOR", "force motor reported a latched motor/encoder/PID/CAN fault");
        return;
    }

    if (timed_active_) {
        if (now_us < timed_start_us_) {
            latchFault("CLOCK", "monotonic clock moved backwards during a timed force action");
            return;
        }
        if (now_us >= 0 && static_cast<std::uint64_t>(now_us) >= snapshot_.deadline_us) {
            snapshot_.state = marshland::force::ForceActionState::COMPLETED;
            snapshot_.error_code = "";
            snapshot_.deadline_us = 0;
            timed_active_ = false;
        }
        // TIMED 动作组内不消费 CH12：控制权接管由 ATOM-02/13 在当前动作组完成后处理。
        return;
    }

    if (!controlHolderIsSbus()) {
        // WEB / AUTO 保持各自提交的目标，直到新目标或安全事件。
        return;
    }
    applySbusManual();
}

void ForceController::applySbusManual() noexcept
{
    const marshland::sbus::SbusChannelSnapshot snapshot = sbus_.readSbusSnapshot();
    if (!snapshot.online ||
        snapshot.frame_validity != marshland::sbus::FrameValidity::VALID) {
        return;
    }

    const std::int32_t rpm = snapshot.scaled_channels[kForceChannelIndex];
    const std::int64_t magnitude = MagnitudeOf(rpm);
    if (magnitude > static_cast<std::int64_t>(marshland::sbus::kForceSpeedLimitRpm)) {
        latchFault("SBUS_RANGE", "SBUS CH12 scaled value exceeds the force speed window");
        return;
    }

    if (magnitude <= static_cast<std::int64_t>(config_.center_deadband_rpm)) {
        // Q-13：CH12 中位（1024）保持电机**零力矩**，不用 0 rpm 闭环代替。
        if (snapshot_.state != marshland::force::ForceActionState::IDLE) {
            (void)motor_.stopMotor(config_.motor_id);
            snapshot_.state = marshland::force::ForceActionState::IDLE;
            snapshot_.target_rpm = 0;
            snapshot_.source = marshland::ControlSource::SBUS;
            snapshot_.deadline_us = 0;
            snapshot_.error_code = "";
            snapshot_.generation += 1U;
        }
        return;
    }

    if (snapshot_.state == marshland::force::ForceActionState::RUNNING &&
        snapshot_.mode == marshland::force::ForceMode::MANUAL &&
        snapshot_.target_rpm == rpm) {
        return;   // 目标未变化，不重复提交
    }
    if (!safetyAllows()) {
        return;
    }
    if (!motor_.submitVelocityTarget(config_.motor_id, rpm)) {
        latchFault("MOTOR", "force motor rejected the SBUS CH12 velocity target");
        return;
    }

    snapshot_.mode = marshland::force::ForceMode::MANUAL;
    snapshot_.state = marshland::force::ForceActionState::RUNNING;
    snapshot_.target_rpm = rpm;
    snapshot_.source = marshland::ControlSource::SBUS;
    snapshot_.deadline_us = 0;
    snapshot_.error_code = "";
    snapshot_.generation += 1U;
}

void ForceController::stop() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    timed_active_ = false;
    if (config_.motor_id > 0) {
        (void)motor_.stopMotor(config_.motor_id);
    }
    snapshot_.state = marshland::force::ForceActionState::IDLE;
    snapshot_.target_rpm = 0;
    snapshot_.deadline_us = 0;
    snapshot_.error_code = "";
}

marshland::force::ForceStatus ForceController::status() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

} // namespace marshland::atom09
