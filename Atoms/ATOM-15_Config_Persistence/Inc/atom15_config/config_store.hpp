#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// ============================================================================
// ATOM-15 配置存储：加载 / 校验 / 查询。
//
// 本头不继承、不包含任何跨模块契约（无 IConfigView、无 ILifecycle）：
// 只有 ConfigStore 一个类，由持有者直接调用。键名与类型见 Rule/ConfigSchema.md。
//
// !! 已知偏离设计（2026-10-17，用户明确授权删除）!!
//   Design/Atomic/ATOM-15_Config_Persistence.md 第 29-34 行（原子操作 10/11/12/14/15）
//   与第 36-43 行「PID 参数修改授权」、第 55-61 行验收条件所要求的
//   「运行时热加载 / 写回 JSON / 保存 PID / 更新预留变量」在本实现中被移除：
//     updateRuntime（两个重载）、writePid、writeReserved、ChangeCallback 已删除。
//   后果：ATOM-15 的三条持久化验收条件当前无法满足，PID 参数无法持久化、重启即丢。
//   恢复方式：重新接入上述三个写入口，并由 ATOM-16 提供对应 HTTP 路由。
//   读取能力不受影响：load / get / getInt / getString / readPid / readReserved 保留。
// ============================================================================

namespace marshland::atom15 {

enum class ConfigError : std::uint8_t { OK, FILE_NOT_FOUND, FILE_IO, INVALID_JSON, INVALID_SCHEMA, NOT_LOADED, MISSING, INVALID_TYPE, OUT_OF_RANGE, NOT_AUTHORIZED, PERSISTENCE_FAILED, LIMIT_REACHED, NOT_FOUND };

// 按值查询的结果状态；非 OK 时 out 保持调用前的值不变。
enum class ConfigStatus : std::uint8_t { OK, NOT_LOADED, MISSING, INVALID_TYPE };

struct ConfigDiagnostic final { std::string key; std::string message; bool warning{false}; };
struct ConfigLoadResult final { bool success{false}; ConfigError error{ConfigError::NOT_LOADED}; std::vector<ConfigDiagnostic> diagnostics; };

// std::monostate 表示 JSON 的 null，即配置里"标记待定、禁止臆造"的显式占位。
using ConfigValue = std::variant<std::monostate, std::int64_t, double, bool, std::string>;

// readReserved 的结果载体。写入路径已移除，本类型当前只服务只读查询。
struct PersistResult final { bool accepted{false}; bool effective{false}; bool persisted{false}; ConfigError error{ConfigError::NOT_LOADED}; std::string message; };
struct PidConfig final { std::int64_t kp{0}; std::int64_t ki{0}; std::int64_t kd{0}; std::int64_t integral_limit{0}; std::int64_t output_limit{0}; std::int64_t control_period_ms{0}; bool enabled{false}; };
struct PidReadResult final { bool success{false}; ConfigError error{ConfigError::NOT_LOADED}; PidConfig value{}; std::string message; };

class ConfigStore final {
public:
    ConfigStore();
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

    // 电机 PID 与预留变量（reserved_001..reserved_100，int32）。
    PidReadResult readPid(std::int64_t motor_id) const;
    PersistResult readReserved(std::uint32_t index, std::int32_t& value) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace marshland::atom15
