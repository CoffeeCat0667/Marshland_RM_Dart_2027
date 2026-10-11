#pragma once

#include <cstdint>
#include "IConfigView.hpp"

namespace marshland::atom03 {

struct SbusConfig final {
    std::uint8_t channel_scale_x{1};
    std::uint32_t receive_period_ms{20};
    std::uint32_t loss_timeout_ms{200};
    std::uint16_t frame_bytes{25};
    std::uint16_t channel_center{1024};
    std::uint16_t channel_minimum{364};
    std::uint16_t channel_maximum{1684};
    std::uint8_t fire_confirmation_frames{20};
};

struct ConfigLoadResult final {
    bool success{false};
    const char* error_message{"configuration not loaded"};
};

ConfigLoadResult LoadSbusConfig(const marshland::config::IConfigView& view,
                                SbusConfig& output) noexcept;

} // namespace marshland::atom03
