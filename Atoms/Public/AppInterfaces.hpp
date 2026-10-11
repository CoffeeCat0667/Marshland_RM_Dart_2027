#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

#include "StateArbitrationTypes.hpp"

namespace marshland::app {

enum class RunGateReason : std::uint8_t {
    ALLOWED,
    HARDWARE_NOT_READY,
    ESTOP_LATCHED,
    FAULT_ACTIVE,
    MODE_NOT_OPERATIONAL,
    NOT_INITIALIZED
};

inline const char* ToString(RunGateReason reason) noexcept
{
    switch (reason) {
    case RunGateReason::ALLOWED: return "ALLOWED";
    case RunGateReason::HARDWARE_NOT_READY: return "HARDWARE_NOT_READY";
    case RunGateReason::ESTOP_LATCHED: return "ESTOP_LATCHED";
    case RunGateReason::FAULT_ACTIVE: return "FAULT_ACTIVE";
    case RunGateReason::MODE_NOT_OPERATIONAL: return "MODE_NOT_OPERATIONAL";
    case RunGateReason::NOT_INITIALIZED: return "NOT_INITIALIZED";
    }
    return "UNKNOWN";
}

class AppCoordinator;

// Borrowed state remains valid only while this read lease owns the lock.
class StateReadView final {
public:
    StateReadView(const StateReadView&) = delete;
    StateReadView& operator=(const StateReadView&) = delete;
    StateReadView& operator=(StateReadView&&) = delete;

    StateReadView(StateReadView&& other) noexcept
        : lock_(std::move(other.lock_))
        , context_(std::exchange(other.context_, nullptr))
        , gate_(other.gate_)
        , hardware_ready_(other.hardware_ready_)
        , estop_latched_(other.estop_latched_)
        , run_id_(other.run_id_)
        , current_shot_(other.current_shot_)
    {
    }

    const state_arbitration::ArbitrationContext& context() const noexcept
    {
        assert(lock_.owns_lock() && context_ != nullptr);
        return *context_;
    }

    const state_arbitration::ExternalInputs& inputs() const noexcept
    {
        return context().inputs;
    }

    RunGateReason run_gate() const noexcept { return gate_; }
    bool hardware_ready() const noexcept { return hardware_ready_; }
    bool estop_latched() const noexcept { return estop_latched_; }
    std::uint64_t run_id() const noexcept { return run_id_; }
    std::uint32_t current_shot() const noexcept { return current_shot_; }

private:
    friend class AppCoordinator;

    StateReadView(std::unique_lock<std::mutex> lock,
                  const state_arbitration::ArbitrationContext& context,
                  RunGateReason gate, bool hardware_ready, bool estop_latched,
                  std::uint64_t run_id, std::uint32_t current_shot) noexcept
        : lock_(std::move(lock))
        , context_(&context)
        , gate_(gate)
        , hardware_ready_(hardware_ready)
        , estop_latched_(estop_latched)
        , run_id_(run_id)
        , current_shot_(current_shot)
    {
    }

    std::unique_lock<std::mutex> lock_;
    const state_arbitration::ArbitrationContext* context_;
    RunGateReason gate_;
    bool hardware_ready_;
    bool estop_latched_;
    std::uint64_t run_id_;
    std::uint32_t current_shot_;
};

class IInputReporter {
public:
    virtual ~IInputReporter() = default;
    virtual void ReportSbus(bool online, bool auto_mode) noexcept = 0;
    virtual void ReportWebOnline(bool online) noexcept = 0;
    virtual void ReportYawArrived(bool arrived) noexcept = 0;
    virtual void ReportStopFire(bool set) noexcept = 0;
    virtual void ReportFault(bool active, FaultSeverity severity) noexcept = 0;
    virtual void ReportSoftwareEstop(bool active) noexcept = 0;
    virtual void ReportClock() noexcept = 0;
};

class IControlAuthority {
public:
    virtual ~IControlAuthority() = default;
    virtual state_arbitration::IntentDecision SubmitIntent(
        const state_arbitration::ControlIntent& intent) noexcept = 0;
    virtual state_arbitration::IntentDecision SubmitStopFireIntent(
        const state_arbitration::ControlIntent& intent) noexcept = 0;
    virtual state_arbitration::ModeTransitionResult RequestModeTransition(
        SystemMode target, state_arbitration::TransitionReason reason) noexcept = 0;
    virtual state_arbitration::AutoFireTransitionResult RequestAutoFireState(
        AutoFireState target) noexcept = 0;
    virtual state_arbitration::AutoFireTransitionResult BeginNewAutoFireRun() noexcept = 0;
    virtual state_arbitration::AutoFireTransitionResult TerminateAutoFireOnSbusTakeover() noexcept = 0;
    virtual state_arbitration::FireAuthorization AuthorizeFire(ControlSource source) noexcept = 0;
    virtual state_arbitration::FireAuthorization AuthorizeManualFire(ControlSource source) noexcept = 0;
};

// Inject only into the safety manager, which owns recovery orchestration.
class ISafetyStateControl {
public:
    virtual ~ISafetyStateControl() = default;
    virtual state_arbitration::ModeTransitionResult ConfirmSoftwareEstop() noexcept = 0;
    virtual bool ReleaseSoftwareEstopLatch() noexcept = 0;
};

class IStateReader {
public:
    virtual ~IStateReader() = default;
    // No port calls or I/O while locked; references must not outlive the lease.
    virtual StateReadView ReadState() const = 0;
    virtual std::size_t CopyEvents(state_arbitration::StateEvent* output,
                                   std::size_t capacity) const noexcept = 0;
    virtual std::size_t dropped_event_count() const noexcept = 0;
};

} // namespace marshland::app
