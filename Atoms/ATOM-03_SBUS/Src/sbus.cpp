#include "atom03_SBUS/sbus_private.hpp"

#include <asm/termbits.h>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace marshland::sbus {
namespace {

struct FileDescriptor final {
    int value;
    ~FileDescriptor() noexcept { if (value >= 0) ::close(value); }
};

void setOffline(SbusData& data) noexcept
{
    std::lock_guard<std::mutex> lock(data.mutex);
    data.online = false;
}

bool configurePort(int fd) noexcept
{
    termios2 settings{};
    if (::ioctl(fd, TCGETS2, &settings) != 0) return false;
    settings.c_cflag = BOTHER | CS8 | CSTOPB | PARENB | CREAD | CLOCAL;
    settings.c_iflag = settings.c_oflag = settings.c_lflag = 0;
    settings.c_ispeed = settings.c_ospeed = 100000U;
    settings.c_cc[VMIN] = settings.c_cc[VTIME] = 0;
    return ::ioctl(fd, TCSETS2, &settings) == 0 &&
           ::ioctl(fd, TCFLSH, TCIOFLUSH) == 0;
}

} // namespace

bool detail::decodeFrame(const ByteRingBuffer& frame, std::uint8_t scale_x,
                         std::uint16_t center, std::uint16_t minimum,
                         std::uint16_t maximum,
                         std::array<std::uint16_t, kChannelCount>& raw,
                         std::array<std::int32_t, kChannelCount>& scaled) noexcept
{
    for (std::size_t channel = 0; channel < raw.size(); ++channel) {
        const auto bit = channel * kChannelBits;
        const auto index = 1U + bit / 8U;
        const auto shift = bit % 8U;
        std::uint32_t packed = static_cast<std::uint32_t>(frame.at(index)) |
            (static_cast<std::uint32_t>(frame.at(index + 1U)) << 8U);
        if (shift + kChannelBits > 16U) {
            packed |= static_cast<std::uint32_t>(frame.at(index + 2U)) << 16U;
        }
        raw[channel] = static_cast<std::uint16_t>((packed >> shift) & 0x07FFU);
        if (raw[channel] < minimum || raw[channel] > maximum) return false;
        // CH7 (fire) and CH13 (feed servo) share this scaling without confirmation.
        scaled[channel] = (static_cast<std::int32_t>(raw[channel]) - center) * scale_x;
    }
    const auto force = static_cast<std::int32_t>(raw[11]) - center;
    const auto range = force < 0 ? center - minimum : maximum - center;
    scaled[11] = force * 2000 / range;
    return true;
}

bool ReceiveSbus(const char* serial_device, SbusData& data,
                 const std::atomic<bool>& stop_requested,
                 std::uint8_t channel_scale_x, std::uint16_t frame_bytes,
                 std::uint16_t channel_center, std::uint16_t channel_minimum,
                 std::uint16_t channel_maximum, std::uint32_t read_timeout_ms) noexcept
{
    setOffline(data);
    if (serial_device == nullptr || serial_device[0] == '\0' ||
        frame_bytes < detail::kMinimumFrameBytes || read_timeout_ms == 0U ||
        read_timeout_ms > INT_MAX || channel_scale_x < 1U || channel_scale_x > 10U ||
        channel_minimum >= channel_center || channel_center >= channel_maximum) {
        return false;
    }
    if (stop_requested.load()) return true;
    try {
        detail::ByteRingBuffer ring(static_cast<std::size_t>(frame_bytes) +
                                     detail::kReadBatchBytes);
        const FileDescriptor port{::open(serial_device, O_RDONLY | O_NOCTTY |
                                                        O_NONBLOCK | O_CLOEXEC)};
        if (port.value < 0 || !configurePort(port.value)) return false;
        std::array<std::uint8_t, detail::kReadBatchBytes> input{};
        std::array<std::uint16_t, kChannelCount> raw{};
        std::array<std::int32_t, kChannelCount> scaled{};
        bool success = true;
        while (!stop_requested.load()) {
            pollfd descriptor{port.value, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, static_cast<int>(read_timeout_ms));
            if (stop_requested.load()) break;
            if (ready < 0) {
                const int error = errno;
                setOffline(data);
                if (error == EINTR) continue;
                success = false;
                break;
            }
            if (ready == 0) {
                ring.discard(ring.size());
                setOffline(data);
                continue;
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                success = false;
                break;
            }
            if ((descriptor.revents & POLLIN) == 0) continue;
            const auto count = ::read(port.value, input.data(), input.size());
            if (count < 0) {
                const int error = errno;
                setOffline(data);
                if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) continue;
                success = false;
                break;
            }
            if (count == 0) {
                success = false;
                break;
            }
            ring.push(input.data(), static_cast<std::size_t>(count));
            while (ring.size() != 0U) {
                if (ring.at(0U) != 0x0FU) {
                    ring.discard(1U);
                    setOffline(data);
                    continue;
                }
                if (ring.size() < frame_bytes) break;
                if (detail::decodeFrame(ring, channel_scale_x, channel_center,
                                         channel_minimum, channel_maximum, raw, scaled)) {
                    std::lock_guard<std::mutex> lock(data.mutex);
                    data.scaled_channels = scaled;
                    data.online = true;
                } else {
                    setOffline(data);
                }
                ring.discard(frame_bytes);
            }
        }
        setOffline(data);
        return success;
    } catch (...) {
        setOffline(data);
        return false;
    }
}

} // namespace marshland::sbus
