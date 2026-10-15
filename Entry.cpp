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
#include "atom04_can/can_core.hpp"
#include "atom04_can/socketcan_transport.hpp"
#include "atom04_can/lifecycle_adapter.hpp"
#include "CanTypes.hpp"
#include "atom05_motor_pid/motor_core.hpp"
#include "atom05_motor_pid/motor_interfaces.hpp"
#include "atom05_motor_pid/lifecycle_adapter.hpp"
#include "atom07_servo_driver/servo_core.hpp"
#include "atom07_servo_driver/linux_i2c_bus.hpp"
#include "atom07_servo_driver/lifecycle_adapter.hpp"
#include "atom08_charge/charge_controller.hpp"
#include "atom08_charge/charge_interfaces.hpp"
#include "atom08_charge/lifecycle_adapter.hpp"
#include "atom09_force/force_controller.hpp"
#include "atom09_force/force_interfaces.hpp"
#include "ForceTypes.hpp"
#include "ForceInterfaces.hpp"
#include "atom10_reload_feed/reload_feed_core.hpp"
#include "atom10_reload_feed/reload_feed_interfaces.hpp"
#include "atom10_reload_feed/lifecycle_adapter.hpp"
#include "atom11_fire/fire_action.hpp"
#include "atom11_fire/lifecycle_adapter.hpp"

// Temporary stub implementations for ATOM-05 dependencies
// TODO: Replace with actual implementations from ATOM-14 (safety) and assembly layer
namespace {

class StubMotorSafetyGate final : public marshland::atom05::IMotorSafetyGate {
public:
    marshland::atom05::MotorPermit checkCommand(const marshland::atom05::MotorCommand&) noexcept override {
        return {false, 0};  // Deny all commands until safety module is implemented
    }
    marshland::atom05::MotorPermit checkOutput(const marshland::atom05::MotorCommand&) noexcept override {
        return {false, 0};
    }
    marshland::atom05::PidEditPermit checkPidEdit(const marshland::atom05::PidUpdateRequest&) noexcept override {
        return {false, marshland::ControlSource::NONE, marshland::SystemMode::UNINITIALIZED};
    }
    bool allowFaultReset(std::uint8_t) noexcept override {
        return false;
    }
};

class StubMotorCurrentOutput final : public marshland::atom05::IMotorCurrentOutput {
public:
    explicit StubMotorCurrentOutput(marshland::atom04::CanCore& can_core) noexcept
        : can_core_(can_core) {}

    bool ready() const noexcept override {
        return can_core_.isRunning();
    }

    bool commit(const marshland::atom05::MotorOutputBatch& batch) noexcept override {
        // Forward motor currents to ATOM-04
        if (!ready()) {
            return false;
        }

        // Commit 0x200 frame (motors 1-4, array index 0)
        if (!can_core_.latchMotorCurrents(marshland::can::kMotorControlFrameIdA, batch.currents[0])) {
            return false;
        }

        // Commit 0x1FF frame (motors 5-6, array index 1)
        return can_core_.latchMotorCurrents(marshland::can::kMotorControlFrameIdB, batch.currents[1]);
    }

    bool forceZero() noexcept override {
        can_core_.forceZeroCurrents();
        return true;
    }

private:
    marshland::atom04::CanCore& can_core_;
};

class StubMotorFaultSink final : public marshland::atom05::IMotorFaultSink {
public:
    void onMotorFault(const marshland::atom05::FaultEvent& event) noexcept override {
        std::fprintf(stderr, "[marshland] Motor fault: motor=%d fault=%d\n",
                     event.motor_id, static_cast<int>(event.fault));
    }

    void onReloadStopRequested(std::uint8_t failed_motor, std::uint8_t peer_motor) noexcept override {
        std::fprintf(stderr, "[marshland] Reload stop requested: failed=%d peer=%d\n",
                     failed_motor, peer_motor);
    }
};

class StubPidPersistence final : public marshland::atom05::IPidPersistence {
public:
    explicit StubPidPersistence(marshland::atom15::ConfigStore& config) noexcept
        : config_(config) {}

    marshland::atom05::PersistenceStatus saveMotorPid(
        std::uint8_t motor_id, std::uint64_t,
        const marshland::atom05::PidConfig&,
        const marshland::atom05::PersistenceLocation&) noexcept override {
        // TODO: Implement actual JSON persistence via ConfigStore
        std::fprintf(stderr, "[marshland] PID persistence stub: motor=%d (not saved)\n", motor_id);
        return marshland::atom05::PersistenceStatus::UNAVAILABLE;
    }

private:
    marshland::atom15::ConfigStore& config_;
};

// Forwarding frame sink to break circular dependency between ATOM-04 and ATOM-05
class ForwardingFrameSink final : public marshland::can::ICanFrameSink {
public:
    void onCanFrame(const marshland::can::CanFrame& frame) noexcept override {
        if (target_) {
            target_->onCanFrame(frame);
        }
    }

    void setTarget(marshland::can::ICanFrameSink* target) noexcept {
        target_ = target;
    }

private:
    marshland::can::ICanFrameSink* target_{nullptr};
};

// Stub implementations for ATOM-08 (Charge) dependencies
class StubChargeMotorCommandSink final : public marshland::atom08::IMotorCommandSink {
public:
    explicit StubChargeMotorCommandSink(marshland::atom05::MotorCore& motor_core) noexcept
        : motor_core_(motor_core) {}

    bool submitVelocityTarget(std::uint8_t motor_id, std::int32_t rpm) noexcept override {
        // Bridge to ATOM-05
        // TODO: Implement actual motor command submission
        std::fprintf(stderr, "[marshland] Charge motor command stub: motor=%d rpm=%d\n", motor_id, rpm);
        return false;  // Stub always fails until ATOM-05 command interface is ready
    }

    bool clearVelocityTarget(std::uint8_t motor_id) noexcept override {
        std::fprintf(stderr, "[marshland] Charge motor clear stub: motor=%d\n", motor_id);
        return false;
    }

private:
    marshland::atom05::MotorCore& motor_core_;
};

class StubLimitSwitchState final : public marshland::atom08::ILimitSwitchState {
public:
    bool readChargeLimit(std::uint8_t motor_id, marshland::atom08::LimitSwitchSample& out) const noexcept override {
        // TODO: Connect to ATOM-06 when it's implemented
        out.valid = false;
        out.low = false;
        return false;  // Report not ready until ATOM-06 exists
    }
};

class StubChargeFaultReporter final : public marshland::atom08::IFaultReporter {
public:
    void reportChargeFault(marshland::atom08::ChargeFaultCode code,
                          marshland::FaultSeverity severity) noexcept override {
        std::fprintf(stderr, "[marshland] Charge fault: code=%d severity=%d\n",
                     static_cast<int>(code), static_cast<int>(severity));
    }
};

// Stub implementations for ATOM-09 (Force) dependencies
class StubForceMotorCommandSink final : public marshland::atom09::IMotorCommandSink {
public:
    explicit StubForceMotorCommandSink(marshland::atom05::MotorCore& motor_core) noexcept
        : motor_core_(motor_core) {}

    bool submitVelocityTarget(int motor_id, std::int32_t rpm) noexcept override {
        std::fprintf(stderr, "[marshland] Force motor command stub: motor=%d rpm=%d\n", motor_id, rpm);
        return false;
    }

    bool stopMotor(int motor_id) noexcept override {
        std::fprintf(stderr, "[marshland] Force motor stop stub: motor=%d\n", motor_id);
        return false;
    }

    bool readFeedback(int motor_id, marshland::force::ForceMotorFeedback& out) noexcept override {
        out.valid = false;
        out.has_received = false;
        out.rpm = 0;
        out.timestamp_us = 0;
        return false;
    }

    bool faultLatched(int motor_id) noexcept override {
        return false;
    }

private:
    marshland::atom05::MotorCore& motor_core_;
};

class StubForceFaultSink final : public marshland::atom09::IForceFaultSink {
public:
    void onForceActionFailed(const char* detail) noexcept override {
        std::fprintf(stderr, "[marshland] Force action failed: %s\n", detail);
    }
};

// Stub implementations for ATOM-10 (Reload/Feed) dependencies
class StubReloadMotorPair final : public marshland::atom10::IReloadMotorPair {
public:
    explicit StubReloadMotorPair(marshland::atom05::MotorCore& motor_core) noexcept
        : motor_core_(motor_core) {}

    bool submitVelocityRpm(marshland::atom10::ReloadSide side, int rpm) noexcept override {
        std::fprintf(stderr, "[marshland] Reload motor stub: side=%d rpm=%d\n", static_cast<int>(side), rpm);
        return false;
    }

    bool requestZeroTorque(marshland::atom10::ReloadSide side) noexcept override {
        std::fprintf(stderr, "[marshland] Reload zero torque stub: side=%d\n", static_cast<int>(side));
        return false;
    }

    bool isSideUsable(marshland::atom10::ReloadSide side) const noexcept override {
        return false;  // Report not usable until ATOM-05 bridge is ready
    }

private:
    marshland::atom05::MotorCore& motor_core_;
};

class StubFeedActuator final : public marshland::atom10::IFeedActuator {
public:
    explicit StubFeedActuator(marshland::atom07::ServoCore& servo_core) noexcept
        : servo_core_(servo_core) {}

    bool moveToReloadPosition() noexcept override {
        std::fprintf(stderr, "[marshland] Feed servo stub: move to reload position\n");
        return false;
    }

    bool moveToStandbyPosition() noexcept override {
        std::fprintf(stderr, "[marshland] Feed servo stub: move to standby position\n");
        return false;
    }

private:
    marshland::atom07::ServoCore& servo_core_;
};

} // anonymous namespace

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
    // Single source of truth: Config/dart_control.nlohmann
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

    // ATOM-04 and ATOM-05 have circular dependency (motor needs CAN output, CAN needs motor frame sink)
    // Solution: Create forwarding frame sink, wire everything, then connect the forward target

    // Stub implementations for ATOM-05 dependencies
    StubMotorSafetyGate motor_safety_gate;
    StubMotorFaultSink motor_fault_sink;
    StubPidPersistence motor_pid_persistence(config_store);

    // Forwarding sink to break circular dependency
    ForwardingFrameSink forwarding_frame_sink;

    // ATOM-04: CAN objects
    marshland::can::CanNodeStatusStore can_node_store;
    marshland::atom04::LinuxSocketCanTransport can_transport;

    marshland::atom04::CanCore::Dependencies can_deps{
        can_transport,
        can_node_store,
        forwarding_frame_sink,  // Will forward to motor_core once it's created
        nullptr,                // event_sink - optional
        nullptr                 // safety_notifier - optional
    };
    marshland::atom04::CanCore can_core(can_deps);

    // Motor output adapter (depends on can_core)
    StubMotorCurrentOutput motor_current_output(can_core);

    // ATOM-05: Motor objects
    marshland::atom05::MotorCore::Dependencies motor_deps{
        clock,
        motor_safety_gate,
        motor_current_output,
        motor_fault_sink,
        motor_pid_persistence
    };
    marshland::atom05::MotorCore motor_core(motor_deps);

    // Wire the forwarding sink to motor_core
    forwarding_frame_sink.setTarget(&motor_core);

    // ATOM-07: Servo objects
    marshland::atom07::LinuxI2cBus servo_i2c_bus;
    marshland::atom07::ServoCore servo_core(
        servo_i2c_bus,
        clock,
        nullptr,  // IInputReporter* - optional fault reporting
        nullptr   // IStateReader* - optional safety gate
    );

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

    // Register ATOM-04 to InitializeSocketCan step
    auto can_adapter = std::make_unique<marshland::atom04::LifecycleAdapter>(
        can_core, config_store);

    const bool can_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeSocketCan,
        true,  // required: CAN is critical hardware
        std::move(can_adapter)
    });

    if (!can_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-04 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Register ATOM-05 Stage 1: Load PID parameters
    auto motor_pid_adapter = std::make_unique<marshland::atom05::LifecycleAdapter>(
        motor_core, config_store, marshland::atom05::LifecycleStage::LOAD_PID);

    const bool pid_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::LoadPidParameters,
        true,  // required: PID parameters are critical
        std::move(motor_pid_adapter)
    });

    if (!pid_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-05 PID load registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Register ATOM-05 Stage 2: Initialize motor channels
    auto motor_init_adapter = std::make_unique<marshland::atom05::LifecycleAdapter>(
        motor_core, config_store, marshland::atom05::LifecycleStage::INITIALIZE_CHANNELS);

    const bool motor_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeMotorAndEncoderChannels,
        true,  // required: motor channels are critical
        std::move(motor_init_adapter)
    });

    if (!motor_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-05 channel init registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Register ATOM-07 to InitializeIicPca9685 step
    auto servo_adapter = std::make_unique<marshland::atom07::LifecycleAdapter>(
        servo_core, config_store);

    const bool servo_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeIicPca9685,
        true,  // required: servo is critical hardware
        std::move(servo_adapter)
    });

    if (!servo_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-07 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // ★ Create AppCoordinator early (before business-layer modules)
    // This allows ATOM-08/09/10/11 to inject IControlAuthority and IStateReader
    AppCoordinator app{runtime, arbitrator, clock};

    // ===== Business-layer modules (ATOM-08/09/10/11) =====
    // These modules need AppCoordinator's interfaces, so they are constructed after app

    // ATOM-08: Charge controller
    StubChargeMotorCommandSink charge_motor_sink(motor_core);
    StubLimitSwitchState charge_limit_switches;
    StubChargeFaultReporter charge_fault_reporter;

    marshland::atom08::ChargeController charge_controller(
        config_store,           // IConfigView
        clock,                  // IMonotonicClock
        charge_motor_sink,      // IMotorCommandSink
        charge_limit_switches,  // ILimitSwitchState
        app,                    // IControlAuthority (from AppCoordinator)
        app,                    // IStateReader (from AppCoordinator)
        sbus_core,              // ISbusSnapshotReader
        charge_fault_reporter   // IFaultReporter
    );

    // ATOM-10: Reload/Feed core
    StubReloadMotorPair reload_motor_pair(motor_core);
    StubFeedActuator feed_actuator(servo_core);

    marshland::atom10::ReloadFeedCore::Dependencies reload_deps{
        reload_motor_pair,      // IReloadMotorPair
        feed_actuator,          // IFeedActuator
        clock                   // IMonotonicClock
    };

    marshland::atom10::ReloadFeedConfig reload_feed_config{};
    marshland::atom10::ReloadFeedCore reload_feed_core(reload_deps, reload_feed_config);

    // Bind AppCoordinator interfaces after construction (late binding per ATOM10ConnectProject.md §5)
    reload_feed_core.bindControlAuthority(app);
    reload_feed_core.bindFaultReporter(&app);  // Optional but we provide it

    // ATOM-09: Force controller (no lifecycle registration)
    StubForceMotorCommandSink force_motor_sink(motor_core);
    StubForceFaultSink force_fault_sink;

    marshland::atom09::ForceController force_controller(
        config_store,           // IConfigView
        clock,                  // IMonotonicClock
        sbus_core,              // ISbusSnapshotReader
        app,                    // IStateReader (from AppCoordinator)
        force_motor_sink,       // IMotorCommandSink
        &force_fault_sink       // IForceFaultSink*
    );

    // Check ATOM-09 initialization (no lifecycle registration)
    if (!force_controller.ready()) {
        const char* error = force_controller.initializationError();
        std::fprintf(stderr, "[marshland] ATOM-09 Force initialization failed: %s\n",
                     error ? error : "unknown error");
        // Continue anyway - force mechanism can degrade without blocking startup
    }

    // Register ATOM-08 (Charge) lifecycle adapter
    auto charge_adapter = std::make_unique<marshland::atom08::LifecycleAdapter>(
        charge_controller, config_store);

    const bool charge_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeServoOutputs,
        false,  // required: charge can fail without blocking startup
        std::move(charge_adapter)
    });

    if (!charge_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-08 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Register ATOM-10 (Reload/Feed) lifecycle adapter
    auto reload_feed_adapter = std::make_unique<marshland::atom10::ReloadFeedLifecycleAdapter>(
        reload_feed_core, config_store);

    const bool reload_feed_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeServoOutputs,
        false,  // required: reload/feed can degrade without blocking startup
        std::move(reload_feed_adapter)
    });

    if (!reload_feed_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-10 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Register ATOM-11 (Fire) lifecycle adapter
    auto fire_adapter = std::make_unique<marshland::atom11::LifecycleAdapter>(
        config_store,           // IConfigView
        app,                    // IStateReader (from AppCoordinator)
        app,                    // IControlAuthority (from AppCoordinator)
        servo_core,             // ServoCore (direct ATOM-07 dependency)
        nullptr                 // IFireEventOutlet* (optional)
    );

    const bool fire_registered = runtime.addModule({
        marshland::runtime::RuntimeStep::InitializeServoOutputs,
        true,  // required: fire mechanism is critical
        std::move(fire_adapter)
    });

    if (!fire_registered) {
        const auto error = runtime.lastRegistrationError();
        std::fprintf(stderr, "[marshland] ATOM-11 registration failed: %s\n",
                     marshland::runtime::toString(error));
        return 1;
    }

    // Remaining lifecycle adapters (ATOM-06, 12..14, 16..19) are registered here once their
    // implementations exist, following the same pattern.

    // Start the application (all modules must be registered before this point)
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
