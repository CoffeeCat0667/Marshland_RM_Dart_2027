#include "atom05_motor_pid/motor_core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "atom05_motor_pid/feedback_decoder.hpp"

namespace marshland::atom05 {
namespace {

bool validPermit(const MotorPermit& permit) noexcept
{
    return permit.allowed && permit.output_limit_ma > 0 &&
           permit.output_limit_ma <= kActuatorLimitMa;
}

bool validSource(ControlSource source) noexcept
{
    return source == ControlSource::WEB || source == ControlSource::SBUS ||
           source == ControlSource::AUTO;
}

} // namespace

MotorCore::MotorCore(const Dependencies& dependencies) noexcept : dependencies_(dependencies) {}
MotorCore::~MotorCore() noexcept { stop(); }

bool MotorCore::now(std::uint64_t& output) const noexcept
{
    const auto value = dependencies_.clock.nowMicros();
    if (value < 0) { return false; }
    output = static_cast<std::uint64_t>(value);
    return true;
}

std::size_t MotorCore::findLocked(std::uint8_t motor_id) const noexcept
{
    for (std::size_t i = 0; i < kMotorCount; ++i) {
        if (channels_[i].state.found && channels_[i].state.motor_id == motor_id) { return i; }
    }
    return kNotFound;
}

std::size_t MotorCore::peerLocked(std::size_t index) const noexcept
{
    if (index >= kMotorCount) { return kNotFound; }
    const auto id = channels_[index].state.motor_id;
    return id == 3U ? findLocked(4) : id == 4U ? findLocked(3) : kNotFound;
}

MotorStatus MotorCore::configure(const MotorRuntimeConfig& config) noexcept
{
    if (!ValidateMotorRuntimeConfig(config)) { return MotorStatus::INVALID_CONFIG; }
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    std::lock_guard update_lock(pid_update_mutex_);
    std::lock_guard lock(mutex_);
    if (running_.load() || worker_.joinable()) { return MotorStatus::BUSY; }
    for (const auto& channel : channels_) {
        if (channel.notification_pending || channel.notification_in_flight) { return MotorStatus::BUSY; }
    }
    config_ = config;
    channels_ = {};
    for (std::size_t i = 0; i < kMotorCount; ++i) {
        auto& state = channels_[i].state;
        state.found = true;
        state.motor_id = config.motors[i].motor_id;
        state.node_id = config.motors[i].node_id;
        state.pid = config.motors[i].pid;
        state.command.motor_id = state.motor_id;
    }
    configured_ = true;
    stopped_ = true;
    return MotorStatus::OK;
}

std::uint64_t MotorCore::configurationVersion() const noexcept
{
    std::lock_guard lock(mutex_);
    return config_.config_version;
}

MotorStatus MotorCore::start() noexcept
{
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    MotorStatus result = MotorStatus::OK;
    {
        std::lock_guard lock(mutex_);
        if (running_.load()) { return MotorStatus::BUSY; }
        if (!configured_ || worker_.joinable() || !dependencies_.output.ready()) {
            return MotorStatus::NOT_READY;
        }
        std::uint64_t now_us = 0;
        if (!now(now_us)) { return MotorStatus::CLOCK_INVALID; }
        stopped_ = false;
        for (std::size_t i = 0; i < kMotorCount; ++i) { revokeLocked(i); }
        if (!commitLocked(now_us)) {
            result = MotorStatus::OUTPUT_FAILED;
        } else {
            running_.store(true, std::memory_order_release);
            try {
                worker_ = std::thread(&MotorCore::run, this);
            } catch (...) {
                running_.store(false, std::memory_order_release);
                result = MotorStatus::NOT_READY;
            }
        }
    }
    lifecycle_lock.unlock();
    publishFaults();
    return result;
}

void MotorCore::stop() noexcept
{
    std::unique_lock lifecycle_lock(lifecycle_mutex_);
    running_.store(false, std::memory_order_release);
    wake();
    {
        std::lock_guard lock(mutex_);
        if (configured_ && !stopped_) {
            for (std::size_t i = 0; i < kMotorCount; ++i) { revokeLocked(i); }
            std::uint64_t now_us = 0;
            (void)now(now_us);
            (void)commitLocked(now_us);
            stopped_ = true;
        }
    }
    if (worker_.joinable()) { worker_.join(); }
    lifecycle_lock.unlock();
    publishFaults();
}

MotorStatus MotorCore::feedbackStatusLocked(const Channel& channel,
                                           std::uint64_t now_us) const noexcept
{
    const auto& feedback = channel.state.feedback;
    if (!feedback.valid || !feedback.has_received ||
        channel.pending_feedback_fault != MotorFault::NONE) { return MotorStatus::FEEDBACK_INVALID; }
    if (feedback.last_valid_time_us > now_us) { return MotorStatus::CLOCK_INVALID; }
    const auto timeout = static_cast<std::uint64_t>(config_.feedback_timeout_ms) * 1000U;
    return now_us - feedback.last_valid_time_us >= timeout ?
           MotorStatus::FEEDBACK_TIMEOUT : MotorStatus::OK;
}

void MotorCore::onCanFrame(const can::CanFrame& frame) noexcept
{
    bool notify = false;
    {
        std::lock_guard lock(mutex_);
        if (!configured_) { return; }
        for (auto& channel : channels_) {
            if (channel.state.node_id != frame.can_id) { continue; }
            auto& previous = channel.state.feedback;
            if (previous.has_received && frame.timestamp_us <= previous.last_valid_time_us) { return; }
            MotorFeedback decoded{};
            auto fault = DecodeMotorFeedback(frame, decoded);
            std::uint64_t now_us = 0;
            if (!now(now_us) || frame.timestamp_us > now_us) { fault = MotorFault::CLOCK_OR_DEADLINE; }
            if (fault != MotorFault::NONE) {
                previous.valid = false;
                channel.pending_feedback_fault = fault;
                notify = true;
            } else {
                decoded.sequence = previous.sequence == std::numeric_limits<std::uint64_t>::max() ?
                                   previous.sequence : previous.sequence + 1U;
                previous = decoded;
            }
            break;
        }
    }
    // The RX callback only updates local data; it never calls ATOM-04/output.
    if (notify) { wake(); }
}

MotorSnapshot MotorCore::snapshot(std::uint8_t motor_id) const noexcept
{
    std::lock_guard lock(mutex_);
    const auto index = findLocked(motor_id);
    if (index == kNotFound) { return {}; }
    auto result = channels_[index].state;
    std::uint64_t now_us = 0;
    result.feedback.valid = now(now_us) &&
        feedbackStatusLocked(channels_[index], now_us) == MotorStatus::OK;
    return result;
}

void MotorCore::revokeLocked(std::size_t index) noexcept
{
    auto& channel = channels_[index];
    channel.state.enabled = false;
    channel.state.target_active = false;
    channel.state.output_ma = 0.0;
    channel.state.command = {};
    channel.state.command.motor_id = channel.state.motor_id;
    channel.state.saturation_cycles = 0;
    channel.controller.reset();
    channel.has_step = false;
    channel.last_step_us = 0;
    channel.next_due_us = 0;
}

void MotorCore::revokePairLocked(std::size_t index) noexcept
{
    revokeLocked(index);
    const auto peer = peerLocked(index);
    if (peer != kNotFound) { revokeLocked(peer); }
}

void MotorCore::faultLocked(std::size_t index, MotorFault fault, std::uint64_t now_us) noexcept
{
    const auto peer = peerLocked(index);
    for (const auto affected : std::array<std::size_t, 2>{index, peer}) {
        if (affected == kNotFound) { continue; }
        auto& channel = channels_[affected];
        const bool first = channel.state.fault == MotorFault::NONE;
        revokeLocked(affected);
        if (first) {
            channel.state.fault = affected == index ? fault : MotorFault::RELOAD_PEER;
            channel.event = {channel.state.motor_id,
                affected == index && peer != kNotFound ? channels_[peer].state.motor_id : std::uint8_t{0},
                channel.state.fault, now_us, false};
            channel.notification_pending = true;
        }
        channel.pending_feedback_fault = MotorFault::NONE;
    }
}

bool MotorCore::commitLocked(std::uint64_t now_us) noexcept
{
    // Revalidate all active channels before publishing a batch, including those
    // not stepped by updateOnce(). The output bridge must enforce safety too.
    for (std::size_t i = 0; i < kMotorCount; ++i) {
        auto& channel = channels_[i];
        if (!channel.state.enabled || !channel.state.target_active) { continue; }
        const auto feedback = feedbackStatusLocked(channel, now_us);
        if (feedback != MotorStatus::OK) {
            const auto fault = channel.pending_feedback_fault != MotorFault::NONE ?
                channel.pending_feedback_fault : feedback == MotorStatus::FEEDBACK_TIMEOUT ?
                MotorFault::FEEDBACK_TIMEOUT : MotorFault::FEEDBACK_INVALID;
            faultLocked(i, fault, now_us);
            continue;
        }
        const auto permit = dependencies_.safety.checkOutput(channel.state.command);
        if (!validPermit(permit)) { revokePairLocked(i); continue; }
        if (channel.has_step &&
            (now_us < channel.last_step_us || now_us - channel.last_step_us >
             static_cast<std::uint64_t>(config_.maximum_cycle_ms) * 1000U)) {
            faultLocked(i, MotorFault::CLOCK_OR_DEADLINE, now_us);
            continue;
        }
        const double limit = std::min({static_cast<double>(kActuatorLimitMa),
            static_cast<double>(permit.output_limit_ma),
            static_cast<double>(channel.state.command.output_limit_ma),
            channel.state.command.mode == MotorMode::VELOCITY ?
                static_cast<double>(channel.state.pid.output_limit) : static_cast<double>(kActuatorLimitMa)});
        if (!std::isfinite(channel.state.output_ma)) {
            faultLocked(i, MotorFault::PID_NONFINITE, now_us);
        } else {
            channel.state.output_ma = std::clamp(channel.state.output_ma, -limit, limit);
        }
    }
    if (output_generation_ >= std::numeric_limits<std::uint64_t>::max() - 1U) {
        for (std::size_t i = 0; i < kMotorCount; ++i) { faultLocked(i, MotorFault::OUTPUT_FAILED, now_us); }
        const bool zeroed = dependencies_.output.forceZero();
        for (auto& channel : channels_) {
            if (channel.notification_pending) { channel.event.zero_latched = zeroed; }
        }
        return false;
    }
    MotorOutputBatch batch{};
    batch.generation = ++output_generation_;
    for (const auto& channel : channels_) {
        const auto slot = can::queryMotorSlot(channel.state.node_id);
        if (!slot.valid || !channel.state.enabled || !channel.state.target_active ||
            channel.state.fault != MotorFault::NONE) { continue; }
        const auto group = slot.frame_id == can::kMotorControlFrameIdA ? 0U : 1U;
        batch.currents[group][slot.slot] = MilliampsToC620(static_cast<std::int32_t>(channel.state.output_ma));
        batch.active_mask = static_cast<std::uint8_t>(batch.active_mask |
            static_cast<std::uint8_t>(1U << (channel.state.node_id - can::kMotorNodeMin)));
    }
    bool latched = dependencies_.output.commit(batch);
    if (!latched) {
        for (std::size_t i = 0; i < kMotorCount; ++i) { faultLocked(i, MotorFault::OUTPUT_FAILED, now_us); }
        MotorOutputBatch zero{};
        zero.generation = ++output_generation_;
        latched = dependencies_.output.commit(zero);
        for (auto& channel : channels_) {
            if (channel.notification_pending) { channel.event.zero_latched = latched; }
        }
        return false;
    }
    for (auto& channel : channels_) {
        if (channel.notification_pending) { channel.event.zero_latched = true; }
    }
    return true;
}

void MotorCore::publishFaults() noexcept
{
    std::array<FaultEvent, kMotorCount> pending{};
    std::array<std::size_t, kMotorCount> indices{};
    std::size_t count = 0;
    {
        std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < kMotorCount; ++index) {
            auto& channel = channels_[index];
            if (channel.notification_pending) {
                pending[count] = channel.event;
                indices[count++] = index;
                channel.notification_pending = false;
                channel.notification_in_flight = true;
            }
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& event = pending[i];
        if (event.reload_peer_id != 0) {
            dependencies_.faults.onReloadStopRequested(event.motor_id, event.reload_peer_id);
        }
        dependencies_.faults.onMotorFault(event);
    }
    {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            channels_[indices[i]].notification_in_flight = false;
        }
    }
}

MotorStatus MotorCore::submitCommand(const MotorCommand& command) noexcept
{
    if (command.mode == MotorMode::DISABLED) { return stopMotor(command.motor_id); }
    if (command.mode == MotorMode::POSITION) { return MotorStatus::UNSUPPORTED_MODE; }
    if ((command.mode != MotorMode::VELOCITY && command.mode != MotorMode::CURRENT &&
         command.mode != MotorMode::TORQUE) || !std::isfinite(command.target) ||
        !validSource(command.source) || command.output_limit_ma < 1 ||
        command.output_limit_ma > kActuatorLimitMa ||
        (command.purpose != CommandPurpose::CONTROL && command.purpose != CommandPurpose::SAFETY_RECOVERY)) {
        return MotorStatus::INVALID_COMMAND;
    }
    MotorStatus status = MotorStatus::OK;
    {
        std::lock_guard lock(mutex_);
        if (!running_.load()) { return MotorStatus::NOT_READY; }
        const auto index = findLocked(command.motor_id);
        if (index == kNotFound) { return MotorStatus::UNKNOWN_MOTOR; }
        auto& channel = channels_[index];
        if (channel.state.fault != MotorFault::NONE) { return MotorStatus::FAULT_LATCHED; }
        if ((command.motor_id == 3U || command.motor_id == 4U) && command.mode != MotorMode::VELOCITY) {
            return MotorStatus::UNSUPPORTED_MODE;
        }
        if (command.mode == MotorMode::VELOCITY && !channel.state.pid.enabled) { return MotorStatus::DISABLED; }
        const double target_limit = command.mode == MotorMode::VELOCITY ?
            (command.motor_id == 5U ? 16000.0 : 15000.0) : static_cast<double>(command.output_limit_ma);
        if (std::abs(command.target) > target_limit) { return MotorStatus::INVALID_COMMAND; }
        const auto permit = dependencies_.safety.checkCommand(command);
        if (!validPermit(permit)) { return MotorStatus::PERMISSION_DENIED; }
        std::uint64_t now_us = 0;
        if (!now(now_us)) { return MotorStatus::CLOCK_INVALID; }
        status = feedbackStatusLocked(channel, now_us);
        if (status != MotorStatus::OK) {
            const auto fault = channel.pending_feedback_fault != MotorFault::NONE ?
                channel.pending_feedback_fault : status == MotorStatus::FEEDBACK_TIMEOUT ?
                MotorFault::FEEDBACK_TIMEOUT : MotorFault::FEEDBACK_INVALID;
            faultLocked(index, fault, now_us);
        } else {
            const bool continuous = channel.state.enabled && channel.state.target_active &&
                channel.state.command.mode == command.mode &&
                channel.state.command.source == command.source &&
                channel.state.command.purpose == command.purpose;
            if (!continuous) {
                channel.controller.reset();
                channel.has_step = false;
                channel.state.saturation_cycles = 0;
                channel.next_due_us = now_us;
            }
            channel.state.command = command;
            channel.state.enabled = true;
            channel.state.target_active = true;
            if (command.mode != MotorMode::VELOCITY) {
                channel.state.output_ma = command.target;
            } else if (!continuous) {
                channel.state.output_ma = 0.0;
            }
        }
        if (!commitLocked(now_us)) {
            status = MotorStatus::OUTPUT_FAILED;
        } else if (status == MotorStatus::OK) {
            if (channel.state.fault != MotorFault::NONE) { status = MotorStatus::FAULT_LATCHED; }
            else if (!channel.state.target_active) { status = MotorStatus::PERMISSION_DENIED; }
        }
    }
    publishFaults();
    wake();
    return status;
}

MotorStatus MotorCore::enable(std::uint8_t motor_id, ControlSource source) noexcept
{
    std::lock_guard lock(mutex_);
    if (!running_.load()) { return MotorStatus::NOT_READY; }
    const auto index = findLocked(motor_id);
    if (index == kNotFound) { return MotorStatus::UNKNOWN_MOTOR; }
    auto& channel = channels_[index];
    if (channel.state.fault != MotorFault::NONE) { return MotorStatus::FAULT_LATCHED; }
    if (!channel.state.pid.enabled) { return MotorStatus::DISABLED; }
    MotorCommand probe{motor_id, MotorMode::VELOCITY, 0.0, kActuatorLimitMa, source};
    if (!validSource(source) || !validPermit(dependencies_.safety.checkCommand(probe))) {
        return MotorStatus::PERMISSION_DENIED;
    }
    std::uint64_t now_us = 0;
    if (!now(now_us)) { return MotorStatus::CLOCK_INVALID; }
    const auto feedback = feedbackStatusLocked(channel, now_us);
    if (feedback != MotorStatus::OK) { return feedback; }
    channel.state.enabled = true;
    // Enabling does not restore an old target. submitCommand is still required.
    return MotorStatus::OK;
}

MotorStatus MotorCore::disable(std::uint8_t motor_id) noexcept { return stopMotor(motor_id); }

MotorStatus MotorCore::stopMotor(std::uint8_t motor_id) noexcept
{
    MotorStatus result = MotorStatus::OK;
    {
        std::lock_guard lock(mutex_);
        const auto index = findLocked(motor_id);
        if (index == kNotFound) { return MotorStatus::UNKNOWN_MOTOR; }
        revokePairLocked(index);
        std::uint64_t now_us = 0;
        (void)now(now_us);
        if (!commitLocked(now_us)) { result = MotorStatus::OUTPUT_FAILED; }
    }
    publishFaults();
    return result;
}

void MotorCore::stopAll() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!configured_) { return; }
        for (std::size_t i = 0; i < kMotorCount; ++i) { revokeLocked(i); }
        std::uint64_t now_us = 0;
        (void)now(now_us);
        (void)commitLocked(now_us);
    }
    publishFaults();
}

MotorStatus MotorCore::clearFault(std::uint8_t motor_id) noexcept
{
    std::lock_guard lock(mutex_);
    const auto index = findLocked(motor_id);
    if (index == kNotFound) { return MotorStatus::UNKNOWN_MOTOR; }
    const auto peer = peerLocked(index);
    if (channels_[index].state.fault == MotorFault::NONE &&
        (peer == kNotFound || channels_[peer].state.fault == MotorFault::NONE)) {
        return MotorStatus::OK;
    }
    std::uint64_t now_us = 0;
    if (!now(now_us)) { return MotorStatus::CLOCK_INVALID; }
    if (!dependencies_.output.ready()) { return MotorStatus::NOT_READY; }
    for (const auto affected : std::array<std::size_t, 2>{index, peer}) {
        if (affected == kNotFound) { continue; }
        if (channels_[affected].notification_pending || channels_[affected].notification_in_flight) {
            return MotorStatus::BUSY;
        }
        // Fault reset needs a new valid frame, but does not clear the pending
        // feedback error until all affected channels have passed the checks.
        const auto& feedback = channels_[affected].state.feedback;
        const auto timeout = static_cast<std::uint64_t>(config_.feedback_timeout_ms) * 1000U;
        if (!feedback.valid || !feedback.has_received || feedback.last_valid_time_us > now_us ||
            now_us - feedback.last_valid_time_us >= timeout) { return MotorStatus::FEEDBACK_INVALID; }
        if (!dependencies_.safety.allowFaultReset(channels_[affected].state.motor_id)) {
            return MotorStatus::PERMISSION_DENIED;
        }
    }
    for (const auto affected : std::array<std::size_t, 2>{index, peer}) {
        if (affected == kNotFound) { continue; }
        revokeLocked(affected);
        channels_[affected].state.fault = MotorFault::NONE;
        channels_[affected].pending_feedback_fault = MotorFault::NONE;
    }
    return MotorStatus::OK;
}

MotorStatus MotorCore::readPidConfig(std::uint8_t motor_id, PidConfig& output) const noexcept
{
    std::lock_guard lock(mutex_);
    const auto index = findLocked(motor_id);
    if (index == kNotFound) { return MotorStatus::UNKNOWN_MOTOR; }
    output = channels_[index].state.pid;
    return MotorStatus::OK;
}

PidUpdateResult MotorCore::updatePid(const PidUpdateRequest& request) noexcept
{
    PidUpdateResult result{};
    if (request.source != ControlSource::WEB) {
        result.status = MotorStatus::PERMISSION_DENIED;
        return result;
    }
    if (!ValidatePidConfig(request.parameters)) {
        result.status = MotorStatus::INVALID_CONFIG;
        return result;
    }
    // Serialize runtime edits and their saves without holding the control lock
    // across persistence. This prevents an older edit from saving last.
    std::unique_lock update_lock(pid_update_mutex_);
    PersistenceLocation location{};
    {
        std::lock_guard lock(mutex_);
        if (!configured_ || !running_.load()) { return result; }
        const auto index = findLocked(request.motor_id);
        if (index == kNotFound) { result.status = MotorStatus::UNKNOWN_MOTOR; return result; }
        if (request.parameters.control_period_ms > config_.maximum_cycle_ms) {
            result.status = MotorStatus::INVALID_CONFIG;
            return result;
        }
        const auto permit = dependencies_.safety.checkPidEdit(request);
        if (!permit.allowed || permit.source != ControlSource::WEB ||
            (permit.mode != SystemMode::MANUAL && permit.mode != SystemMode::DEBUG)) {
            result.status = MotorStatus::PERMISSION_DENIED;
            return result;
        }
        auto& channel = channels_[index];
        if (channel.state.pid_revision == std::numeric_limits<std::uint64_t>::max()) {
            result.status = MotorStatus::BUSY;
            return result;
        }
        std::uint64_t now_us = 0;
        if (!now(now_us)) { result.status = MotorStatus::CLOCK_INVALID; return result; }
        channel.state.pid = request.parameters;
        channel.controller.reset();
        channel.has_step = false;
        channel.state.saturation_cycles = 0;
        channel.next_due_us = now_us;
        if (!request.parameters.enabled) {
            revokePairLocked(index);
        } else if (channel.state.enabled && channel.state.target_active &&
                   channel.state.command.mode == MotorMode::VELOCITY) {
            channel.state.output_ma = 0.0;
            result.status = stepLocked(index, now_us);
        }
        result.revision = ++channel.state.pid_revision;
        result.applied = true;
        if (result.status == MotorStatus::NOT_READY) { result.status = MotorStatus::OK; }
        if (!commitLocked(now_us)) { result.status = MotorStatus::OUTPUT_FAILED; }
        location = config_.persistence;
    }
    wake();
    result.persistence = dependencies_.persistence.saveMotorPid(
        request.motor_id, result.revision, request.parameters, location);
    if (result.persistence != PersistenceStatus::SAVED &&
        result.persistence != PersistenceStatus::UNAVAILABLE) {
        result.persistence = PersistenceStatus::FAILED;
    }
    update_lock.unlock();
    publishFaults();
    wake();
    return result;
}

MotorStatus MotorCore::stepLocked(std::size_t index, std::uint64_t now_us) noexcept
{
    auto& channel = channels_[index];
    if (channel.state.fault != MotorFault::NONE) { return MotorStatus::FAULT_LATCHED; }
    if (channel.pending_feedback_fault != MotorFault::NONE) {
        faultLocked(index, channel.pending_feedback_fault, now_us);
        return MotorStatus::FEEDBACK_INVALID;
    }
    if (!channel.state.enabled || !channel.state.target_active) { return MotorStatus::DISABLED; }
    const auto feedback = feedbackStatusLocked(channel, now_us);
    if (feedback != MotorStatus::OK) {
        faultLocked(index, feedback == MotorStatus::FEEDBACK_TIMEOUT ?
            MotorFault::FEEDBACK_TIMEOUT : MotorFault::FEEDBACK_INVALID, now_us);
        return feedback;
    }
    const auto permit = dependencies_.safety.checkOutput(channel.state.command);
    if (!validPermit(permit)) {
        revokePairLocked(index);
        return MotorStatus::PERMISSION_DENIED;
    }
    if (channel.has_step && now_us <= channel.last_step_us) {
        if (now_us < channel.last_step_us) {
            faultLocked(index, MotorFault::CLOCK_OR_DEADLINE, now_us);
            return MotorStatus::CLOCK_INVALID;
        }
        return MotorStatus::NOT_DUE;
    }
    if (now_us < channel.next_due_us) { return MotorStatus::NOT_DUE; }
    const auto period_us = static_cast<std::uint64_t>(channel.state.pid.control_period_ms) * 1000U;
    const auto elapsed_us = channel.has_step ? now_us - channel.last_step_us : period_us;
    const auto maximum_us = static_cast<std::uint64_t>(config_.maximum_cycle_ms) * 1000U;
    if (elapsed_us > maximum_us || now_us - channel.next_due_us > maximum_us) {
        faultLocked(index, MotorFault::CLOCK_OR_DEADLINE, now_us);
        return MotorStatus::CLOCK_INVALID;
    }
    const double limit = std::min({static_cast<double>(kActuatorLimitMa),
        static_cast<double>(permit.output_limit_ma),
        static_cast<double>(channel.state.command.output_limit_ma)});
    if (channel.state.command.mode == MotorMode::VELOCITY) {
        const auto result = channel.controller.step(channel.state.pid, channel.state.command.target,
            channel.state.feedback.speed_rpm, static_cast<double>(elapsed_us) / 1000000.0, limit);
        channel.state.saturation_cycles = result.saturation_cycles;
        if (result.fault != MotorFault::NONE) {
            faultLocked(index, result.fault, now_us);
            return MotorStatus::FAULT_LATCHED;
        }
        channel.state.output_ma = result.output_ma;
    } else {
        channel.state.output_ma = std::clamp(channel.state.command.target, -limit, limit);
    }
    channel.has_step = true;
    channel.last_step_us = now_us;
    // Advance absolute deadlines, skipping missed slots instead of catching up.
    const auto missed = (now_us - channel.next_due_us) / period_us + 1U;
    channel.next_due_us += missed * period_us;
    return MotorStatus::OK;
}

MotorStatus MotorCore::updateOnce(std::uint8_t motor_id) noexcept
{
    MotorStatus result = MotorStatus::NOT_READY;
    {
        std::lock_guard lock(mutex_);
        if (!running_.load()) { return result; }
        const auto index = findLocked(motor_id);
        if (index == kNotFound) { return MotorStatus::UNKNOWN_MOTOR; }
        std::uint64_t now_us = 0;
        if (!now(now_us)) {
            for (std::size_t i = 0; i < kMotorCount; ++i) { faultLocked(i, MotorFault::CLOCK_OR_DEADLINE, 0); }
            result = MotorStatus::CLOCK_INVALID;
        } else {
            result = stepLocked(index, now_us);
        }
        if (!commitLocked(now_us)) { result = MotorStatus::OUTPUT_FAILED; }
        else if (result == MotorStatus::OK) {
            if (channels_[index].state.fault != MotorFault::NONE) { result = MotorStatus::FAULT_LATCHED; }
            else if (!channels_[index].state.target_active) { result = MotorStatus::PERMISSION_DENIED; }
        }
    }
    publishFaults();
    return result;
}

void MotorCore::tick() noexcept
{
    {
        std::lock_guard lock(mutex_);
        if (!running_.load()) { return; }
        std::uint64_t now_us = 0;
        if (!now(now_us)) {
            for (std::size_t i = 0; i < kMotorCount; ++i) { faultLocked(i, MotorFault::CLOCK_OR_DEADLINE, 0); }
        } else {
            for (std::size_t i = 0; i < kMotorCount; ++i) { (void)stepLocked(i, now_us); }
        }
        (void)commitLocked(now_us);
    }
    publishFaults();
}

void MotorCore::wake() noexcept
{
    {
        std::lock_guard lock(wait_mutex_);
        wake_requested_.store(true, std::memory_order_release);
    }
    wait_cv_.notify_all();
}

void MotorCore::run() noexcept
{
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    while (running_.load(std::memory_order_acquire)) {
        tick();
        std::uint32_t period_ms = 1;
        {
            std::lock_guard lock(mutex_);
            period_ms = std::min(config_.motor_loop_period_ms, config_.pid_period_ms);
            for (const auto& channel : channels_) {
                period_ms = std::min(period_ms, channel.state.pid.control_period_ms);
            }
        }
        const auto period = std::chrono::milliseconds(period_ms);
        const auto current = Clock::now();
        if (next <= current) {
            next += period;
            if (next <= current) { next = current + period; }
        } else {
            next = std::min(next, current + period);
        }
        std::unique_lock wait_lock(wait_mutex_);
        wait_cv_.wait_until(wait_lock, next, [this] {
            return !running_.load(std::memory_order_acquire) ||
                   wake_requested_.exchange(false, std::memory_order_acq_rel);
        });
    }
}

} // namespace marshland::atom05
