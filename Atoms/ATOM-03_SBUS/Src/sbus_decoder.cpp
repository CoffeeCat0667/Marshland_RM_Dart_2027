#include "atom03_SBUS/sbus_decoder.hpp"

namespace marshland::atom03 {

bool SbusDecoder::decode(const std::uint8_t* frame, std::size_t size,
                         marshland::sbus::SbusChannelSnapshot& output) const noexcept
{
    if (frame == nullptr || size != config_.frame_bytes || frame[0] != 0x0FU) {
        return false;
    }
    marshland::sbus::SbusChannelSnapshot decoded{};
    for (std::size_t channel = 0; channel < marshland::sbus::kChannelCount; ++channel) {
        const std::size_t bit = channel * 11U;
        const std::size_t index = 1U + bit / 8U;
        const unsigned shift = static_cast<unsigned>(bit % 8U);
        const std::uint32_t packed = static_cast<std::uint32_t>(frame[index]) |
            (static_cast<std::uint32_t>(frame[index + 1U]) << 8U) |
            (static_cast<std::uint32_t>(frame[index + 2U]) << 16U);
        const auto raw = static_cast<std::uint16_t>((packed >> shift) & 0x07FFU);
        if (raw < config_.channel_minimum || raw > config_.channel_maximum) {
            return false;
        }
        decoded.raw_channels[channel] = raw;
        decoded.scaled_channels[channel] =
            (static_cast<std::int32_t>(raw) - config_.channel_center) * config_.channel_scale_x;
    }
    decoded.frame_validity = marshland::sbus::FrameValidity::VALID;
    output = decoded;
    return true;
}

} // namespace marshland::atom03
