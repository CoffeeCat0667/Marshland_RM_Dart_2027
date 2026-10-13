#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
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
    "motor_loop_period_ms": 20
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
  "can": {
    "interface": "CAN0",
    "bitrate": "1Mbps",
    "protocol_source": "C620"
  },
  "sbus": {
    "serial_device": "/dev/ttyS10",
    "baudrate": 100000,
    "channel_count": 16
  },
  "persistence": {
    "file_format": "JSON",
    "directory": "./Config"
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
    const char* temp_file = "test_config_valid.json";
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
    const char* temp_file = "test_config_invalid.json";
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
    const auto result = store.load("nonexistent_config_file.json");

    Check(!result.success, "reject missing file");
}

void TestConfigStoreGetString()
{
    const char* temp_file = "test_config_get_string.json";
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
    const char* temp_file = "test_config_get_int.json";
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
}

void TestConfigStoreGetIntFromString()
{
    const char* temp_file = "test_config_get_int_wrong_type.json";
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
    const char* temp_file = "test_config_get_string_wrong_type.json";
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
    const char* temp_file = "test_config_missing_key.json";
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
    const char* temp_file = "test_config_motor_array.json";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    // Access motor[0].id
    std::int64_t motor_id = 0;
    const auto status = store.getInt("motor.0.id", motor_id);

    Check(status == ConfigStatus::OK, "access motor array element");
    Check(motor_id == 1, "motor[0].id == 1");
}

void TestConfigStoreNestedKeys()
{
    const char* temp_file = "test_config_nested.json";
    {
        std::ofstream out(temp_file);
        out << kMinimalValidConfig;
    }

    ConfigStore store;
    store.load(temp_file);
    std::remove(temp_file);

    // Access motor[0].pid.kp
    std::int64_t kp = 0;
    const auto status = store.getInt("motor.0.pid.kp", kp);

    Check(status == ConfigStatus::OK, "access deeply nested key");
    Check(kp == 3, "motor[0].pid.kp == 3");
}

void TestConfigStoreReload()
{
    const char* temp_file = "test_config_reload.json";

    // First config
    {
        std::ofstream out(temp_file);
        out << R"({
          "schema_version": 1,
          "test_value": 42
        })";
    }

    ConfigStore store;
    store.load(temp_file);

    std::int64_t value = 0;
    store.getInt("test_value", value);
    Check(value == 42, "first load: test_value == 42");

    // Second config (reload)
    {
        std::ofstream out(temp_file);
        out << R"({
          "schema_version": 1,
          "test_value": 99
        })";
    }

    store.load(temp_file);
    store.getInt("test_value", value);
    Check(value == 99, "reload: test_value == 99");

    std::remove(temp_file);
}

void TestConfigStoreIConfigViewInterface()
{
    const char* temp_file = "test_config_interface.json";
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
    const char* temp_file = "test_config_thread_safety.json";
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
    const char* temp_file = "test_config_schema.json";
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
