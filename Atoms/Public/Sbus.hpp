#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace marshland::sbus {

inline constexpr std::size_t kChannelCount = 16;

// One writer (ATOM-03). All readers must hold mutex while copying the fields.
// Index 0 is CH1. Scaled CH12 (index 11) is in rpm; other channels use x.
// CH7 fire (index 6) and CH13 feed servo (index 12) both use (raw-center)*x.
// Neither channel applies a hold time, confirmation counter, or actuator command.
// Errors and stop retain channel values but clear online. This is not an age watchdog.
struct SbusData final {
    mutable std::mutex mutex;
    std::array<std::int32_t, kChannelCount> scaled_channels{};
    bool online{false};
};

// Blocking receiver; the caller owns the thread and the lifetime of both objects.
// Returns true on requested stop, false on invalid parameters or device failure.
// Stop is checked at least after each poll timeout; data.mutex must not be held
// while waiting for this function to finish. Serial format is fixed at 100K 8E2.
bool ReceiveSbus(const char* serial_device, SbusData& data,
                 const std::atomic<bool>& stop_requested,
                 std::uint8_t channel_scale_x, std::uint16_t frame_bytes,
                 std::uint16_t channel_center, std::uint16_t channel_minimum,
                 std::uint16_t channel_maximum,
                 std::uint32_t read_timeout_ms = 20U) noexcept;

} // namespace marshland::sbus
