#pragma once

#include <vector>
#include "Sbus.hpp"

namespace marshland::sbus::detail {

inline constexpr std::size_t kChannelBits = 11U;
inline constexpr std::size_t kMinimumFrameBytes =
    1U + (kChannelCount * kChannelBits + 7U) / 8U;
inline constexpr std::size_t kReadBatchBytes = 256U;

class ByteRingBuffer final {
public:
    explicit ByteRingBuffer(std::size_t capacity) : data_(capacity) {}

    void push(const std::uint8_t* data, std::size_t count) noexcept
    {
        for (std::size_t i = 0; i < count; ++i) {
            if (size_ == data_.size()) discard(1U);
            data_[(head_ + size_) % data_.size()] = data[i];
            ++size_;
        }
    }
    std::uint8_t at(std::size_t index) const noexcept
    {
        return data_[(head_ + index) % data_.size()];
    }
    void discard(std::size_t count) noexcept
    {
        head_ = (head_ + count) % data_.size();
        size_ -= count;
    }
    std::size_t size() const noexcept { return size_; }

private:
    std::vector<std::uint8_t> data_;
    std::size_t head_{0};
    std::size_t size_{0};
};

bool decodeFrame(const ByteRingBuffer& frame, std::uint8_t scale_x,
                 std::uint16_t center, std::uint16_t minimum, std::uint16_t maximum,
                 std::array<std::uint16_t, kChannelCount>& raw,
                 std::array<std::int32_t, kChannelCount>& scaled) noexcept;

} // namespace marshland::sbus::detail
