// ============================================================================
// force_config.hpp — ATOM-09 的配置解析（IConfigView -> ForceConfig）
//
// 依据：Rule/ConfigSchema.md、Rule/AtomIntegration.md §5、PLAN.md §4
//   * 只经 IConfigView 读取，禁止自解析 JSON；
//   * 逐项检查 ConfigStatus，非 OK 一律视为配置不可用；
//   * 变力电机按 motor[].role == "force" 唯一匹配解析，禁止硬编码 ID/节点（Q-08）。
// ============================================================================
#pragma once

#include <cstdint>

#include "IConfigView.hpp"

namespace marshland::atom09 {

struct ForceConfig final {
    int motor_id{0};                          // motor[].role == "force" 的 id
    std::int32_t target_range_rpm{0};         // force.target_range（对称 ±）
    std::int32_t center_deadband_rpm{0};      // force.center_deadband_rpm
    std::uint32_t timed_action_ms{0};         // timing.force_time_s * 1000
    std::uint32_t max_control_cycle_ms{0};    // timing.max_control_cycle_ms（校验用）
};

struct ForceConfigLoadResult final {
    bool success{false};
    const char* error_message{"force configuration not loaded"};
};

// 失败时 output 保持调用前的值（与 IConfigView 的约定一致）。
ForceConfigLoadResult LoadForceConfig(const marshland::config::IConfigView& view,
                                      ForceConfig& output) noexcept;

} // namespace marshland::atom09
