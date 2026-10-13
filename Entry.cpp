// Process entry and object-graph assembly; business logic stays in the ATOMs.
#include <cstdio>
#include <cstring>

#include "AppCoordinator.hpp"
#include "ProcessExitSignals.hpp"
#include "RuntimeCoordinator.hpp"
#include "StateArbitration.hpp"
#include "SystemMonotonicClock.hpp"
#include "atom15_config/config_store.hpp"
#include "atom03_SBUS/linux_sbus_byte_stream.hpp"
#include "atom03_SBUS/sbus_core.hpp"
#include "atom03_SBUS/lifecycle_adapter.hpp"
#include "SbusTypes.hpp"

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

    // ATOM-15: ConfigStore must outlive RuntimeCoordinator and all adapters
    marshland::atom15::ConfigStore config_store;

    marshland::runtime::RuntimeContext context{};
    context.configuration_path = "./Config/dart_control.json";
    RuntimeCoordinator runtime{context};

    // Register ATOM-15 to LoadConfiguration step
    auto config_adapter = std::make_unique<marshland::atom15::LifecycleAdapter>(config_store);
    const bool config_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::LoadConfiguration,
        true,  // required: configuration load failure must enter INIT_FAILED
        std::move(config_adapter)
    });

    if (!config_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-15 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Transfer configuration values from ConfigStore to RuntimeContext
    // Single source of truth: Config/dart_control.json
    const char* sbus_device_data = nullptr;
    std::uint64_t sbus_device_size = 0;
    const auto sbus_device_status = config_store.getString(
        "sbus.serial_device", sbus_device_data, sbus_device_size);

    if (sbus_device_status == marshland::config::ConfigStatus::OK && sbus_device_data != nullptr) {
        context.values["sbus.serial_device"] = std::string(sbus_device_data, sbus_device_size);
    } else {
        std::fprintf(stderr, "[marshland] Failed to read sbus.serial_device from config: status=%d\n",
                     static_cast<int>(sbus_device_status));
        return 1;
    }

    // ATOM-03: SBUS objects must outlive LifecycleAdapter
    marshland::atom03::LinuxSbusByteStream sbus_uart;
    marshland::sbus::SbusChannelStore sbus_store;
    marshland::atom03::SbusCore sbus_core(sbus_uart, clock, sbus_store);

    // Register ATOM-03 to InitializeSbusAndVision step
    auto sbus_adapter = std::make_unique<marshland::atom03::LifecycleAdapter>(
        sbus_core, config_store);

    const bool sbus_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeSbusAndVision,
        false,  // required: SBUS offline can degrade to WEB, not blocking startup
        std::move(sbus_adapter)
    });

    if (!sbus_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-03 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Remaining lifecycle adapters (ATOM-04..14, 16..19) are registered here once their
    // implementations exist, following the same pattern: construct the kernel and
    // its adapter, check addModule(), and read lastRegistrationError() on failure.
    // Declare every object before AppCoordinator so that it outlives the adapters,
    // which hold references only.
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
