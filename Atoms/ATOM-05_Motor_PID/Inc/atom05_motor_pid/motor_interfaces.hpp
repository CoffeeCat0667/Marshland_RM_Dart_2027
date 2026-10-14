#pragma once

#include "motor_types.hpp"

namespace marshland::atom05 {

struct MotorPermit final {
    bool allowed{false};
    std::int32_t output_limit_ma{0};
};
struct PidEditPermit final {
    bool allowed{false};
    ControlSource source{ControlSource::NONE};
    SystemMode mode{SystemMode::UNINITIALIZED};
};

// Bounded/nonblocking checks; both ATOM-02 and ATOM-14 must approve.
// Implementations must not call MotorCore while a check is in progress.
class IMotorSafetyGate {
public:
    virtual ~IMotorSafetyGate() = default;
    virtual MotorPermit checkCommand(const MotorCommand& command) noexcept = 0;
    virtual MotorPermit checkOutput(const MotorCommand& command) noexcept = 0;
    virtual PidEditPermit checkPidEdit(const PidUpdateRequest& request) noexcept = 0;
    virtual bool allowFaultReset(std::uint8_t motor_id) noexcept = 0;
};

// An assembly bridge forwards both frame groups to ATOM-04's current latch.
// Success means LATCHED, not transmitted. Reject obsolete generations, honor
// external safety overrides, and leave BOTH groups zero on failure. An active
// batch rejected by a safety override must return false, not silently succeed.
// No I/O or MotorCore callbacks are allowed inside commit()/forceZero().
class IMotorCurrentOutput {
public:
    virtual ~IMotorCurrentOutput() = default;
    virtual bool ready() const noexcept = 0;
    virtual bool commit(const MotorOutputBatch& batch) noexcept = 0;
    // Clears BOTH ATOM-04 latches without relying on a usable generation.
    virtual bool forceZero() noexcept = 0;
};

// Runs outside the control lock on the request thread, never on the PID worker.
// Save only this motor's PID and preserve unrelated configuration fields.
// Do not invoke lifecycle/updatePid from this callback; edits are serialized.
class IPidPersistence {
public:
    virtual ~IPidPersistence() = default;
    virtual PersistenceStatus saveMotorPid(std::uint8_t motor_id,
        std::uint64_t revision, const PidConfig& parameters,
        const PersistenceLocation& location) noexcept = 0;
};

// Runs after the zero-current commit attempt, outside the control lock.
// Must be bounded and thread-safe. May query/stop channels, but must not invoke
// lifecycle operations, updatePid(), or destroy the core from a callback.
class IMotorFaultSink {
public:
    virtual ~IMotorFaultSink() = default;
    virtual void onMotorFault(const FaultEvent& event) noexcept = 0;
    virtual void onReloadStopRequested(std::uint8_t failed_motor,
                                      std::uint8_t peer_motor) noexcept = 0;
};

} // namespace marshland::atom05
