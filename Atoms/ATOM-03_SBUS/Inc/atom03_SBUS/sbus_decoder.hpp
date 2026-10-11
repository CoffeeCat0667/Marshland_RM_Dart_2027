#pragma once

#include <cstddef>
#include <cstdint>
#include "SbusTypes.hpp"
#include "sbus_config.hpp"

namespace marshland::atom03 {

class SbusDecoder final {
public:
    explicit SbusDecoder(SbusConfig config) noexcept : config_(config) {}
    bool decode(const std::uint8_t* frame, std::size_t size,
                marshland::sbus::SbusChannelSnapshot& output) const noexcept;
private:
    SbusConfig config_;
};

} // namespace marshland::atom03
