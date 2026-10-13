#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include "IConfigView.hpp"
#include "ILifecycle.hpp"
namespace marshland::atom15 {
enum class UpdateSource : std::uint8_t { NONE, SBUS, WEB, AUTO };
enum class UpdateMode : std::uint8_t { UNINITIALIZED, INITIALIZING, INIT_FAILED, STANDBY, MANUAL, AUTO_FIRE, DEBUG, CALIBRATION, RECOVERY, SOFTWARE_ESTOP, FAULT };
enum class ConfigError : std::uint8_t { OK, FILE_NOT_FOUND, FILE_IO, INVALID_JSON, INVALID_SCHEMA, NOT_LOADED, MISSING, INVALID_TYPE, OUT_OF_RANGE, NOT_AUTHORIZED, PERSISTENCE_FAILED, LIMIT_REACHED, NOT_FOUND };
struct ConfigDiagnostic final { std::string key; std::string message; bool warning{false}; };
struct ConfigLoadResult final { bool success{false}; ConfigError error{ConfigError::NOT_LOADED}; std::vector<ConfigDiagnostic> diagnostics; };
using ConfigValue = std::variant<std::int64_t, double, bool, std::string>;
struct PersistResult final { bool accepted{false}; bool effective{false}; bool persisted{false}; ConfigError error{ConfigError::NOT_LOADED}; std::string message; };
struct PidConfig final { std::int64_t kp{0}; std::int64_t ki{0}; std::int64_t kd{0}; std::int64_t integral_limit{0}; std::int64_t output_limit{0}; std::int64_t control_period_ms{0}; bool enabled{false}; };
struct PidReadResult final { bool success{false}; ConfigError error{ConfigError::NOT_LOADED}; PidConfig value{}; std::string message; };
class ConfigStore final : public config::IConfigView {
public:
    using ChangeCallback = std::function<void(const std::string&, std::uint64_t)>;
    explicit ConfigStore(ChangeCallback callback = {});
    ~ConfigStore() override;
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;
    ConfigLoadResult load(const std::filesystem::path& path);
    ConfigLoadResult reload();
    std::filesystem::path path() const;
    std::vector<ConfigDiagnostic> diagnostics() const;
    void setChangeCallback(ChangeCallback callback);
    PersistResult updateRuntime(const char* key, const ConfigValue& value, UpdateSource source, UpdateMode mode);
    PidReadResult readPid(std::int64_t motor_id) const;
    PersistResult writePid(std::int64_t motor_id, const PidConfig& value, UpdateSource source, UpdateMode mode);
    PersistResult readReserved(std::uint32_t index, std::int32_t& value) const;
    PersistResult writeReserved(std::uint32_t index, std::int32_t value, UpdateSource source, UpdateMode mode);
    config::ConfigStatus getInt(const char* key, std::int64_t& out) const noexcept override;
    config::ConfigStatus getDouble(const char* key, double& out) const noexcept override;
    config::ConfigStatus getBool(const char* key, bool& out) const noexcept override;
    config::ConfigStatus getString(const char* key, const char*& out_data, std::uint64_t& out_size) const noexcept override;
    bool has(const char* key) const noexcept override;
    config::ConfigStatus getArraySize(const char* key, std::uint64_t& out_size) const noexcept override;
    config::ConfigStatus getIntAt(const char* key, std::uint64_t index, const char* element_key, std::int64_t& out) const noexcept override;
    config::ConfigStatus getDoubleAt(const char* key, std::uint64_t index, const char* element_key, double& out) const noexcept override;
    config::ConfigStatus getBoolAt(const char* key, std::uint64_t index, const char* element_key, bool& out) const noexcept override;
    config::ConfigStatus getStringAt(const char* key, std::uint64_t index, const char* element_key, const char*& out_data, std::uint64_t& out_size) const noexcept override;
    std::uint64_t configVersion() const noexcept override;
    bool isLoaded() const noexcept override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    explicit LifecycleAdapter(ConfigStore& store) noexcept : store_(store) {}
    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom15-config-persistence"; }
private:
    ConfigStore& store_;
};
}

