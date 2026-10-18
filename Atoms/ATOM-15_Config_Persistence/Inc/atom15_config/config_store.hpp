#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

// ============================================================================
// ATOM-15 配置存储：加载 / 校验 / 运行时更新 / 持久化。
//
// 本头不继承、不包含任何跨模块契约（无 IConfigView、无 ILifecycle）：
// 只有 ConfigStore 一个类，由持有者直接调用。键名与类型见 Rule/ConfigSchema.md。
// ============================================================================

namespace marshland::atom15 {

enum class UpdateSource : std::uint8_t { NONE, SBUS, WEB, AUTO };
enum class UpdateMode : std::uint8_t { UNINITIALIZED, INITIALIZING, INIT_FAILED, STANDBY, MANUAL, AUTO_FIRE, DEBUG, CALIBRATION, RECOVERY, SOFTWARE_ESTOP, FAULT };
enum class ConfigError : std::uint8_t { OK, FILE_NOT_FOUND, FILE_IO, INVALID_JSON, INVALID_SCHEMA, NOT_LOADED, MISSING, INVALID_TYPE, OUT_OF_RANGE, NOT_AUTHORIZED, PERSISTENCE_FAILED, LIMIT_REACHED, NOT_FOUND };

// 按值查询的结果状态；非 OK 时 out 保持调用前的值不变。
enum class ConfigStatus : std::uint8_t { OK, NOT_LOADED, MISSING, INVALID_TYPE };

struct ConfigDiagnostic final { std::string key; std::string message; bool warning{false}; };
struct ConfigLoadResult final { bool success{false}; ConfigError error{ConfigError::NOT_LOADED}; std::vector<ConfigDiagnostic> diagnostics; };

// std::monostate 表示 JSON 的 null，即配置里"标记待定、禁止臆造"的显式占位。
using ConfigValue = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

// 一条整键赋值；批量提交时全部落在同一候选快照上（一次校验、一次落盘、一次版本自增）。
struct Assignment final { const char* key{nullptr}; ConfigValue value; };

struct PersistResult final { bool accepted{false}; bool effective{false}; bool persisted{false}; ConfigError error{ConfigError::NOT_LOADED}; std::string message; };
struct PidConfig final { std::int64_t kp{0}; std::int64_t ki{0}; std::int64_t kd{0}; std::int64_t integral_limit{0}; std::int64_t output_limit{0}; std::int64_t control_period_ms{0}; bool enabled{false}; };
struct PidReadResult final { bool success{false}; ConfigError error{ConfigError::NOT_LOADED}; PidConfig value{}; std::string message; };

class ConfigStore final {
public:
    // 更新生效后由本类在锁外调用（参数为键与递增后的版本号）。
    using ChangeCallback = std::function<void(const std::string&, std::uint64_t)>;

    explicit ConfigStore(ChangeCallback callback = {});
    ~ConfigStore();
    ConfigStore(const ConfigStore&) = delete;
    ConfigStore& operator=(const ConfigStore&) = delete;

    // 加载 / 重载；只有完整校验通过后才替换内存快照。
    // 重载就是再次调用 load(同一路径)——路径由持有者自己记住。
    ConfigLoadResult load(const std::filesystem::path& path);

    // ---- 按值查询（只读内存快照、不做 I/O、任意线程可调）------------------
    // path 语法："a.b" 或 "a[3].b"（每段最多一个下标）。成功时 out 拿到的是
    // 快照值的副本；JSON null 以 std::monostate 返回（OK），数组/对象本身不可
    // 用 get 取（返回 INVALID_TYPE），用 getArraySize + 索引路径访问元素。
    ConfigStatus get(const char* path, ConfigValue& out) const noexcept;

    // 类型化包装：语义与 get 完全一致，只是省掉调用方的 variant 判定。
    // 消费方优先用这四个；get 留给枚举字符串与尚未类型化的值。
    ConfigStatus getInt(const char* path, std::int64_t& out) const noexcept;
    ConfigStatus getDouble(const char* path, double& out) const noexcept;
    ConfigStatus getBool(const char* path, bool& out) const noexcept;
    ConfigStatus getString(const char* path, std::string& out) const noexcept;

    ConfigStatus getArraySize(const char* path, std::uint64_t& out) const noexcept;
    // 每次成功 load 或热更新自增；0 表示从未加载成功。消费方据此决定是否重读。
    std::uint64_t configVersion() const noexcept;

    // 运行时更新：仅 WEB 来源 + MANUAL/DEBUG 模式；候选快照经同一套校验后才生效。
    // 批量提交是原子的：任一条不合法则整批不生效、不落盘、版本不变；
    // 回调按赋值逐条触发（版本号同一）。
    PersistResult updateRuntime(std::span<const Assignment> assignments, UpdateSource source, UpdateMode mode);
    PersistResult updateRuntime(const char* key, const ConfigValue& value, UpdateSource source, UpdateMode mode);

    // 电机 PID 与预留变量（reserved_001..reserved_100，int32）。
    PidReadResult readPid(std::int64_t motor_id) const;
    PersistResult writePid(std::int64_t motor_id, const PidConfig& value, UpdateSource source, UpdateMode mode);
    PersistResult readReserved(std::uint32_t index, std::int32_t& value) const;
    PersistResult writeReserved(std::uint32_t index, std::int32_t value, UpdateSource source, UpdateMode mode);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace marshland::atom15
