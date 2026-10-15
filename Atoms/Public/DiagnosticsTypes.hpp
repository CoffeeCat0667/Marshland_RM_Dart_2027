// ============================================================================
// DiagnosticsTypes.hpp — 故障诊断与日志的跨 ATOM 公共类型
//
// 归属说明：
//   本文件是**跨模块契约**，与 MarshlandTypes.hpp / IConfigView.hpp 同级，
//   置于 Atoms/Public/。由 ATOM-18_Diagnostics_Logging 实现对应接口，
//   其他 ATOM 只依赖本契约，禁止包含 ATOM-18 的私有头。
//
// 权威来源：
//   Design/Config.md §16.1 日志运行参数、§16.3 日志字段、§14.2 故障表
//   Rule/ConfigSchema.md §2.18（log.* 键名；log.level 合法集 TRACE/DEBUG/
//   INFO/WARN/ERROR）
//   Design/Atomic/ATOM-18_Diagnostics_Logging.md 内部原子操作 1~15
//
// 设计约束：
//   * payload 用固定容量字段列表表达（用户裁决 2026-10-15）：
//     nlohmann/json 按 ThirdParty 约定 PRIVATE 链接，不得出现在 Public 签名；
//   * 所有字符串参数以 string_view 传入，实现方负责在入队前拷贝；
//     string_view 所指内存只需在调用期间有效；
//   * 时间戳由实现方在受理时打（UTC ISO8601 毫秒，Config §16.1），
//     不由调用方提供。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "MarshlandTypes.hpp"

namespace marshland::diagnostics {

// ---------------------------------------------------------------------------
// 日志等级（Rule/ConfigSchema.schema.json log.level 封闭枚举）
// ---------------------------------------------------------------------------
enum class LogLevel : std::uint8_t {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR
};

inline const char* ToString(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::TRACE: return "TRACE";
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    case LogLevel::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

inline std::optional<LogLevel> LogLevelFromString(std::string_view text) noexcept
{
    if (text == "TRACE") return LogLevel::TRACE;
    if (text == "DEBUG") return LogLevel::DEBUG;
    if (text == "INFO")  return LogLevel::INFO;
    if (text == "WARN")  return LogLevel::WARN;
    if (text == "ERROR") return LogLevel::ERROR;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 逻辑日志流（诊断 / 运行 / 安全）
//
// Config §16.1 只有一套轮转参数，三类事件落入同一文件流（用户裁决
// 2026-10-15：单一事件流文件）；本枚举注入 payload 的保留键 "stream"，
// 使三类事件在 JSON Lines 中可区分。
// ---------------------------------------------------------------------------
enum class LogStream : std::uint8_t {
    DIAGNOSTIC,
    RUN,
    SAFETY
};

inline const char* ToString(LogStream stream) noexcept
{
    switch (stream) {
    case LogStream::DIAGNOSTIC: return "diagnostic";
    case LogStream::RUN:        return "run";
    case LogStream::SAFETY:     return "safety";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// 事件 payload：固定容量字段列表
// ---------------------------------------------------------------------------
using PayloadScalar =
    std::variant<std::monostate, std::int64_t, double, bool, std::string_view>;

struct PayloadField {
    std::string_view key{};
    PayloadScalar value{std::monostate{}};
};

inline constexpr std::size_t kMaxPayloadFields = 16;

// payload 中由实现方注入的保留键名
inline constexpr std::string_view kPayloadKeyStream = "stream";
inline constexpr std::string_view kPayloadKeyTruncated = "_truncated";

// ---------------------------------------------------------------------------
// 单条日志事件（字段对齐 Config §16.3；timestamp 由实现方打）
// ---------------------------------------------------------------------------
struct LogEvent {
    LogLevel level{LogLevel::INFO};
    std::string_view module{};
    std::string_view event{};
    std::string_view source{};
    std::string_view state{};
    std::string_view request_id{};   // 可为空
    std::string_view error_code{};   // 可为空
    const PayloadField* payload{nullptr};
    std::size_t payload_count{0};
};

// ---------------------------------------------------------------------------
// 故障记录
// ---------------------------------------------------------------------------
struct FaultRecord {
    std::uint64_t id{0};             // 模块内单调分配；0 为无效
    std::string code{};              // 故障码，必须取自 Config §14.2
    FaultSeverity severity{FaultSeverity::INFO};
    std::int64_t occurred_utc_ms{0};
    std::int64_t occurred_monotonic_us{0};
    std::string source{};
    bool active{true};               // true=活动故障；清除后进入历史
    bool recovered{false};           // 恢复状态（根因已清除）
};

// 历史故障查询条件；空 optional 表示该维度不过滤
struct FaultQuery {
    std::optional<std::string> code{};
    std::optional<std::string> source{};
    std::optional<std::int64_t> occurred_since_utc_ms{};
    std::optional<std::int64_t> occurred_until_utc_ms{};
};

// ---------------------------------------------------------------------------
// 诊断快照（POD；活动故障明细经 IFaultRegistry 查询）
// ---------------------------------------------------------------------------
struct DiagnosticsSnapshot {
    bool config_loaded{false};
    std::uint64_t config_version{0};
    bool writer_active{false};
    std::uint64_t active_fault_count{0};
    std::uint64_t history_fault_count{0};
    std::uint64_t dropped_event_count{0};    // 队列满丢最旧累计
    std::uint64_t buffered_event_count{0};   // 配置就绪前内存缓冲中的事件数
    std::uint64_t written_event_count{0};
    std::uint64_t rotation_count{0};
    std::int64_t generated_utc_ms{0};
    std::int64_t generated_monotonic_us{0};
};

} // namespace marshland::diagnostics
