#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "MarshlandTypes.hpp"

namespace marshland::sbus {

struct SbusSerialParameters final {
    std::string serial_device;
};

inline constexpr std::size_t kChannelCount = 16;
inline constexpr std::size_t kFrameBytes = 25;
inline constexpr std::uint16_t kChannelCenter = 1024;
inline constexpr std::uint16_t kChannelHalfRange = 660;
inline constexpr std::uint16_t kChannelMinimum = 364;
inline constexpr std::uint16_t kChannelMaximum = 1684;
inline constexpr std::uint8_t kFireConfirmationFrames = 20;
inline constexpr std::int32_t kForceSpeedLimitRpm = 2000;

enum class FrameValidity : std::uint8_t {
    INVALID,
    VALID,
};

enum class EstopState : std::uint8_t {
    CENTER,
    NORMAL,
    ESTOP,
};

struct SbusChannelSnapshot {
    std::array<std::uint16_t, kChannelCount> raw_channels{};
    std::array<std::int32_t, kChannelCount> scaled_channels{};
    marshland::ControlSource control_source{marshland::ControlSource::NONE};
    EstopState estop_state{EstopState::CENTER};
    bool fire_request_confirmed{false};
    std::uint8_t fire_request_frames{0};
    FrameValidity frame_validity{FrameValidity::INVALID};
    bool online{false};
    std::uint64_t frame_sequence{0};
    std::uint64_t last_valid_frame_time_us{0};
};

class SbusChannelStore final {
public:
    SbusChannelStore() = default;
    SbusChannelStore(const SbusChannelStore&) = delete;
    SbusChannelStore& operator=(const SbusChannelStore&) = delete;

    SbusChannelSnapshot snapshot() const noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void replace(const SbusChannelSnapshot& snapshot) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = snapshot;
    }

private:
    mutable std::mutex mutex_;
    SbusChannelSnapshot snapshot_{};
};

} // namespace marshland::sbus

