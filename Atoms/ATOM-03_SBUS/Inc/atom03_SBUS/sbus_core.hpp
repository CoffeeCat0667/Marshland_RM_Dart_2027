#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include "AppInterfaces.hpp"
#include "IMonotonicClock.hpp"
#include "SbusInterfaces.hpp"
#include "sbus_config.hpp"
#include "sbus_decoder.hpp"

namespace marshland::atom03 {

class SbusCore final : public marshland::sbus::ISbusSnapshotReader {
public:
    SbusCore(marshland::sbus::IUartByteStream& uart,
             const marshland::app::IMonotonicClock& clock,
             marshland::sbus::SbusChannelStore& store,
             marshland::app::IInputReporter* input_reporter = nullptr,
             marshland::app::IControlAuthority* control_authority = nullptr) noexcept;
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
    void publishInputs(const marshland::sbus::SbusChannelSnapshot& snapshot) noexcept;
    void waitBeforeRetry() noexcept;

    marshland::sbus::IUartByteStream& uart_;
    const marshland::app::IMonotonicClock& clock_;
    marshland::sbus::SbusChannelStore& store_;
    marshland::app::IInputReporter* input_reporter_;
    marshland::app::IControlAuthority* control_authority_;
    SbusConfig config_{};
    SbusDecoder decoder_{config_};
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex state_mutex_;
    marshland::sbus::SbusChannelSnapshot state_{};
    std::mutex retry_mutex_;
    std::condition_variable retry_cv_;
    marshland::ControlSource last_reported_control_source_{marshland::ControlSource::NONE};
};

} // namespace marshland::atom03
