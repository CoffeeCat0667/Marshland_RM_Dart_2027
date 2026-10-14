#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "atom15_config/config_store.hpp"
#include "IConfigView.hpp"

namespace {

using namespace marshland;
using namespace marshland::atom15;
using namespace marshland::config;

void Check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Create a minimal valid config file for testing
const char* kMinimalValidConfig = R"({
  "schema_version": 1,
  "baseline": {
    "auto_total_shots": 4,
    "time_base": "MONOTONIC"
  },
  "mode": {
    "enabled": {
      "initialization": true,
      "standby": true
    },
    "initial": {
      "system_mode": "UNINITIALIZED",
      "control_source": "AUTO"
    }
  },
  "timing": {
    "max_control_cycle_ms": 100,
    "motor_loop_period_ms": 20,
    "sbus_receive_period_ms": 20,
    "sbus_loss_timeout_ms": 200,
    "can_comm_timeout_ms": 200,
    "iic_comm_timeout_ms": 200,
    "charge_max_action_ms": 30000,
    "charge_home_max_action_ms": 25000
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
    "protocol_source": "C620"
  },
  "sbus": {
    "serial_device": "/dev/ttyS10",
    "baudrate": 100000,
    "channel_count": 16,
    "channel_scale_x": 1,
    "frame_bytes": 25,
    "joystick_center": 1024,
    "valid_min": 364,
    "valid_max": 1684,
    "functions": [
      {"name": "manual_charge", "channel": "CH2", "signal_type": "JOYSTICK"},
      {"name": "manual_charge_left", "channel": "CH8", "signal_type": "JOYSTICK"},
      {"name": "manual_charge_right", "channel": "CH9", "signal_type": "JOYSTICK"}
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
    "file_name": "dart_control.nlohmann",
    "reserved_variable_count": 100
  }
})";

const char* kInvalidJson = R"({
  "schema_version": 1,
  "baseline": {
    "auto_total_shots": 4
  // Missing closing brace
})";

const char* kMissingRequiredField = R"({
  "baseline": {
    "auto_total_shots": 4
  }
})";

constexpr const char* kCanTimeoutKey = "timing.can_comm_timeout_ms";

std::string WithCanTimeout(const char* json_value)
{
    std::string text{kMinimalValidConfig};
    constexpr std::string_view field{"\"can_comm_timeout_ms\": 200"};
    const auto position = text.find(field);
    Check(position != std::string::npos, "fixture contains CAN timeout");
    // Keep the surrounding object valid when testing a missing required field.
    if (!json_value) {
        const auto comma = text.rfind(',', position);
        text.erase(comma, position + field.size() - comma);
    } else {
        text.replace(position, field.size(),
                     std::string{"\"can_comm_timeout_ms\": "} + json_value);
    }
    return text;
}

class CanTimeoutFile final {
public:
    static constexpr const char* path = "test_config_can_timeout.nlohmann";

    explicit CanTimeoutFile(const std::string& text) { write(text); }
    ~CanTimeoutFile() { std::remove(path); }

    void write(const std::string& text) const
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
        Check(static_cast<bool>(out), "write CAN timeout fixture");
    }

    std::string read() const
    {
        std::ifstream in(path, std::ios::binary);
        Check(static_cast<bool>(in), "read CAN timeout fixture");
        return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    }
};

} // namespace

namespace marshland::tests {

void TestConfigStoreDefaultState()
{
    ConfigStore store;

    const char* value = nullptr;
    std::uint64_t size = 0;
    const auto status = store.getString("nonexistent.key", value, size);

    Check(status == ConfigStatus::NOT_LOADED, "default state is NOT_LOADED");
}

void TestConfigStoreLoadValidFile()
{
    // Create temporary config file
    const char* temp_file = "test_config_valid.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    const auto result = store.load(temp_file);

    std::remove(temp_file);

    Check(result.success, "load valid config");
}

void TestConfigStoreLoadInvalidJson()
{
    const char* temp_file = "test_config_invalid.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kInvalidJson;
    }

    ConfigStore store;
    const auto result = store.load(temp_file);

    std::remove(temp_file);

    Check(!result.success, "reject invalid JSON");
}

void TestConfigStoreLoadMissingFile()
{
    ConfigStore store;
    const auto result = store.load("nonexistent_config_file.nlohmann");

    Check(!result.success, "reject missing file");
}

void TestConfigStoreGetString()
{
    const char* temp_file = "test_config_get_string.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    const char* value = nullptr;
    std::uint64_t size = 0;
    const auto status = store.getString("sbus.serial_device", value, size);

    Check(status == ConfigStatus::OK, "getString status OK");
    Check(value != nullptr, "getString returns non-null");
    Check(size > 0, "getString returns non-zero size");

    const std::string_view device_path(value, size);
    Check(device_path == "/dev/ttyS10", "getString correct value");
}

void TestConfigStoreGetInt()
{
    const char* temp_file = "test_config_get_int.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    std::int64_t value = 0;
    const auto status = store.getInt("baseline.auto_total_shots", value);

    Check(status == ConfigStatus::OK, "getInt status OK");
    Check(value == 4, "getInt correct value");

    Check(store.getInt("charge.charge_speed_rpm", value) == ConfigStatus::OK && value == 5000,
          "charge speed default is 5000 rpm");
    Check(store.getInt("charge.home_speed_rpm", value) == ConfigStatus::OK && value == 5000,
          "charge home speed default is 5000 rpm");
    Check(store.getInt("force.center_deadband_rpm", value) == ConfigStatus::OK && value == 0,
          "force center deadband default is 0 rpm");
    const char* center_action = nullptr;
    std::uint64_t center_action_size = 0;
    Check(store.getString("force.center_action", center_action, center_action_size) == ConfigStatus::OK &&
              std::string_view(center_action, center_action_size) == "ZERO_TORQUE",
          "force center action is ZERO_TORQUE");

    std::uint64_t channel_count = 0;
    Check(store.getArraySize("servo.channels", channel_count) == ConfigStatus::OK && channel_count == 2,
          "servo channel mapping has FEED and FIRE entries");
    Check(store.getIntAt("servo.channels", 0, "channel", value) == ConfigStatus::OK && value == 4,
          "FEED uses PCA9685A channel 4");
    Check(store.getIntAt("servo.channels", 1, "channel", value) == ConfigStatus::OK && value == 5,
          "FIRE uses PCA9685A channel 5");
    Check(store.getIntAt("servo.pca9685.channel_range", 0, "", value) == ConfigStatus::OK && value == 0,
          "scalar integer array lower bound is readable");
    Check(store.getIntAt("servo.pca9685.channel_range", 1, "", value) == ConfigStatus::OK && value == 15,
          "scalar integer array upper bound is readable");
}

void TestConfigStoreGetIntFromString()
{
    const char* temp_file = "test_config_get_int_wrong_type.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    std::int64_t value = 0;
    const auto status = store.getInt("sbus.serial_device", value);

    Check(status == ConfigStatus::INVALID_TYPE, "getInt rejects string");
}

void TestConfigStoreGetStringFromInt()
{
    const char* temp_file = "test_config_get_string_wrong_type.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    const char* value = nullptr;
    std::uint64_t size = 0;
    const auto status = store.getString("baseline.auto_total_shots", value, size);

    Check(status == ConfigStatus::INVALID_TYPE, "getString rejects int");
}

void TestConfigStoreGetMissingKey()
{
    const char* temp_file = "test_config_missing_key.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    const char* value = nullptr;
    std::uint64_t size = 0;
    const auto status = store.getString("nonexistent.key", value, size);

    Check(status == ConfigStatus::MISSING, "missing key returns MISSING");
}

void TestConfigStoreMotorArray()
{
    const char* temp_file = "test_config_motor_array.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    // Access motor[0].id
    std::int64_t motor_id = 0;
    const auto status = store.getIntAt("motor", 0, "id", motor_id);

    Check(status == ConfigStatus::OK, "access motor array element");
    Check(motor_id == 1, "motor[0].id == 1");
}

void TestConfigStoreNestedKeys()
{
    const char* temp_file = "test_config_nested.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    const char* mode = nullptr;
    std::uint64_t size = 0;
    const auto status = store.getString("mode.initial.system_mode", mode, size);

    Check(status == ConfigStatus::OK, "access deeply nested key");
    Check(std::string_view(mode, size) == "UNINITIALIZED", "nested mode value");
}

void TestConfigStoreReload()
{
    CanTimeoutFile file{WithCanTimeout("200")};
    ConfigStore store;
    Check(store.load(CanTimeoutFile::path).success, "first config loads");
    const auto version = store.configVersion();
    std::int64_t value = 0;
    Check(store.getInt(kCanTimeoutKey, value) == ConfigStatus::OK && value == 200,
          "first load uses initial CAN timeout");
    file.write(WithCanTimeout("500"));
    Check(store.reload().success, "reload accepts changed timeout");
    Check(store.getInt(kCanTimeoutKey, value) == ConfigStatus::OK && value == 500,
          "reload exposes changed CAN timeout");
    Check(store.configVersion() == version + 1, "reload increments version");
}

void TestCanCommTimeoutLoad()
{
    for (const char* valid : {"1", "200", "6000"}) {
        CanTimeoutFile file{WithCanTimeout(valid)};
        ConfigStore store;
        Check(store.load(CanTimeoutFile::path).success, "legal CAN timeout loads");
        const IConfigView& view = store;
        std::int64_t value = 0;
        Check(view.getInt(kCanTimeoutKey, value) == ConfigStatus::OK,
              "CAN timeout readable through public view");
        Check(value == std::stoll(valid), "CAN timeout value preserved");
    }

    const char* invalid_values[] = {nullptr, "0", "-1", "6001", "9223372036854775807",
                                   "\"200\"", "200.0", "true", "null"};
    for (const char* invalid : invalid_values) {
        CanTimeoutFile file{WithCanTimeout(invalid)};
        ConfigStore store;
        const auto result = store.load(CanTimeoutFile::path);
        Check(!result.success && result.error == ConfigError::INVALID_SCHEMA,
              "missing, mistyped or out-of-range CAN timeout rejected");
        Check(!store.isLoaded() && store.configVersion() == 0,
              "failed load leaves store unloaded");
        Check(std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                          [](const auto& d) { return d.key == kCanTimeoutKey; }),
              "CAN timeout diagnostic contains full field path");
        std::int64_t value = 77;
        Check(store.getInt(kCanTimeoutKey, value) == ConfigStatus::NOT_LOADED && value == 77,
              "failed load does not fabricate a default");
    }
}

void TestCanCommTimeoutUpdate()
{
    CanTimeoutFile file{WithCanTimeout("200")};
    std::uint64_t callbacks = 0;
    ConfigStore store([&](const std::string& key, std::uint64_t version) {
        Check(key == kCanTimeoutKey, "callback identifies CAN timeout");
        Check(version == callbacks + 2, "callback identifies committed version");
        ++callbacks;
    });
    Check(store.load(CanTimeoutFile::path).success, "load before updating timeout");
    const ConfigValue invalid_values[] = {
        std::int64_t{0}, std::int64_t{-1}, std::int64_t{6001},
        std::numeric_limits<std::int64_t>::max(), 200.0, true,
        std::string{"200"}, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()
    };
    const auto unchanged_file = file.read();
    for (const auto& value : invalid_values) {
        const auto result = store.updateRuntime(kCanTimeoutKey, value, UpdateSource::WEB,
                                                UpdateMode::MANUAL);
        Check(!result.accepted && !result.effective && !result.persisted,
              "invalid update is not effective or persisted");
        std::int64_t current = 0;
        Check(store.getInt(kCanTimeoutKey, current) == ConfigStatus::OK && current == 200,
              "rejected update retains active value");
        Check(store.configVersion() == 1 && callbacks == 0 && file.read() == unchanged_file,
              "rejected update retains version, callbacks and file");
    }
    for (const auto source : {UpdateSource::NONE, UpdateSource::SBUS, UpdateSource::AUTO}) {
        const auto result = store.updateRuntime(kCanTimeoutKey, std::int64_t{500}, source,
                                                UpdateMode::MANUAL);
        Check(result.error == ConfigError::NOT_AUTHORIZED && !result.effective,
              "non-WEB source cannot update timeout");
    }
    for (const auto mode : {UpdateMode::STANDBY, UpdateMode::AUTO_FIRE, UpdateMode::FAULT}) {
        const auto result = store.updateRuntime(kCanTimeoutKey, std::int64_t{500},
                                                UpdateSource::WEB, mode);
        Check(result.error == ConfigError::NOT_AUTHORIZED && !result.effective,
              "disallowed mode cannot update timeout");
    }
    Check(file.read() == unchanged_file && store.configVersion() == 1 && callbacks == 0,
          "authorization rejection has no side effects");
    for (const std::int64_t value : {1, 500, 6000}) {
        const auto mode = value == 500 ? UpdateMode::DEBUG : UpdateMode::MANUAL;
        const auto result = store.updateRuntime(kCanTimeoutKey, value, UpdateSource::WEB, mode);
        Check(result.accepted && result.effective && result.persisted && result.error == ConfigError::OK,
              "WEB update in MANUAL/DEBUG applies and persists");
        const IConfigView& view = store;
        std::int64_t current = 0;
        Check(view.getInt(kCanTimeoutKey, current) == ConfigStatus::OK && current == value,
              "successful update immediately visible");
        ConfigStore restarted;
        Check(restarted.load(CanTimeoutFile::path).success,
              "persisted config can be loaded after restart");
        Check(restarted.getInt(kCanTimeoutKey, current) == ConfigStatus::OK && current == value,
              "restart restores persisted timeout");
    }
    Check(callbacks == 3 && store.configVersion() == 4, "only successful updates increment version");
}

void TestCanCommTimeoutPersistenceFailure()
{
    CanTimeoutFile file{WithCanTimeout("200")};
    ConfigStore store;
    Check(store.load(CanTimeoutFile::path).success, "load before persistence failure");
    Check(std::remove(CanTimeoutFile::path) == 0, "remove original fixture");
    std::filesystem::create_directory(CanTimeoutFile::path);
    PersistResult result;
    try {
        result = store.updateRuntime(kCanTimeoutKey, std::int64_t{500}, UpdateSource::WEB,
                                     UpdateMode::MANUAL);
    } catch (...) {
        std::filesystem::remove(CanTimeoutFile::path);
        throw;
    }
    std::filesystem::remove(CanTimeoutFile::path);
    Check(result.accepted && result.effective && !result.persisted &&
          result.error == ConfigError::PERSISTENCE_FAILED,
          "persistence failure is separate from runtime effectiveness");
    std::int64_t current = 0;
    Check(store.getInt(kCanTimeoutKey, current) == ConfigStatus::OK && current == 500,
          "effective value preserved after persistence failure");
    Check(store.configVersion() == 2, "effective update increments version on write failure");
}

void TestConfigStoreIConfigViewInterface()
{
    const char* temp_file = "test_config_interface.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    // Test through IConfigView interface
    IConfigView* view = &store;

    const char* device = nullptr;
    std::uint64_t size = 0;
    const auto status = view->getString("sbus.serial_device", device, size);

    Check(status == ConfigStatus::OK, "IConfigView interface works");
    Check(device != nullptr, "IConfigView getString returns data");
}

void TestConfigStoreThreadSafety()
{
    const char* temp_file = "test_config_thread_safety.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    std::atomic<bool> stop{false};
    std::atomic<int> read_count{0};

    // Multiple reader threads
    auto reader_func = [&]() {
        while (!stop.load()) {
            const char* value = nullptr;
            std::uint64_t size = 0;
            store.getString("sbus.serial_device", value, size);
            ++read_count;
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    };

    std::thread reader1(reader_func);
    std::thread reader2(reader_func);
    std::thread reader3(reader_func);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop = true;

    reader1.join();
    reader2.join();
    reader3.join();

    Check(read_count.load() > 0, "concurrent reads completed");
}

void TestConfigStoreSchemaVersion()
{
    const char* temp_file = "test_config_schema.nlohmann";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    std::int64_t version = 0;
    const auto status = store.getInt("schema_version", version);

    Check(status == ConfigStatus::OK, "schema_version accessible");
    Check(version == 1, "schema_version == 1");
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

        if (case_name == "config.store_default_state") {
            TestConfigStoreDefaultState();
        } else if (case_name == "config.store_load_valid") {
            TestConfigStoreLoadValidFile();
        } else if (case_name == "config.store_load_invalid_json") {
            TestConfigStoreLoadInvalidJson();
        } else if (case_name == "config.store_load_missing_file") {
            TestConfigStoreLoadMissingFile();
        } else if (case_name == "config.store_get_string") {
            TestConfigStoreGetString();
        } else if (case_name == "config.store_get_int") {
            TestConfigStoreGetInt();
        } else if (case_name == "config.store_get_int_wrong_type") {
            TestConfigStoreGetIntFromString();
        } else if (case_name == "config.store_get_string_wrong_type") {
            TestConfigStoreGetStringFromInt();
        } else if (case_name == "config.store_missing_key") {
            TestConfigStoreGetMissingKey();
        } else if (case_name == "config.store_motor_array") {
            TestConfigStoreMotorArray();
        } else if (case_name == "config.store_nested_keys") {
            TestConfigStoreNestedKeys();
        } else if (case_name == "config.store_reload") {
            TestConfigStoreReload();
        } else if (case_name == "config.store_interface") {
            TestConfigStoreIConfigViewInterface();
        } else if (case_name == "config.store_thread_safety") {
            TestConfigStoreThreadSafety();
        } else if (case_name == "config.store_schema_version") {
            TestConfigStoreSchemaVersion();
        } else if (case_name == "config.can_comm_timeout") {
            TestCanCommTimeoutLoad();
            TestCanCommTimeoutUpdate();
            TestCanCommTimeoutPersistenceFailure();
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
