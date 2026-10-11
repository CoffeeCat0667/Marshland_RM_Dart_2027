#include "atom03_SBUS/sbus_core.hpp"

#include <array>
#include <system_error>

namespace marshland::atom03 {
namespace {

std::uint64_t monotonicNow(const marshland::app::IMonotonicClock& clock) noexcept
{
    const auto value = clock.nowMicros();
    return value < 0 ? 0U : static_cast<std::uint64_t>(value);
}

} // namespace

SbusCore::SbusCore(marshland::sbus::IUartByteStream& uart,
                   const marshland::app::IMonotonicClock& clock,
                   marshland::sbus::SbusChannelStore& store) noexcept
    : uart_(uart), clock_(clock), store_(store)
{
}

SbusCore::~SbusCore() noexcept
{
    stop();
}

bool SbusCore::start(const SbusConfig& config) noexcept
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;
    }
    config_ = config;
    decoder_ = SbusDecoder(config_);
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
    std::array<std::uint8_t, marshland::sbus::kFrameBytes> frame{};
    std::size_t frame_size = 0;
    while (running_.load()) {
        std::uint8_t byte = 0;
        std::size_t count = 0;
        if (!uart_.read(&byte, 1U, count)) {
            updateOfflineState(monotonicNow(clock_));
            continue;
        }
        if (count == 0U) {
            updateOfflineState(monotonicNow(clock_));
            continue;
        }
        if (frame_size == 0U && byte != 0x0FU) {
            updateOfflineState(monotonicNow(clock_));
            continue;
        }
        frame[frame_size++] = byte;
        if (frame_size == frame.size()) {
            processFrame(frame.data());
            frame_size = 0U;
        }
    }
}

void SbusCore::processFrame(const std::uint8_t* frame) noexcept
{
    marshland::sbus::SbusChannelSnapshot next{};
    if (!decoder_.decode(frame, marshland::sbus::kFrameBytes, next)) {
        updateOfflineState(monotonicNow(clock_));
        return;
    }
    const auto now = monotonicNow(clock_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    next.frame_sequence = state_.frame_sequence + 1U;
    next.last_valid_frame_time_us = now;
    next.online = true;
    next.control_source = marshland::ControlSource::NONE;
    next.estop_state = marshland::sbus::EstopState::CENTER;
    next.fire_request_frames = 0;
    next.fire_request_confirmed = false;

    const auto control = next.raw_channels[4];
    if (control == 364U) {
        next.control_source = marshland::ControlSource::AUTO;
    } else if (control == 1024U) {
        next.control_source = marshland::ControlSource::WEB;
    } else if (control == 1684U) {
        next.control_source = marshland::ControlSource::SBUS;
    }
    const auto centered_estop = static_cast<std::int32_t>(next.raw_channels[5]) - 1024;
    next.estop_state = centered_estop > 0 ? marshland::sbus::EstopState::NORMAL
        : centered_estop < 0 ? marshland::sbus::EstopState::ESTOP
                             : marshland::sbus::EstopState::CENTER;
    if (next.raw_channels[6] == 364U) {
        next.fire_request_frames = static_cast<std::uint8_t>(
            state_.fire_request_frames < marshland::sbus::kFireConfirmationFrames
                ? state_.fire_request_frames + 1U
                : marshland::sbus::kFireConfirmationFrames);
    }
    next.fire_request_confirmed =
        next.fire_request_frames >= marshland::sbus::kFireConfirmationFrames;
    state_ = next;
    store_.replace(state_);
}

void SbusCore::updateOfflineState(std::uint64_t now_us) noexcept
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
    store_.replace(state_);
}

} // namespace marshland::atom03



