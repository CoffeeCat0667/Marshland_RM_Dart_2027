#pragma once

#include <cstddef>
#include <cstdint>

#include "SbusTypes.hpp"

namespace marshland::sbus {

class IUartByteStream {
public:
    virtual ~IUartByteStream() = default;

    // Applies the IConfigView-provided serial device before reading.
    virtual bool configure(const SbusSerialParameters& parameters) noexcept = 0;

    // Returns false on transport failure. A successful call may return zero bytes.
    virtual bool read(std::uint8_t* destination, std::size_t capacity,
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
