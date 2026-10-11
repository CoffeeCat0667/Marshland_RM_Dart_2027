#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "AppInterfaces.hpp"
#include "IMonotonicClock.hpp"
#include "RuntimeCoordinator.hpp"
#include "StateArbitration.hpp"

namespace marshland::app {

// Assembly bridge only. ATOMs depend on AppInterfaces.hpp, not this header.
// Runtime, authority and clock must outlive the bridge and all borrowed views.
// After assembly, the authority is accessed exclusively through this bridge.
class AppCoordinator final : public IInputReporter,
                             public IControlAuthority,
                             public ISafetyStateControl,
                             public IStateReader {
public:
    static constexpr int kOk = 0;
    static constexpr int kRuntimeStartFailed = -2;
    static constexpr int kAlreadyStarted = -3;
    static constexpr int kPreconditionFailed = -4;
    static constexpr int kRestartNotSupported = -5;
    static constexpr int kRegistrationInfoUnavailable = -6;

    AppCoordinator(runtime::RuntimeCoordinator& runtime,
                   state_arbitration::StateArbitrator& authority,
                   IMonotonicClock& clock) noexcept;
    ~AppCoordinator() noexcept override;

    AppCoordinator(const AppCoordinator&) = delete;
    AppCoordinator& operator=(const AppCoordinator&) = delete;
    AppCoordinator(AppCoordinator&&) = delete;
    AppCoordinator& operator=(AppCoordinator&&) = delete;

    // No in-process restart. Missing registration information fails closed.
    int Start(runtime::ModuleInitResult* detail = nullptr) noexcept;
    void Shutdown() noexcept;

    RunGateReason RunGate() const noexcept;
    bool CanRunActuators() const noexcept;
    bool hardware_ready() const noexcept;

    // Owned diagnostic copies: no reference to data being published at startup.
    runtime::RuntimeStartResult startup() const;
    std::vector<runtime::StepResult> stepResults() const;
    state_arbitration::ExternalInputs lastInputs() const noexcept;
    StateReadView ReadState() const override;
    std::size_t CopyEvents(state_arbitration::StateEvent* output,
                           std::size_t capacity) const noexcept override;
    std::size_t dropped_event_count() const noexcept override;

    void ReportSbus(bool online, bool auto_mode) noexcept override;
    void ReportWebOnline(bool online) noexcept override;
    void ReportYawArrived(bool arrived) noexcept override;
    void ReportStopFire(bool set) noexcept override;
    void ReportFault(bool active, FaultSeverity severity) noexcept override;
    void ReportSoftwareEstop(bool active) noexcept override;
    void ReportClock() noexcept override;

    state_arbitration::IntentDecision SubmitIntent(
        const state_arbitration::ControlIntent& intent) noexcept override;
    // Delegated authorization and flag commit share one critical section.
    state_arbitration::IntentDecision SubmitStopFireIntent(
        const state_arbitration::ControlIntent& intent) noexcept override;
    state_arbitration::ModeTransitionResult RequestModeTransition(
        SystemMode target, state_arbitration::TransitionReason reason) noexcept override;
    state_arbitration::AutoFireTransitionResult RequestAutoFireState(
        AutoFireState target) noexcept override;
    state_arbitration::AutoFireTransitionResult BeginNewAutoFireRun() noexcept override;
    state_arbitration::AutoFireTransitionResult TerminateAutoFireOnSbusTakeover() noexcept override;
    state_arbitration::FireAuthorization AuthorizeFire(ControlSource source) noexcept override;
    state_arbitration::FireAuthorization AuthorizeManualFire(ControlSource source) noexcept override;
    state_arbitration::ModeTransitionResult ConfirmSoftwareEstop() noexcept override;
    bool ReleaseSoftwareEstopLatch() noexcept override;

private:
    bool applyStartupOutcomeLocked(bool hardware_start_ok,
                                   runtime::ModuleInitResult* detail) noexcept;
    RunGateReason runGateLocked() const noexcept;
    void reportClockLocked() noexcept;
    void reportStopFireLocked(bool set) noexcept;

    runtime::RuntimeCoordinator& runtime_;
    state_arbitration::StateArbitrator& authority_;
    IMonotonicClock& clock_;

    // Lifecycle calls serialize independently: never hold state_mutex_ over I/O
    // or join, because initializing/stopping modules may report state back.
    std::mutex lifecycle_mutex_;
    mutable std::mutex state_mutex_;
    runtime::RuntimeStartResult startup_{};
    std::uint64_t report_sequence_{0};
    bool hardware_ready_{false};
    bool started_{false};
    bool start_attempted_{false};
    bool shutdown_requested_{false};
};

} // namespace marshland::app
