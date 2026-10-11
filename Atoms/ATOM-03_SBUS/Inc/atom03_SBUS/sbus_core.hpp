#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include "IMonotonicClock.hpp"
#include "SbusInterfaces.hpp"
#include "sbus_config.hpp"
#include "sbus_decoder.hpp"

namespace marshland::atom03 {

class SbusCore final : public marshland::sbus::ISbusSnapshotReader {
public:
    SbusCore(marshland::sbus::IUartByteStream& uart,
             const marshland::app::IMonotonicClock& clock,
             marshland::sbus::SbusChannelStore& store) noexcept;
    ~SbusCore() noexcept;
    SbusCore(const SbusCore&) = delete;
    SbusCore& operator=(const SbusCore&) = delete;

    bool start(const SbusConfig& config) noexcept;
    void stop() noexcept;
    marshland::sbus::SbusChannelSnapshot readSbusSnapshot() const noexcept override;

private:
    void run() noexcept;
    void processFrame(const std::uint8_t* frame) noexcept;
    void updateOfflineState(std::uint64_t now_us) noexcept;

    marshland::sbus::IUartByteStream& uart_;
    const marshland::app::IMonotonicClock& clock_;
    marshland::sbus::SbusChannelStore& store_;
    SbusConfig config_{};
    SbusDecoder decoder_{config_};
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex state_mutex_;
    marshland::sbus::SbusChannelSnapshot state_{};
};

} // namespace marshland::atom03
