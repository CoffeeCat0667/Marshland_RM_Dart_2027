#pragma once

#include <cstddef>
#include <cstdint>

#include "SbusTypes.hpp"

namespace marshland::sbus {

enum class UartReadStatus : std::uint8_t {
    DATA,
    TIMEOUT,
    CANCELLED,
    TEMPORARY_ERROR,
    DEVICE_ERROR,
};

class IUartByteStream {
public:
    virtual ~IUartByteStream() = default;

    // Applies the IConfigView-provided serial device before reading.
    virtual bool configure(const SbusSerialParameters& parameters) noexcept = 0;

    virtual UartReadStatus read(std::uint8_t* destination, std::size_t capacity,
                                std::size_t& bytes_read) noexcept = 0;

    // Must interrupt a blocked read and make a subsequent read fail or return.
    virtual void cancel() noexcept = 0;
};

class ISbusSnapshotReader {
public:
    virtual ~ISbusSnapshotReader() = default;
    virtual SbusChannelSnapshot readSbusSnapshot() const noexcept = 0;
};

} // namespace marshland::sbus
