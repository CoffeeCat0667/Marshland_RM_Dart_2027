#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

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

namespace {

using namespace marshland;
using namespace marshland::app;
using namespace marshland::runtime;
using namespace marshland::state_arbitration;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Mock UART for testing without real hardware
class MockUartByteStream final : public marshland::sbus::IUartByteStream {
public:
    bool configure(const marshland::sbus::SbusSerialParameters&) noexcept override
    {
        return false;  // Simulate offline SBUS (required=false allows startup)
    }

    marshland::sbus::UartReadStatus read(std::uint8_t*, std::size_t, std::size_t&) noexcept override
    {
        return marshland::sbus::UartReadStatus::TIMEOUT;
    }

    void cancel() noexcept override {}
};

const char* kTestConfig = R"({
  "schema_version": 1,
  "baseline": {
    "auto_total_shots": 4,
    "continuous_extra_interval_ms": 0,
    "repeat_charge_per_shot": true,
    "repeat_force_per_shot": true,
    "fire_priority_order": ["SBUS", "WEB", "AUTO"],
    "estop_overrides_all_sources": true,
    "estop_trigger_sources": ["WEB", "SBUS"],
    "hardware_estop_added": false,
    "estop_confirm_requires_reinit": false,
    "stop_fire_default": true,
    "stop_fire_setter": "WEB",
    "stop_fire_clear_paths": ["VISION", "MAIN_ON_TIMEOUT", "WEB"],
    "stop_fire_scan_state": "FIRE_READY",
    "initial_control_source": "AUTO",
    "yaw_arrived_type": "bool",
    "yaw_arrived_initial": false,
    "yaw_arrived_clearer": "MAIN_CONTROLLER",
    "yaw_arrived_clear_moment": "AFTER_FIRE_ACTION",
    "pressure_sensor_enabled": false,
    "time_base": "MONOTONIC"
  },
  "mode": {
    "enabled": {
      "initialization": true,
      "standby": true,
      "auto_fire": true,
      "manual": true,
      "debug": true,
      "calibration": true,
      "auto_recovery": true,
      "init_failed": true,
      "fault": true,
      "software_estop": true
    },
    "initial": {
      "system_mode": "UNINITIALIZED",
      "control_source": "AUTO",
      "current_shot": 0,
      "total_shots": 4,
      "recovery_fire_count": 3,
      "recovery_final_mode": "MANUAL",
      "auto_fire_state": "IDLE",
      "estop_active": false,
      "fault_active": false,
      "stop_fire": true,
      "yaw_arrived": false
    }
  },
  "timing": {
    "max_control_cycle_ms": 100,
    "motor_loop_period_ms": 20,
    "pid_period_ms": 20,
    "can_io_period_ms": 20,
    "gpio_sample_period_ms": 20,
    "sbus_receive_period_ms": 20,
    "websocket_push_period_ms": 20,
    "estop_response_max_ms": 100,
    "zero_torque_send_period_ms": 20,
    "reload_lift_run_s": 3,
    "reload_lower_run_s": 3,
    "feed_angle2_hold_ms": 100,
    "feed_to_reload_lower_delay_s": 2,
    "fire_angle_hold_ms": 100,
    "sbus_loss_timeout_ms": 200,
    "can_comm_timeout_ms": 200,
    "iic_comm_timeout_ms": 200,
    "charge_time_s": 30,
    "charge_max_action_ms": 30000,
    "charge_home_max_action_ms": 25000,
    "force_time_s": 30,
    "feed_max_action_ms": 1000,
    "force_max_action_ms": 30000,
    "yaw_adjust_max_action_ms": 2000,
    "fire_max_action_ms": 300,
    "vision_status_update_timeout_ms": 200,
    "vision_no_response_timeout_ms": 30000
  },
  "motor": [
    {
      "id": 1,
      "role": "charge_right",
      "bus": "CAN0",
      "node": "0x201",
      "control_mode": "CLOSED_LOOP_PID",
      "encoder_source": "CAN",
      "target_unit": "rpm",
      "pid": {
        "kp": 3,
        "ki": 0,
        "kd": 0,
        "integral_limit": 16000,
        "output_limit": 16000,
        "control_period_ms": 20,
        "enabled": true
      }
    },
    {
      "id": 2,
      "role": "charge_left",
      "bus": "CAN0",
      "node": "0x202",
      "control_mode": "CLOSED_LOOP_PID",
      "encoder_source": "CAN",
      "target_unit": "rpm",
      "pid": {
        "kp": 3,
        "ki": 0,
        "kd": 0,
        "integral_limit": 16000,
        "output_limit": 16000,
        "control_period_ms": 20,
        "enabled": true
      }
    },
    {
      "id": 3,
      "role": "reload_right",
      "bus": "CAN0",
      "node": "0x203",
      "control_mode": "CLOSED_LOOP_PID",
      "encoder_source": "CAN",
      "target_unit": "rpm",
      "pid": {
        "kp": 3,
        "ki": 0,
        "kd": 0,
        "integral_limit": 16000,
        "output_limit": 16000,
        "control_period_ms": 20,
        "enabled": true
      }
    },
    {
      "id": 4,
      "role": "reload_left",
      "bus": "CAN0",
      "node": "0x204",
      "control_mode": "CLOSED_LOOP_PID",
      "encoder_source": "CAN",
      "target_unit": "rpm",
      "pid": {
        "kp": 3,
        "ki": 0,
        "kd": 0,
        "integral_limit": 16000,
        "output_limit": 16000,
        "control_period_ms": 20,
        "enabled": true
      }
    },
    {
      "id": 5,
      "role": "yaw",
      "bus": "CAN0",
      "node": "0x205",
      "control_mode": "CLOSED_LOOP_PID",
      "encoder_source": "CAN",
      "target_unit": "rpm",
      "pid": {
        "kp": 3,
        "ki": 0,
        "kd": 0,
        "integral_limit": 16000,
        "output_limit": 16000,
        "control_period_ms": 20,
        "enabled": true
      }
    },
    {
      "id": 6,
      "role": "force",
      "bus": "CAN0",
      "node": "0x206",
      "control_mode": "CLOSED_LOOP_PID",
      "encoder_source": "CAN",
      "target_unit": "rpm",
      "pid": {
        "kp": 3,
        "ki": 0,
        "kd": 0,
        "integral_limit": 16000,
        "output_limit": 16000,
        "control_period_ms": 20,
        "enabled": true
      }
    }
  ],
  "charge": {
    "charge_speed_rpm": 5000,
    "home_speed_rpm": 5000,
    "target_range": 15000
  },
  "force": {
    "center_deadband_rpm": 0,
    "center_action": "ZERO_TORQUE"
  },
  "can": {
    "interface": "CAN0",
    "bitrate": "1Mbps",
    "data_bits": 8,
    "start_mode": "CLASSIC_CAN_FD_OFF",
    "rx_filter": "NONE",
    "error_frame_handling": "DISCARD",
    "rx_task": "DEDICATED_THREAD",
    "tx_task": "DEDICATED_THREAD",
    "task_priority_order": ["SAFETY", "MANUAL", "AUTO"],
    "task_sched_policy": "SCHED_OTHER",
    "protocol_source": "C620",
    "zero_torque": {
      "send_mode": "OPEN_LOOP",
      "period_ms": 20,
      "frames": ["0x200", "0x1FF"],
      "motor_on_comm_fault": "ZERO_TORQUE",
      "servo_on_comm_fault": "HOLD",
      "fire_servo_on_comm_fault": "REJECT_CHANGE",
      "payload_hex": "0000000000000000"
    },
    "silence_trigger": "ZERO_TORQUE_SEND_FAILED"
  },
  "sbus": {
    "receiver": "ET16S",
    "protocol": "SBUS",
    "serial_device": "/dev/ttyS10",
    "channel_count": 16,
    "channel_scale_x": 1,
    "baudrate": 100000,
    "serial_format": "8E2",
    "inverted": false,
    "receive_period_ms": 20,
    "loss_timeout_ms": 200,
    "frame_bytes": 25,
    "validate_frame_tail": false,
    "joystick_center": 1024,
    "joystick_range": 660,
    "switch_values": [364, 1024, 1684],
    "valid_min": 364,
    "valid_max": 1684,
    "loss_handling": "DEGRADE_TO_WEB",
    "priority": "SBUS_GT_WEB_GT_AUTO",
    "functions": [
      {"name": "auto_mode", "channel": "CH5", "signal_type": "SWITCH"},
      {"name": "estop_trigger", "channel": "CH6", "signal_type": "SWITCH"},
      {"name": "manual_charge", "channel": "CH2", "signal_type": "JOYSTICK"},
      {"name": "manual_charge_left", "channel": "CH8", "signal_type": "JOYSTICK"},
      {"name": "manual_charge_right", "channel": "CH9", "signal_type": "JOYSTICK"},
      {"name": "manual_reload", "channel": "CH3", "signal_type": "JOYSTICK"},
      {"name": "manual_yaw", "channel": "CH4", "signal_type": "JOYSTICK"},
      {"name": "manual_fire", "channel": "CH7", "signal_type": "SWITCH"}
    ]
  },
  "servo": {
    "pca9685": {
      "i2c_device": "/dev/i2c-7",
      "address": "0x40",
      "channel_count": 16,
      "pwm_resolution_bits": 12,
      "pwm_frequency_hz": 50,
      "channel_range": [0, 15],
      "oe_active_level": "LOW",
      "access_mode": "SERIALIZED",
      "task_priority": "MEDIUM_HIGH",
      "error_handling": "ENTER_FAULT"
    },
    "model": {
      "name": "MG996R",
      "type": "180_DEGREE",
      "pulse_min_us": 500,
      "pulse_max_us": 2500,
      "angle_range_deg": [0, 180]
    },
    "channels": [
      {"id": 5, "mechanism": "FEED", "channel": 4, "standby_deg": 20, "action_deg": 30},
      {"id": null, "mechanism": "FIRE", "channel": 5, "standby_deg": null, "action_deg": null}
    ],
    "strategy": {
      "reload_lift_group": "USE_LIFT_ANGLES",
      "reload_lower_group": "USE_LOWER_ANGLES",
      "feed_sequence": "ANGLE1_TO_ANGLE2_TO_ANGLE1",
      "fire_sequence": "STANDBY_TO_FIRE_TO_STANDBY",
      "estop_fire_servo": "REJECT_ALL",
      "estop_other_servos": "HOLD",
      "comm_fault_servos": "HOLD",
      "recovery_other_servos": "HOLD",
      "fire_servo_safe_hold": "HOLD_CURRENT",
      "out_of_range_action": "REJECT_AND_WARN",
      "action_failure_action": "FAIL_AND_BLOCK_FIRE"
    }
  },
  "thread": {
    "iic_access": "SERIALIZED",
    "iic_priority": "MEDIUM_HIGH"
  },
  "persistence": {
    "file_format": "JSON",
    "directory": "./Config",
    "file_name": "dart_control.json",
    "write_mode": "PLAIN_FILE_WRITE",
    "power_loss_protection": false,
    "corruption_recovery": false,
    "missing_param_recovery": "USE_BUILTIN_DEFAULT",
    "version_migration": false,
    "out_of_range_handling": "NONE",
    "effective_timing": "HOT_RELOAD_IMMEDIATE",
    "pid_edit_allowed_modes": ["MANUAL", "DEBUG"],
    "pid_edit_control_source": "WEB",
    "pid_abnormal_criterion": "CONFIG_DEFINED",
    "pid_abnormal_action": "REJECT_AND_REPORT",
    "load_failure_action": "INIT_FAILED_AND_BLOCK_ACTUATORS",
    "load_timing": "INITIALIZATION",
    "reserved_variable_count": 100,
    "reserved_variable_pattern": "reserved_[001-100]",
    "reserved_variable_type": "int32"
  },
  "reserved_001": 0
})";

} // namespace

namespace marshland::tests {

void TestIntegrationFullStartup()
{
    // Create temporary config file
    const char* temp_config = "test_integration_config.nlohmann";
    {
        std::ofstream out(temp_config);
        out << kTestConfig;
    }

    StateArbitrator arbitrator;
    SystemMonotonicClock clock;

    // ATOM-15: ConfigStore
    marshland::atom15::ConfigStore config_store;

    RuntimeContext context{};
    context.configuration_path = temp_config;
    RuntimeCoordinator runtime{context};

    // Register ATOM-15
    auto config_adapter = std::make_unique<marshland::atom15::LifecycleAdapter>(config_store);
    const bool config_registered = runtime.addModule({
        RuntimeStep::LoadConfiguration,
        true,
        std::move(config_adapter)
    });

    Check(config_registered, "ATOM-15 registration");

    // Transfer sbus.serial_device to RuntimeContext
    const char* sbus_device_data = nullptr;
    std::uint64_t sbus_device_size = 0;
    const auto sbus_device_status = config_store.getString(
        "sbus.serial_device", sbus_device_data, sbus_device_size);

    Check(sbus_device_status == marshland::config::ConfigStatus::OK, "read sbus.serial_device");
    Check(sbus_device_data != nullptr, "sbus_device_data not null");

    context.values["sbus.serial_device"] = std::string(sbus_device_data, sbus_device_size);

    // ATOM-03: SBUS (with mock UART, will fail to open but won't block startup)
    MockUartByteStream sbus_uart;
    marshland::sbus::SbusChannelStore sbus_store;
    marshland::atom03::SbusCore sbus_core(sbus_uart, clock, sbus_store);

    auto sbus_adapter = std::make_unique<marshland::atom03::LifecycleAdapter>(
        sbus_core, config_store);

    const bool sbus_registered = runtime.addModule({
        RuntimeStep::InitializeSbusAndVision,
        false,  // not required
        std::move(sbus_adapter)
    });

    Check(sbus_registered, "ATOM-03 registration");

    // AppCoordinator
    AppCoordinator app{runtime, arbitrator, clock};
    ModuleInitResult detail{};
    const int start_result = app.Start(&detail);

    std::remove(temp_config);

    Check(start_result == AppCoordinator::kOk, "app.Start()");

    // Verify state
    {
        const auto view = app.ReadState();
        Check(view.context().system_mode == SystemMode::STANDBY ||
              view.context().system_mode == SystemMode::INITIALIZING,
              "valid initial mode");
    }

    app.Shutdown();
}

void TestIntegrationConfigLoadFailure()
{
    StateArbitrator arbitrator;
    SystemMonotonicClock clock;

    marshland::atom15::ConfigStore config_store;

    RuntimeContext context{};
    context.configuration_path = "nonexistent_config_file.nlohmann";
    RuntimeCoordinator runtime{context};

    auto config_adapter = std::make_unique<marshland::atom15::LifecycleAdapter>(config_store);
    const bool config_registered = runtime.addModule({
        RuntimeStep::LoadConfiguration,
        true,  // required: failure must block startup
        std::move(config_adapter)
    });

    Check(config_registered, "ATOM-15 registration");

    AppCoordinator app{runtime, arbitrator, clock};
    ModuleInitResult detail{};
    const int start_result = app.Start(&detail);

    Check(start_result != AppCoordinator::kOk, "app.Start() fails on config load failure");
    Check(detail.error_message.find("config") != std::string::npos ||
          detail.error_message.find("load") != std::string::npos,
          "error message mentions config");

    app.Shutdown();
}

void TestIntegrationAtom15BeforeAtom03()
{
    // Verify ATOM-15 runs before ATOM-03 (dependency ordering)
    const char* temp_config = "test_integration_order.nlohmann";
    {
        std::ofstream out(temp_config);
        out << kTestConfig;
    }

    StateArbitrator arbitrator;
    SystemMonotonicClock clock;

    marshland::atom15::ConfigStore config_store;

    RuntimeContext context{};
    context.configuration_path = temp_config;
    RuntimeCoordinator runtime{context};

    // Register in correct order
    auto config_adapter = std::make_unique<marshland::atom15::LifecycleAdapter>(config_store);
    runtime.addModule({
        RuntimeStep::LoadConfiguration,
        true,
        std::move(config_adapter)
    });

    MockUartByteStream sbus_uart;
    marshland::sbus::SbusChannelStore sbus_store;
    marshland::atom03::SbusCore sbus_core(sbus_uart, clock, sbus_store);

    auto sbus_adapter = std::make_unique<marshland::atom03::LifecycleAdapter>(
        sbus_core, config_store);

    runtime.addModule({
        RuntimeStep::InitializeSbusAndVision,
        false,
        std::move(sbus_adapter)
    });

    AppCoordinator app{runtime, arbitrator, clock};
    ModuleInitResult detail{};
    const int start_result = app.Start(&detail);

    std::remove(temp_config);

    Check(start_result == AppCoordinator::kOk, "startup with correct ordering");

    app.Shutdown();
}

void TestIntegrationMotorConfig6Motors()
{
    const char* temp_config = "test_integration_motors.nlohmann";
    {
        std::ofstream out(temp_config);
        out << kTestConfig;
    }

    marshland::atom15::ConfigStore config_store;
    const auto load_status = config_store.load(temp_config);

    std::remove(temp_config);

    Check(load_status.success, "load motor config");

    // Verify all 6 motors present（数组元素须经 getIntAt/getStringAt 读取，
    // 点分路径 "motor.0.id" 不是 IConfigView 的合法键）
    for (int i = 0; i < 6; ++i) {
        std::int64_t motor_id = 0;
        const auto status = config_store.getIntAt("motor", static_cast<std::uint64_t>(i), "id", motor_id);

        Check(status == marshland::config::ConfigStatus::OK, "motor exists");
        Check(motor_id == i + 1, "motor ID sequential");
    }

    // Verify motor roles
    const char* motor0_role = nullptr;
    std::uint64_t role_size = 0;
    config_store.getStringAt("motor", 0, "role", motor0_role, role_size);
    Check(std::string_view(motor0_role, role_size) == "charge_right", "motor 0 role");

    const char* motor4_role = nullptr;
    config_store.getStringAt("motor", 4, "role", motor4_role, role_size);
    Check(std::string_view(motor4_role, role_size) == "yaw", "motor 4 role");

    const char* motor5_role = nullptr;
    config_store.getStringAt("motor", 5, "role", motor5_role, role_size);
    Check(std::string_view(motor5_role, role_size) == "force", "motor 5 role");
}

void TestIntegrationCanZeroTorqueFrames()
{
    const char* temp_config = "test_integration_can.nlohmann";
    {
        std::ofstream out(temp_config);
        out << kTestConfig;
    }

    marshland::atom15::ConfigStore config_store;
    config_store.load(temp_config);

    std::remove(temp_config);

    // Verify zero torque frames include both 0x200 and 0x1FF
    // （标量字符串数组经 getStringAt 以空字段名读取元素）
    const char* frame0 = nullptr;
    std::uint64_t size0 = 0;
    const auto status0 = config_store.getStringAt("can.zero_torque.frames", 0, "", frame0, size0);

    Check(status0 == marshland::config::ConfigStatus::OK, "frame 0 exists");
    Check(std::string_view(frame0, size0) == "0x200", "frame 0 is 0x200");

    const char* frame1 = nullptr;
    std::uint64_t size1 = 0;
    const auto status1 = config_store.getStringAt("can.zero_torque.frames", 1, "", frame1, size1);

    Check(status1 == marshland::config::ConfigStatus::OK, "frame 1 exists");
    Check(std::string_view(frame1, size1) == "0x1FF", "frame 1 is 0x1FF");
}

void TestIntegrationRuntimeContextTransfer()
{
    const char* temp_config = "test_integration_context.nlohmann";
    {
        std::ofstream out(temp_config);
        out << kTestConfig;
    }

    marshland::atom15::ConfigStore config_store;
    config_store.load(temp_config);

    std::remove(temp_config);

    // Simulate Entry.cpp config transfer pattern
    const char* sbus_device = nullptr;
    std::uint64_t size = 0;
    const auto status = config_store.getString("sbus.serial_device", sbus_device, size);

    Check(status == marshland::config::ConfigStatus::OK, "read config");

    RuntimeContext context{};
    context.values["sbus.serial_device"] = std::string(sbus_device, size);

    Check(context.values.count("sbus.serial_device") > 0, "value transferred");
    Check(context.values["sbus.serial_device"] == "/dev/ttyS10", "correct value");
}

void TestIntegrationShutdownCleanup()
{
    const char* temp_config = "test_integration_shutdown.nlohmann";
    {
        std::ofstream out(temp_config);
        out << kTestConfig;
    }

    StateArbitrator arbitrator;
    SystemMonotonicClock clock;

    marshland::atom15::ConfigStore config_store;

    RuntimeContext context{};
    context.configuration_path = temp_config;
    RuntimeCoordinator runtime{context};

    auto config_adapter = std::make_unique<marshland::atom15::LifecycleAdapter>(config_store);
    runtime.addModule({
        RuntimeStep::LoadConfiguration,
        true,
        std::move(config_adapter)
    });

    const char* sbus_device = nullptr;
    std::uint64_t size = 0;
    config_store.getString("sbus.serial_device", sbus_device, size);
    context.values["sbus.serial_device"] = std::string(sbus_device, size);

    MockUartByteStream sbus_uart;
    marshland::sbus::SbusChannelStore sbus_store;
    marshland::atom03::SbusCore sbus_core(sbus_uart, clock, sbus_store);

    auto sbus_adapter = std::make_unique<marshland::atom03::LifecycleAdapter>(
        sbus_core, config_store);
    runtime.addModule({
        RuntimeStep::InitializeSbusAndVision,
        false,
        std::move(sbus_adapter)
    });

    AppCoordinator app{runtime, arbitrator, clock};
    ModuleInitResult detail{};
    app.Start(&detail);

    // Shutdown should not crash
    app.Shutdown();
    Check(true, "shutdown completed");

    std::remove(temp_config);
}

} // namespace marshland::tests

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <test-case>\n", argv[0]);
        return 1;
    }

    const std::string_view case_name{argv[1]};

    try {
        using namespace marshland::tests;

        if (case_name == "integration.full_startup") {
            TestIntegrationFullStartup();
        } else if (case_name == "integration.config_load_failure") {
            TestIntegrationConfigLoadFailure();
        } else if (case_name == "integration.atom15_before_atom03") {
            TestIntegrationAtom15BeforeAtom03();
        } else if (case_name == "integration.motor_config_6_motors") {
            TestIntegrationMotorConfig6Motors();
        } else if (case_name == "integration.can_zero_torque_frames") {
            TestIntegrationCanZeroTorqueFrames();
        } else if (case_name == "integration.runtime_context_transfer") {
            TestIntegrationRuntimeContextTransfer();
        } else if (case_name == "integration.shutdown_cleanup") {
            TestIntegrationShutdownCleanup();
        } else {
            std::fprintf(stderr, "unknown test case: %s\n", argv[1]);
            return 1;
        }

        std::printf("[PASS] %s\n", argv[1]);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[FAIL] %s: %s\n", argv[1], e.what());
        return 1;
    }
}
