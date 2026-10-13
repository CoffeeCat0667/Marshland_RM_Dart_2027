#pragma once

#include <cstdint>
#include "IConfigView.hpp"
#include "SbusTypes.hpp"

namespace marshland::atom03 {

struct SbusConfig final {
    marshland::sbus::SbusSerialParameters serial{};
    std::uint8_t channel_scale_x{1};
    std::uint32_t receive_period_ms{20};
    std::uint32_t loss_timeout_ms{200};
    std::uint16_t frame_bytes{0};
    std::uint16_t channel_center{0};
    std::uint16_t channel_minimum{0};
    std::uint16_t channel_maximum{0};
    std::uint8_t fire_confirmation_frames{20};
};

struct ConfigLoadResult final {
    bool success{false};
    const char* error_message{"configuration not loaded"};
};

ConfigLoadResult LoadSbusConfig(const marshland::config::IConfigView& view,
                                SbusConfig& output) noexcept;

} // namespace marshland::atom03
