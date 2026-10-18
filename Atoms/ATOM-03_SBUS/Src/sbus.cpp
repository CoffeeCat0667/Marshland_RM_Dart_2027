#include "atom03_SBUS/sbus_private.hpp"
#include "Log.hpp"

#include <algorithm>
#include <asm/termbits.h>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace marshland::sbus {
namespace {

struct FileDescriptor final {
    int value;
    ~FileDescriptor() noexcept { if (value >= 0) ::close(value); }
};

template<typename... Args>
void logMessage(log::Level level, const char* format, Args... args) noexcept
{
    char message[512];
    const int size = std::snprintf(message, sizeof(message), format, args...);
    if (size > 0) {
        const auto length = static_cast<std::size_t>(size);
        log::Log(level, std::string_view(message,
            length < sizeof(message) ? length : sizeof(message) - 1U));
    }
}

using LogTime = std::chrono::steady_clock::time_point;
constexpr auto kLossTimeout = std::chrono::milliseconds(200);

bool allowLog(LogTime& next) noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return false;
    next = now + std::chrono::seconds(1);
    return true;
}

void logBytes(const std::uint8_t* bytes, std::size_t count,
              std::size_t buffered) noexcept
{
    char hex[32U * 3U + 1U]{};
    const auto preview = count < 32U ? count : 32U;
    for (std::size_t i = 0; i < preview; ++i) {
        std::snprintf(hex + i * 3U, 4U, "%02X ", static_cast<unsigned>(bytes[i]));
    }
    logMessage(log::Level::INFO,
        "[SBUS] first UART read: bytes=%zu buffered_before=%zu hex(first %zu)=%s",
        count, buffered, preview, hex);
}

void expireOnline(SbusData& data, LogTime last_valid_frame) noexcept
{
    const auto age = std::chrono::steady_clock::now() - last_valid_frame;
    if (age < kLossTimeout) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(data.mutex);
        if (data.online) {
            data.online = false;
            changed = true;
        }
    }
    if (changed) {
        logMessage(log::Level::WARN,
            "[SBUS] online=false: no valid frame for %lld ms; loss_timeout_ms=200 data=%p",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count()),
            static_cast<void*>(&data));
    }
}

bool configurePort(int fd) noexcept
{
    termios2 settings{};
    if (::ioctl(fd, TCGETS2, &settings) != 0) {
        logMessage(log::Level::ERROR, "[SBUS] TCGETS2 failed: fd=%d errno=%d", fd, errno);
        return false;
    }
    settings.c_cflag = BOTHER | CS8 | CSTOPB | PARENB | CREAD | CLOCAL;
    settings.c_iflag = settings.c_oflag = settings.c_lflag = 0;
    settings.c_ispeed = settings.c_ospeed = 100000U;
    settings.c_cc[VMIN] = settings.c_cc[VTIME] = 0;
    if (::ioctl(fd, TCSETS2, &settings) != 0) {
        logMessage(log::Level::ERROR, "[SBUS] TCSETS2 failed (100000 8E2): fd=%d errno=%d", fd, errno);
        return false;
    }
    if (::ioctl(fd, TCFLSH, TCIOFLUSH) != 0) {
        logMessage(log::Level::ERROR, "[SBUS] TCFLSH failed: fd=%d errno=%d", fd, errno);
        return false;
    }
    return true;
}

} // namespace

bool detail::decodeFrame(const ByteRingBuffer& frame, std::uint8_t scale_x,
                         std::uint16_t center, std::uint16_t minimum,
                         std::uint16_t maximum,
                         std::array<std::uint16_t, kChannelCount>& raw,
                         std::array<std::int32_t, kChannelCount>& scaled,
                         std::size_t& rejected_channel) noexcept
{
    rejected_channel = kChannelCount;
    raw[15] = 0;
    scaled[15] = 0;
    for (std::size_t channel = 0; channel < kDecodedChannelCount; ++channel) {
        const auto bit = channel * kChannelBits;
        const auto index = 1U + bit / 8U;
        const auto shift = bit % 8U;
        std::uint32_t packed = static_cast<std::uint32_t>(frame.at(index)) |
            (static_cast<std::uint32_t>(frame.at(index + 1U)) << 8U);
        if (shift + kChannelBits > 16U) {
            packed |= static_cast<std::uint32_t>(frame.at(index + 2U)) << 16U;
        }
        raw[channel] = static_cast<std::uint16_t>((packed >> shift) & 0x07FFU);
        if (raw[channel] < minimum || raw[channel] > maximum) {
            rejected_channel = channel;
            return false;
        }
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
    auto last_valid_frame = std::chrono::steady_clock::now();
    FileDescriptor port{-1};
    const auto finish = [&](bool success) noexcept {
        if (port.value >= 0) {
            ::close(port.value);
            port.value = -1;
        }
        bool online = false;
        {
            std::lock_guard<std::mutex> lock(data.mutex);
            online = data.online;
        }
        // No receiver remains after return, so let the same watchdog expire first.
        if (online) std::this_thread::sleep_until(last_valid_frame + kLossTimeout);
        expireOnline(data, last_valid_frame);
        logMessage(success ? log::Level::INFO : log::Level::ERROR,
                   "[SBUS] receiver exited: success=%d stop_requested=%d online=false",
                   static_cast<int>(success), static_cast<int>(stop_requested.load()));
        return success;
    };
    logMessage(log::Level::INFO,
        "[SBUS] start: device=%s data=%p format=100000/8E2 x=%u frame_bytes=%u "
        "center=%u valid=[%u,%u] poll_timeout_ms=%u loss_timeout_ms=200 decoded_channels=15 CH16=ignored",
        serial_device ? serial_device : "<null>", static_cast<void*>(&data),
        static_cast<unsigned>(channel_scale_x), static_cast<unsigned>(frame_bytes),
        static_cast<unsigned>(channel_center), static_cast<unsigned>(channel_minimum),
        static_cast<unsigned>(channel_maximum), static_cast<unsigned>(read_timeout_ms));
    if (serial_device == nullptr || serial_device[0] == '\0' ||
        frame_bytes < detail::kMinimumFrameBytes || read_timeout_ms == 0U ||
        read_timeout_ms > INT_MAX || channel_scale_x < 1U || channel_scale_x > 10U ||
        channel_minimum >= channel_center || channel_center >= channel_maximum) {
        logMessage(log::Level::ERROR,
            "[SBUS] invalid parameters: check start log; minimum_frame_bytes=%zu, "
            "x=1..10, 0<timeout<=INT_MAX, minimum<center<maximum", detail::kMinimumFrameBytes);
        return finish(false);
    }
    if (stop_requested.load()) {
        log::Log(log::Level::INFO, "[SBUS] stopped before opening device: stop_requested=true");
        return finish(true);
    }
    try {
        detail::ByteRingBuffer ring(static_cast<std::size_t>(frame_bytes) +
                                     detail::kReadBatchBytes);
        port.value = ::open(serial_device, O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (port.value < 0) {
            logMessage(log::Level::ERROR, "[SBUS] open failed: device=%s errno=%d", serial_device, errno);
            return finish(false);
        }
        if (!configurePort(port.value)) return finish(false);
        logMessage(log::Level::INFO, "[SBUS] serial configured: device=%s fd=%d format=100000/8E2",
                   serial_device, port.value);
        std::array<std::uint8_t, detail::kReadBatchBytes> input{};
        std::array<std::uint16_t, kChannelCount> raw{};
        std::array<std::int32_t, kChannelCount> scaled{};
        bool success = true;
        bool first_read = true;
        LogTime timeout_log{}, header_log{}, rejected_log{}, partial_log{}, transient_log{}, online_log{};
        while (!stop_requested.load()) {
            expireOnline(data, last_valid_frame);
            const auto remaining_ms = std::chrono::ceil<std::chrono::milliseconds>(
                last_valid_frame + kLossTimeout - std::chrono::steady_clock::now()).count();
            const auto deadline_wait_ms = static_cast<std::uint32_t>(
                remaining_ms > 0 ? remaining_ms : kLossTimeout.count());
            const int wait_ms = static_cast<int>(std::min(read_timeout_ms, deadline_wait_ms));
            pollfd descriptor{port.value, POLLIN, 0};
            const int ready = ::poll(&descriptor, 1, wait_ms);
            const int poll_error = ready < 0 ? errno : 0;
            expireOnline(data, last_valid_frame);
            if (stop_requested.load()) break;
            if (ready < 0) {
                const int error = poll_error;
                if (error == EINTR) {
                    if (allowLog(transient_log)) {
                        logMessage(log::Level::WARN, "[SBUS] poll interrupted: errno=%d; last valid frame unchanged", error);
                    }
                    continue;
                }
                logMessage(log::Level::ERROR, "[SBUS] poll failed: errno=%d", error);
                success = false;
                break;
            }
            if (ready == 0) {
                if (allowLog(timeout_log)) {
                    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_valid_frame).count();
                    logMessage(log::Level::WARN,
                        "[SBUS] poll timeout: wait_ms=%d retained_partial_bytes=%zu "
                        "expected_frame_bytes=%u no_valid_frame_ms=%lld",
                        wait_ms, ring.size(), static_cast<unsigned>(frame_bytes),
                        static_cast<long long>(age_ms));
                }
                continue;
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                logMessage(log::Level::ERROR, "[SBUS] UART poll error: revents=0x%X (ERR/HUP/NVAL)",
                           static_cast<unsigned>(descriptor.revents));
                success = false;
                break;
            }
            if ((descriptor.revents & POLLIN) == 0) continue;
            const auto count = ::read(port.value, input.data(), input.size());
            if (count < 0) {
                const int error = errno;
                if (error == EINTR || error == EAGAIN || error == EWOULDBLOCK) {
                    if (allowLog(transient_log)) {
                        logMessage(log::Level::WARN, "[SBUS] temporary read failure: errno=%d; last valid frame unchanged", error);
                    }
                    continue;
                }
                logMessage(log::Level::ERROR, "[SBUS] read failed: errno=%d", error);
                success = false;
                break;
            }
            if (count == 0) {
                log::Log(log::Level::ERROR, "[SBUS] read returned zero bytes; receiver exiting");
                success = false;
                break;
            }
            if (first_read) {
                logBytes(input.data(), static_cast<std::size_t>(count), ring.size());
                first_read = false;
            }
            ring.push(input.data(), static_cast<std::size_t>(count));
            while (ring.size() != 0U) {
                if (ring.at(0U) != 0x0FU) {
                    const auto byte = ring.at(0U);
                    ring.discard(1U);
                    if (allowLog(header_log)) {
                        logMessage(log::Level::WARN,
                            "[SBUS] sync discard: byte=0x%02X expected=0x0F remaining=%zu "
                            "configured_frame_bytes=%u; last valid frame unchanged",
                            static_cast<unsigned>(byte), ring.size(), static_cast<unsigned>(frame_bytes));
                    }
                    continue;
                }
                if (ring.size() < frame_bytes) {
                    if (allowLog(partial_log)) {
                        logMessage(log::Level::DEBUG, "[SBUS] partial frame: buffered=%zu expected=%u",
                                   ring.size(), static_cast<unsigned>(frame_bytes));
                    }
                    break;
                }
                std::size_t rejected_channel = kChannelCount;
                if (detail::decodeFrame(ring, channel_scale_x, channel_center,
                                         channel_minimum, channel_maximum, raw, scaled, rejected_channel)) {
                    bool became_online = false;
                    {
                        std::lock_guard<std::mutex> lock(data.mutex);
                        became_online = !data.online;
                        data.scaled_channels = scaled;
                        last_valid_frame = std::chrono::steady_clock::now();
                        data.online = true;
                    }
                    if (became_online && allowLog(online_log)) {
                        logMessage(log::Level::INFO,
                            "[SBUS] frame accepted: online=true data=%p raw_CH7=%u raw_CH13=%u "
                            "raw_CH14_15=[%u,%u] CH16=ignored scaled_CH7=%d scaled_CH13=%d",
                            static_cast<void*>(&data), static_cast<unsigned>(raw[6]),
                            static_cast<unsigned>(raw[12]), static_cast<unsigned>(raw[13]),
                            static_cast<unsigned>(raw[14]),
                            static_cast<int>(scaled[6]), static_cast<int>(scaled[12]));
                    }
                } else {
                    if (allowLog(rejected_log)) {
                        logMessage(log::Level::WARN,
                            "[SBUS] frame rejected: CH%zu raw=%u valid=[%u,%u] "
                            "frame_bytes=%u buffered=%zu; entire frame discarded, last valid frame unchanged",
                            rejected_channel + 1U, static_cast<unsigned>(raw[rejected_channel]),
                            static_cast<unsigned>(channel_minimum), static_cast<unsigned>(channel_maximum),
                            static_cast<unsigned>(frame_bytes), ring.size());
                    }
                }
                ring.discard(frame_bytes);
            }
        }
        return finish(success);
    } catch (const std::exception& error) {
        logMessage(log::Level::ERROR, "[SBUS] exception: %s; stopping receiver", error.what());
        return finish(false);
    } catch (...) {
        log::Log(log::Level::ERROR, "[SBUS] unknown exception; stopping receiver");
        return finish(false);
    }
}

} // namespace marshland::sbus
