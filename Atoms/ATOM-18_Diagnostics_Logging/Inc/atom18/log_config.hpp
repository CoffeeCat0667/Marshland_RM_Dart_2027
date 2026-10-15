// ============================================================================
// log_config.hpp — 日志配置加载（模块内部）
//
// 键名权威：Rule/ConfigSchema.md §2.18；数值权威：Design/Config.md §16。
// 加载模式参考 ATOM-03 的 LoadSbusConfig：自由函数 + POD 输出。
//
// 错误处理（对齐 IConfigView 契约）：
//   * NOT_LOADED        → ready=false，调用方保持缓冲模式，不得用默认值开工；
//   * MISSING           → 该键使用 Config §16.1 内置默认值，used_defaults 计数
//                         （Config §13.1：缺失的可选键使用内置默认值）；
//   * INVALID_TYPE / OUT_OF_RANGE → success=false，显式报错，禁止静默兜底。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DiagnosticsTypes.hpp"
#include "IConfigView.hpp"

namespace marshland::atom18 {

enum class StdoutEcho : std::uint8_t {
    NONE,
    CRITICAL_ONLY,   // 同步输出关键告警和错误（WARN 及以上）
    ALL
};

struct LogConfig {
    diagnostics::LogLevel level{diagnostics::LogLevel::INFO};
    std::string output_directory{"./Logs/"};
    StdoutEcho stdout_echo{StdoutEcho::CRITICAL_ONLY};
    bool rotation_enabled{true};
    std::int64_t max_files{10};
    std::int64_t max_file_size_bytes{10ll * 1024 * 1024};
    std::int64_t max_total_size_bytes{100ll * 1024 * 1024};
    std::int64_t fault_history_limit{1000};
    std::string flush_policy{"ERROR_SAFETY_IMMEDIATE_OTHERS_BUFFER_1S"};
    std::vector<std::string> required_events{};
    // 输出字段顺序（Config §16.3）；序列化按此顺序输出
    std::vector<std::string> fields{"timestamp", "level", "module", "event",
                                    "source", "state", "request_id",
                                    "error_code", "payload"};
};

struct ConfigLoadResult {
    bool ready{false};                // 配置视图已加载（非 NOT_LOADED）
    bool success{false};              // 所有出现的键类型/范围合法
    const char* error_message{""};    // success=false 时的键名级原因
    std::uint32_t used_defaults{0};   // 落入内置默认值的键数
};

[[nodiscard]] ConfigLoadResult LoadLogConfig(
    const marshland::config::IConfigView& view, LogConfig& out) noexcept;

} // namespace marshland::atom18
