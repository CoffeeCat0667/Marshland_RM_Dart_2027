#include "atom03_SBUS/sbus_core.hpp"

#include <array>
#include <chrono>
#include <system_error>
#include <vector>

namespace marshland::atom03 {
namespace {

class ByteRingBuffer final {
public:
    explicit ByteRingBuffer(std::size_t capacity) : data_(capacity) {}

    void push(const std::uint8_t* data, std::size_t size) noexcept
    {
        if (data_.empty()) {
            return;
        }
        for (std::size_t i = 0; i < size; ++i) {
            if (size_ == data_.size()) {
                tail_ = (tail_ + 1U) % data_.size();
                --size_;
            }
            data_[head_] = data[i];
            head_ = (head_ + 1U) % data_.size();
            ++size_;
        }
    }

    bool pop(std::uint8_t& value) noexcept
    {
        if (size_ == 0U) {
            return false;
        }
        value = data_[tail_];
        tail_ = (tail_ + 1U) % data_.size();
        --size_;
        return true;
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t head_{0};
    std::size_t tail_{0};
    std::size_t size_{0};
};

std::uint64_t monotonicNow(const marshland::app::IMonotonicClock& clock) noexcept
{
    const auto value = clock.nowMicros();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

} // namespace

SbusCore::SbusCore(marshland::sbus::IUartByteStream& uart,
                   const marshland::app::IMonotonicClock& clock,
                   marshland::sbus::SbusChannelStore& store,
                   marshland::app::IInputReporter* input_reporter,
                   marshland::app::IControlAuthority* control_authority) noexcept
    : uart_(uart), clock_(clock), store_(store), input_reporter_(input_reporter),
      control_authority_(control_authority)
{
}

SbusCore::~SbusCore() noexcept
{
    stop();
}

bool SbusCore::start(const SbusConfig& config) noexcept
{
    if (config.frame_bytes < marshland::sbus::kMinimumDecodeFrameBytes ||
        config.receive_period_ms == 0U || config.loss_timeout_ms == 0U) {
        return false;
    }
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    config_ = config;
    decoder_ = SbusDecoder(config_);
    if (!uart_.configure(config_.serial)) {
        running_.store(false);
        return false;
    }
    try {
        worker_ = std::thread(&SbusCore::run, this);
    } catch (const std::system_error&) {
        running_.store(false);
        return false;
    }
    return true;
}

void SbusCore::stop() noexcept
{
    if (!running_.exchange(false)) {
        return;
    }
    uart_.cancel();
    retry_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

marshland::sbus::SbusChannelSnapshot SbusCore::readSbusSnapshot() const noexcept
{
    return store_.snapshot();
}

void SbusCore::run() noexcept
{
    std::array<std::uint8_t, 256U> input{};
    try {
        ByteRingBuffer ring(static_cast<std::size_t>(config_.frame_bytes) + input.size());
        std::vector<std::uint8_t> frame(config_.frame_bytes);
        std::size_t frame_size = 0;
        while (running_.load()) {
            std::size_t count = 0;
            const auto status = uart_.read(input.data(), input.size(), count);
            switch (status) {
            case marshland::sbus::UartReadStatus::CANCELLED:
                return;
            case marshland::sbus::UartReadStatus::DEVICE_ERROR:
                updateOfflineState(monotonicNow(clock_));
                waitBeforeRetry();
                continue;
            case marshland::sbus::UartReadStatus::TEMPORARY_ERROR:
                waitBeforeRetry();
                continue;
            case marshland::sbus::UartReadStatus::TIMEOUT:
                updateOfflineState(monotonicNow(clock_));
                continue;
            case marshland::sbus::UartReadStatus::DATA:
                break;
            }
            ring.push(input.data(), count);

            std::uint8_t byte = 0;
            while (ring.pop(byte)) {
                if (frame_size == 0U && byte != 0x0FU) {
                    continue;
                }
                frame[frame_size++] = byte;
                if (frame_size == frame.size()) {
                    processFrame(frame.data());
                    frame_size = 0U;
                }
            }
        }
    } catch (...) {
        updateOfflineState(monotonicNow(clock_));
    }
}

void SbusCore::processFrame(const std::uint8_t* frame) noexcept
{
    marshland::sbus::SbusChannelSnapshot next{};
    if (!decoder_.decode(frame, config_.frame_bytes, next)) {
        updateOfflineState(monotonicNow(clock_));
        return;
    }
    const auto half_range = (static_cast<std::int32_t>(config_.channel_maximum) -
                             static_cast<std::int32_t>(config_.channel_minimum)) / 2;
    next.scaled_channels[11] = half_range == 0
        ? 0
        : (static_cast<std::int32_t>(next.raw_channels[11]) -
           static_cast<std::int32_t>(config_.channel_center)) *
          marshland::sbus::kForceSpeedLimitRpm / half_range;
    const auto now = monotonicNow(clock_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        next.frame_sequence = state_.frame_sequence + 1U;
        next.last_valid_frame_time_us = now;
        next.online = true;
        next.control_source = marshland::ControlSource::NONE;
        next.estop_state = marshland::sbus::EstopState::CENTER;
        next.fire_request_frames = 0;
        next.fire_request_confirmed = false;

        const auto control = next.raw_channels[4];
        if (control == config_.channel_minimum) {
            next.control_source = marshland::ControlSource::AUTO;
        } else if (control == config_.channel_center) {
            next.control_source = marshland::ControlSource::WEB;
        } else if (control == config_.channel_maximum) {
            next.control_source = marshland::ControlSource::SBUS;
        }
        const auto centered_estop = static_cast<std::int32_t>(next.raw_channels[5]) -
                                    static_cast<std::int32_t>(config_.channel_center);
        next.estop_state = centered_estop > 0 ? marshland::sbus::EstopState::NORMAL
            : centered_estop < 0 ? marshland::sbus::EstopState::ESTOP
                                 : marshland::sbus::EstopState::CENTER;
        if (next.raw_channels[6] == config_.channel_minimum) {
            next.fire_request_frames = static_cast<std::uint8_t>(
                state_.fire_request_frames < marshland::sbus::kFireConfirmationFrames
                    ? state_.fire_request_frames + 1U
                    : marshland::sbus::kFireConfirmationFrames);
        }
        next.fire_request_confirmed =
            next.fire_request_frames >= marshland::sbus::kFireConfirmationFrames;
        state_ = next;
    }
    store_.replace(next);
    publishInputs(next);
}

void SbusCore::updateOfflineState(std::uint64_t now_us) noexcept
{
    marshland::sbus::SbusChannelSnapshot offline{};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (state_.last_valid_frame_time_us == 0U ||
            now_us < state_.last_valid_frame_time_us ||
            now_us - state_.last_valid_frame_time_us <
                static_cast<std::uint64_t>(config_.loss_timeout_ms) * 1000U) {
            return;
        }
        state_.online = false;
        state_.control_source = marshland::ControlSource::WEB;
        state_.fire_request_frames = 0;
        state_.fire_request_confirmed = false;
        offline = state_;
    }
    store_.replace(offline);
    publishInputs(offline);
}

void SbusCore::publishInputs(const marshland::sbus::SbusChannelSnapshot& snapshot) noexcept
{
    if (input_reporter_ != nullptr) {
        input_reporter_->ReportSbus(snapshot.online,
                                    snapshot.control_source == marshland::ControlSource::AUTO);
        if (snapshot.online) {
            input_reporter_->ReportSoftwareEstop(
                snapshot.estop_state == marshland::sbus::EstopState::ESTOP);
        }
    }
    if (control_authority_ == nullptr || !snapshot.online) {
        return;
    }
    marshland::state_arbitration::ControlIntent intent{};
    intent.source = snapshot.control_source;
    intent.timestamp_us = snapshot.last_valid_frame_time_us;
    if (snapshot.control_source != marshland::ControlSource::NONE) {
        intent.kind = marshland::state_arbitration::IntentKind::CONTROL;
        (void)control_authority_->SubmitIntent(intent);
    }
    if (snapshot.control_source == marshland::ControlSource::SBUS &&
        last_reported_control_source_ != marshland::ControlSource::SBUS) {
        (void)control_authority_->TerminateAutoFireOnSbusTakeover();
    }
    if (snapshot.control_source == marshland::ControlSource::SBUS &&
        snapshot.fire_request_confirmed) {
        const auto authorization = control_authority_->AuthorizeFire(
            marshland::ControlSource::SBUS);
        if (authorization.allowed) {
            intent.kind = marshland::state_arbitration::IntentKind::FIRE;
            (void)control_authority_->SubmitIntent(intent);
        }
    }
    last_reported_control_source_ = snapshot.control_source;
}

void SbusCore::waitBeforeRetry() noexcept
{
    std::unique_lock<std::mutex> lock(retry_mutex_);
    retry_cv_.wait_for(lock, std::chrono::milliseconds(config_.receive_period_ms),
                       [this] { return !running_.load(); });
}

} // namespace marshland::atom03



