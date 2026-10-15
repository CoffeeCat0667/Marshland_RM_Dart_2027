#include "atom07_servo_driver/servo_core.hpp"

#include <cerrno>
#include <chrono>
#include <system_error>

#include "MarshlandTypes.hpp"
#include "atom07_servo_driver/angle_to_pulse.hpp"

namespace marshland::atom07 {

ServoCore::ServoCore(II2cBus& bus,
                     const marshland::app::IMonotonicClock& clock,
                     marshland::app::IInputReporter* input_reporter,
                     const marshland::app::IStateReader* state_reader) noexcept
    : bus_(bus),
      clock_(clock),
      input_reporter_(input_reporter),
      state_reader_(state_reader)
{
}

ServoCore::~ServoCore() noexcept
{
    stop();
}

bool ServoCore::start(const ServoConfig& config) noexcept
{
    if (running_.load()) {
        return false;
    }
    if (config.i2c_device.empty() || config.slots.empty()) {
        return false;
    }
    try {
        config_ = config;
    } catch (...) {
        return false;
    }

    ready_.store(false);
    {
        std::lock_guard<std::mutex> target_lock(target_mutex_);
        for (auto& value : last_accepted_) {
            value.reset();
        }
    }
    forgetFault();

    // 装配层无需解析配置：设备路径/地址/超时由内核在此按配置打开总线。
    if (!bus_.isOpen()) {
        const I2cStatus opened = bus_.open(config_.i2c_device.c_str(),
                                           static_cast<std::uint8_t>(config_.device_address),
                                           config_.iic_comm_timeout_ms);
        if (!opened.ok) {
            const ServoFaultKind kind = (opened.system_error == ETIMEDOUT)
                                            ? ServoFaultKind::Timeout
                                            : ServoFaultKind::InitFailed;
            recordFault(kind, 0, -1, opened.system_error);
            return false;
        }
    }

    device_.emplace(bus_, static_cast<std::uint8_t>(config_.device_address));
    if (!device_->initialize(config_.pwm_frequency_hz, config_.pwm_resolution_bits)) {
        const ServoFaultKind kind = (device_->lastFaultKind() == ServoFaultKind::None)
                                        ? ServoFaultKind::InitFailed
                                        : device_->lastFaultKind();
        recordFault(kind, 0, -1, device_->lastSystemError());
        device_.reset();
        return false;
    }

    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        queue_.clear();
    }
    running_.store(true);
    try {
        worker_ = std::thread(&ServoCore::run, this);
    } catch (const std::system_error&) {
        running_.store(false);
        device_.reset();
        recordFault(ServoFaultKind::InitFailed, 0, -1, 0);
        return false;
    }
    ready_.store(true);
    return true;
}

void ServoCore::stop() noexcept
{
    running_.store(false);
    queue_cv_.notify_all();
    done_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> queue_lock(queue_mutex_);
        queue_.clear();
    }
    ready_.store(false);
    if (device_.has_value()) {
        device_->resetFault();
        device_.reset();
    }
}

ServoStatus ServoCore::gateStatus() const noexcept
{
    // 先判定闸门（持 StateReadView 租约期间不做任何 I/O），再判定本模块故障状态。
    if (state_reader_ != nullptr) {
        const marshland::app::StateReadView view = state_reader_->ReadState();
        if (view.estop_latched()) {
            return ServoStatus::EstopRejected;
        }
        if (!view.hardware_ready()) {
            return ServoStatus::NotReady;
        }
        if (view.run_gate() != marshland::app::RunGateReason::ALLOWED) {
            return ServoStatus::NotReady;
        }
    }
    {
        std::lock_guard<std::mutex> fault_lock(fault_mutex_);
        if (fault_.active()) {
            return ServoStatus::FaultRejected; // comm_fault_servos = HOLD
        }
    }
    return ServoStatus::Ok;
}

bool ServoCore::locateSlot(int servo_id, const ServoSlotConfig*& slot,
                           std::size_t& index) const noexcept
{
    if (servo_id <= 0) {
        return false;
    }
    for (std::size_t i = 0; i < config_.slots.size(); ++i) {
        if (config_.slots[i].servo_id == servo_id) {
            slot = &config_.slots[i];
            index = i;
            return true;
        }
    }
    return false;
}

bool ServoCore::locateSlot(ServoMechanism mechanism, const ServoSlotConfig*& slot,
                           std::size_t& index) const noexcept
{
    for (std::size_t i = 0; i < config_.slots.size(); ++i) {
        if (config_.slots[i].mechanism == mechanism) {
            slot = &config_.slots[i];
            index = i;
            return true;
        }
    }
    return false;
}

WriteResult ServoCore::writeTarget(int servo_id, double angle_deg) noexcept
{
    const ServoSlotConfig* slot = nullptr;
    std::size_t index = 0U;
    if (!locateSlot(servo_id, slot, index) || slot == nullptr) {
        WriteResult result{};
        result.servo_id = servo_id;
        result.angle_deg = angle_deg;
        result.status = ServoStatus::ChannelNotWired; // 未登记/未接入，禁止推断
        return result;
    }
    return writeLocated(*slot, index, angle_deg);
}

WriteResult ServoCore::writeTarget(ServoMechanism mechanism, double angle_deg) noexcept
{
    const ServoSlotConfig* slot = nullptr;
    std::size_t index = 0U;
    if (!locateSlot(mechanism, slot, index) || slot == nullptr) {
        WriteResult result{};
        result.servo_id = 0;
        result.angle_deg = angle_deg;
        result.status = ServoStatus::ChannelNotWired;
        return result;
    }
    return writeLocated(*slot, index, angle_deg);
}

WriteResult ServoCore::writeLocated(const ServoSlotConfig& slot, std::size_t index,
                                    double angle_deg) noexcept
{
    WriteResult result{};
    result.servo_id = slot.servo_id;
    result.angle_deg = angle_deg;
    result.channel = slot.channel;

    if (!running_.load() || !ready_.load() || !device_.has_value()) {
        result.status = ServoStatus::NotReady;
        return result;
    }
    if (slot.mechanism == ServoMechanism::Reload || !slot.wired()) {
        result.status = ServoStatus::ChannelNotWired; // 换弹已改为双 M3508：不产生 PWM
        return result;
    }

    const ServoStatus gate = gateStatus();
    if (gate != ServoStatus::Ok) {
        result.status = gate;
        return result;
    }
    if (!config_.channelInRange(slot.channel)) {
        result.status = ServoStatus::ChannelInvalid;
        return result;
    }

    const PulseConversion conversion = angleToOffTicks(angle_deg,
                                                       config_.angle_min_deg,
                                                       config_.angle_max_deg,
                                                       config_.pulse_min_us,
                                                       config_.pulse_max_us,
                                                       config_.pwm_resolution_bits,
                                                       config_.pwm_frequency_hz);
    if (!conversion.ok) {
        result.status = ServoStatus::AngleOutOfRange; // REJECT_AND_WARN：不改当前有效目标
        return result;
    }
    result.off_ticks = conversion.off_ticks;

    std::shared_ptr<Request> request;
    try {
        request = std::make_shared<Request>();
    } catch (...) {
        result.status = ServoStatus::NotReady;
        return result;
    }
    request->servo_id = slot.servo_id;
    request->channel = slot.channel;
    request->off_ticks = conversion.off_ticks;
    request->status = ServoStatus::NotReady;

    {
        std::unique_lock<std::mutex> queue_lock(queue_mutex_);
        if (!running_.load()) {
            result.status = ServoStatus::NotReady;
            return result;
        }
        try {
            queue_.push_back(request);
        } catch (...) {
            result.status = ServoStatus::NotReady;
            return result;
        }
        const auto pending = static_cast<long>(queue_.size());
        queue_cv_.notify_one();

        // 单次事务超时 = timing.iic_comm_timeout_ms；等待预算按队列长度放大。
        const auto budget = std::chrono::milliseconds(
            ((static_cast<long>(config_.iic_comm_timeout_ms) + 20L) * (pending + 1L)) + 50L);
        done_cv_.wait_for(queue_lock, budget,
                          [&request, this] { return request->done || !running_.load(); });
        if (!request->done) {
            result.status = running_.load() ? ServoStatus::Timeout : ServoStatus::NotReady;
            return result;
        }
    }

    result.status = request->status;
    if (result.status == ServoStatus::Ok && index < last_accepted_.size()) {
        std::lock_guard<std::mutex> target_lock(target_mutex_);
        last_accepted_[index] = angle_deg;
    }
    return result;
}

std::optional<double> ServoCore::lastAcceptedTarget(int servo_id) const noexcept
{
    const ServoSlotConfig* slot = nullptr;
    std::size_t index = 0U;
    if (!locateSlot(servo_id, slot, index) || index >= last_accepted_.size()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> target_lock(target_mutex_);
    return last_accepted_[index];
}

std::optional<double> ServoCore::lastAcceptedTarget(ServoMechanism mechanism) const noexcept
{
    const ServoSlotConfig* slot = nullptr;
    std::size_t index = 0U;
    if (!locateSlot(mechanism, slot, index) || index >= last_accepted_.size()) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> target_lock(target_mutex_);
    return last_accepted_[index];
}

ServoFault ServoCore::lastIicFault() const noexcept
{
    std::lock_guard<std::mutex> fault_lock(fault_mutex_);
    return fault_;
}

void ServoCore::resetFault() noexcept
{
    forgetFault();
    if (device_.has_value()) {
        device_->resetFault();
    }
}

void ServoCore::forgetFault() noexcept
{
    {
        std::lock_guard<std::mutex> fault_lock(fault_mutex_);
        fault_ = ServoFault{};
    }
    if (input_reporter_ != nullptr) {
        input_reporter_->ReportFault(false, marshland::FaultSeverity::SEVERE);
    }
}

void ServoCore::recordFault(ServoFaultKind kind, int servo_id, int channel,
                            int system_error) noexcept
{
    {
        std::lock_guard<std::mutex> fault_lock(fault_mutex_);
        fault_.kind = (kind == ServoFaultKind::None) ? ServoFaultKind::IicError : kind;
        fault_.servo_id = servo_id;
        fault_.channel = channel;
        fault_.system_error = system_error;
        fault_.timestamp_us = clock_.nowMicros();
    }
    if (input_reporter_ != nullptr) {
        // Config.md §14.2：F-IIC-001 / F-SERVO-001 均为"严重"；记录由 ATOM-18 负责。
        input_reporter_->ReportFault(true, marshland::FaultSeverity::SEVERE);
    }
}

void ServoCore::run() noexcept
{
    for (;;) {
        std::shared_ptr<Request> request;
        {
            std::unique_lock<std::mutex> queue_lock(queue_mutex_);
            queue_cv_.wait(queue_lock, [this] { return !running_.load() || !queue_.empty(); });
            if (!running_.load()) {
                break;
            }
            if (queue_.empty()) {
                continue;
            }
            request = queue_.front();
            queue_.pop_front();
        }

        const bool written =
            device_.has_value() &&
            device_->writeChannelOffTicks(static_cast<std::uint8_t>(request->channel),
                                          request->off_ticks);
        if (written) {
            request->status = ServoStatus::Ok;
        } else if (device_.has_value()) {
            const ServoFaultKind kind = device_->lastFaultKind();
            request->status = (kind == ServoFaultKind::Timeout) ? ServoStatus::Timeout
                                                               : ServoStatus::IicError;
            recordFault(kind, request->servo_id, request->channel, device_->lastSystemError());
        } else {
            request->status = ServoStatus::NotReady;
        }

        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex_);
            request->done = true;
        }
        done_cv_.notify_all();
    }
}

} // namespace marshland::atom07
