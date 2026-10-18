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
// The 16-slot layout is retained; CH16 is ignored and scaled_channels[15] is zero.
// CH7 fire (index 6) and CH13 feed servo (index 12) both use (raw-center)*x.
// Neither channel applies a hold time, confirmation counter, or actuator command.
// Only 200 ms without a valid frame clears online; invalid frames retain last data.
struct SbusData final {
    mutable std::mutex mutex;
    std::array<std::int32_t, kChannelCount> scaled_channels{};
    bool online{false};
};

// Blocking receiver; the caller owns the thread and the lifetime of both objects.
// Returns true on requested stop, false on invalid parameters or device failure.
// Use only one receiver per SbusData; keep data and stop_requested alive for the call.
// Poll waits are capped by the 200 ms valid-frame deadline. On stop/error, the port
// closes first; if still online, return waits for that deadline to clear online.
// Never hold data.mutex while waiting for this function. Serial format: 100K 8E2.
bool ReceiveSbus(const char* serial_device, SbusData& data,
                 const std::atomic<bool>& stop_requested,
                 std::uint8_t channel_scale_x, std::uint16_t frame_bytes,
                 std::uint16_t channel_center, std::uint16_t channel_minimum,
                 std::uint16_t channel_maximum,
                 std::uint32_t read_timeout_ms = 20U) noexcept;

} // namespace marshland::sbus
