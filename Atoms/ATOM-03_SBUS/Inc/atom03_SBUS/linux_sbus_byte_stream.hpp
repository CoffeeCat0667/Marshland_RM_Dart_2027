#pragma once

#include <cstddef>
#include <cstdint>

#include "SbusInterfaces.hpp"

namespace marshland::atom03 {

class LinuxSbusByteStream final : public marshland::sbus::IUartByteStream {
public:
    LinuxSbusByteStream() noexcept;
    ~LinuxSbusByteStream() noexcept override;

    LinuxSbusByteStream(const LinuxSbusByteStream&) = delete;
    LinuxSbusByteStream& operator=(const LinuxSbusByteStream&) = delete;

    bool configure(const marshland::sbus::SbusSerialParameters& parameters) noexcept override;
    marshland::sbus::UartReadStatus read(std::uint8_t* destination,
                                         std::size_t capacity,
                                         std::size_t& bytes_read) noexcept override;
    void cancel() noexcept override;

private:
    void closeDevice() noexcept;
    void drainCancelPipe() noexcept;

    int device_fd_{-1};
    int cancel_pipe_[2]{-1, -1};
    int read_timeout_ms_{0};
};

} // namespace marshland::atom03

