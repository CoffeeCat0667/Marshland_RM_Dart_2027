// Entry.cpp
// Marshland -- RoboMaster 2027 dart system gimbal control program.
//
// This file provides the process entry point and performs ONLY object-graph
// assembly: it constructs the state arbitrator, the runtime coordinator, the
// monotonic clock and the AppCoordinator bridge, wires their references
// together, starts the system, and reports the outcome.
//
// Deliberately NOT implemented here (see AGENTS.md: this project is the gateway
// and module assembler; implementing Design/Atomic functionality is forbidden):
//   * the startup sequence itself              -> ATOM-01 RuntimeCoordinator::start()
//   * system mode / control arbitration        -> ATOM-02 StateArbitrator
//   * hardware and service initialisation      -> ATOM-03..ATOM-19 adapters,
//                                                 registered as ILifecycleModule
//   * HTTP / WebSocket / PEF flow / recovery   -> the corresponding ATOMs
//
// ─────────────────────────────────────────────────────────────────────────────
// IMPORTANT — current readiness state
//
// ATOM-03..ATOM-19 have no implementation yet, so there is nothing to register.
// RuntimeCoordinator::start() iterates over an EMPTY module list, performs zero
// steps, and therefore reports RuntimeState::Standby with succeeded() == true.
// Mapping that straight to SystemMode::STANDBY would open the run gate without
// a single device having been initialised — a SILENT FALSE-READY, not a crash.
//
// That hazard is defended against inside AppCoordinator::Start(), which refuses
// to start when the registry is empty (see its header comment). This file does
// not duplicate the check: the failure simply surfaces here as
// kRuntimeStartFailed with an explanatory reason.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <memory>

#include "AppCoordinator.hpp"
#include "RuntimeCoordinator.hpp"
#include "StateArbitration.hpp"
#include "SystemMonotonicClock.hpp"

int main()
{
    using marshland::app::AppCoordinator;
    using marshland::app::SystemMonotonicClock;
    using marshland::runtime::RuntimeCoordinator;
    using marshland::state_arbitration::StateArbitrator;

    // ---------------------------------------------------------------------
    // 1) Assemble the object graph.
    //
    // Ownership/order requirement: RuntimeCoordinator and StateArbitrator must
    // outlive AppCoordinator, which holds references to both. Declare them
    // first so they are destroyed last.
    // ---------------------------------------------------------------------
    StateArbitrator arbitrator;
    SystemMonotonicClock clock;

    marshland::runtime::RuntimeContext context{};
    context.configuration_path = "./Config/dart_control.json";

    RuntimeCoordinator runtime{context};

    // ---------------------------------------------------------------------
    // 2) Register lifecycle modules (ATOM-03..ATOM-19 adapters).
    //
    // Each adapter implements marshland::runtime::ILifecycleModule. Register
    // one entry per (RuntimeStep, adapter) pair: a step accepts N modules, and
    // modules within the same step run in registration order (FIFO).
    //
    // If any addModule() call fails, the reason is queryable through
    // RuntimeCoordinator::lastRegistrationError().
    //
    // Example (uncomment once the adapter exists):
    //
    //   runtime.addModule(marshland::runtime::ModuleRegistration{
    //       marshland::runtime::RuntimeStep::InitializeSocketCan,
    //       true,                                   // required
    //       std::make_unique<CanAdapter>()});
    //
    // Nothing is registered today: ATOM-03..ATOM-19 are still placeholders.
    // AppCoordinator::Start() detects the empty registry and refuses to start,
    // so no manual guard is needed here — see step 4.
    // ---------------------------------------------------------------------

    AppCoordinator app{runtime, arbitrator, clock};

    // ---------------------------------------------------------------------
    // 3) Start. This is the single entry into the bridge:
    //      RuntimeCoordinator::start()  -> hardware/service bring-up
    //      applyStartupOutcome()        -> SystemMode STANDBY / INIT_FAILED
    //
    // The empty-registry refusal is enforced inside AppCoordinator::Start(),
    // so it arrives here as an ordinary kRuntimeStartFailed with a reason.
    // ---------------------------------------------------------------------
    marshland::runtime::ModuleInitResult detail{};
    const int start_result = app.Start(&detail);

    if (start_result != AppCoordinator::kOk) {
        std::fprintf(stderr,
                     "[marshland] start failed: code=%d module='%s' reason='%s'\n",
                     start_result,
                     detail.module_name.c_str(),
                     detail.error_message.c_str());
        return 1;
    }

    // ---------------------------------------------------------------------
    // 4) Report the established state.
    //
    // SystemMode / ControlSource / RunGateReason all have marshland::ToString
    // overloads (declared in MarshlandTypes.hpp and AppCoordinator.hpp).
    // ---------------------------------------------------------------------
    std::printf("[marshland] started: mode=%s hardware_ready=%s run_gate=%s\n",
                marshland::ToString(arbitrator.system_mode()),
                app.hardware_ready() ? "true" : "false",
                marshland::app::ToString(app.RunGate()));

    // ---------------------------------------------------------------------
    // 5) Main loop placeholder.
    //
    // The periodic work (CAN receive/send, motor closed loop, GPIO sampling,
    // SBUS parsing, WebSocket push — all at the periods defined in Config §4)
    // belongs to the respective ATOMs running on their own threads
    // (Config §15). Entry.cpp must not implement that scheduling.
    //
    // When those threads exist, block here (for example on a signal-driven
    // condition variable) instead of returning immediately.
    // ---------------------------------------------------------------------

    app.Shutdown();
    return 0;
}
