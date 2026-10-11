// Process entry and object-graph assembly; business logic stays in the ATOMs.
#include <cstdio>
#include <cstring>

#include "AppCoordinator.hpp"
#include "ProcessExitSignals.hpp"
#include "RuntimeCoordinator.hpp"
#include "StateArbitration.hpp"
#include "SystemMonotonicClock.hpp"

int main()
{
    using marshland::app::AppCoordinator;
    using marshland::app::ProcessExitSignals;
    using marshland::app::SystemMonotonicClock;
    using marshland::runtime::RuntimeCoordinator;
    using marshland::state_arbitration::StateArbitrator;

    // Prepare before any lifecycle module can create a worker thread.
    ProcessExitSignals exit_signals;
    const int signal_setup = exit_signals.Prepare();
    if (signal_setup != 0) {
        std::fprintf(stderr, "[marshland] signal setup failed: %s\n", std::strerror(signal_setup));
        return 1;
    }

    StateArbitrator arbitrator;
    SystemMonotonicClock clock;
    marshland::runtime::RuntimeContext context{};
    context.configuration_path = "./Config/dart_control.json";
    RuntimeCoordinator runtime{context};

    // Register real ATOM-03..19 lifecycle adapters here, checking addModule().
    // No placeholder is registered. ATOM-01 must supply registeredModuleCount().
    // Inject the bridge through Public port types, not its concrete type.
    AppCoordinator app{runtime, arbitrator, clock};
    marshland::runtime::ModuleInitResult detail{};
    const int start_result = app.Start(&detail);
    if (start_result != AppCoordinator::kOk) {
        std::fprintf(stderr, "[marshland] start failed: code=%d module='%s' reason='%s'\n",
                     start_result, detail.module_name.c_str(), detail.error_message.c_str());
        return 1;
    }

    marshland::SystemMode mode;
    bool hardware_ready;
    marshland::app::RunGateReason gate;
    {
        const auto view = app.ReadState();
        mode = view.context().system_mode;
        hardware_ready = view.hardware_ready();
        gate = view.run_gate();
    }
    std::printf("[marshland] started: mode=%s hardware_ready=%s run_gate=%s\n",
                marshland::ToString(mode), hardware_ready ? "true" : "false",
                marshland::app::ToString(gate));

    int exit_signal = 0;
    const int wait_result = exit_signals.Wait(exit_signal);
    if (wait_result != 0) {
        std::fprintf(stderr, "[marshland] exit signal wait failed: %s\n", std::strerror(wait_result));
    }
    app.Shutdown();
    return wait_result == 0 ? 0 : 1;
}
