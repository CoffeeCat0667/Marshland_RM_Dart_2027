// ============================================================================
// reload_feed_config.hpp — ATOM-10 配置装载（IConfigView -> 参数）
//
// 键名遵循 Rule/ConfigSchema.md（v1.9）；数值权威为 Design/Config.md §4/§5.7。
// 任一键非 ConfigStatus::OK 即整体失败：禁止使用未校验值，禁止臆造默认值，
// 禁止为 [待定] 项填值。
// ============================================================================
#pragma once

#include "IConfigView.hpp"

namespace marshland::atom10 {

struct ReloadFeedConfig {
    int speed_rpm{0};          // reload.speed_rpm          换弹速度目标幅值（>0）
    int lift_run_s{0};         // timing.reload_lift_run_s  抬起运行时间（>0）
    int lower_run_s{0};        // timing.reload_lower_run_s 放下运行时间（>0）
    int feed_hold_ms{0};       // feed.hold_ms              换弹位保持时间（>=0）
    int feed_max_action_ms{0}; // timing.feed_max_action_ms 供弹最大动作时间（>0）
};

// 方向约定（2026-10-15 用户裁决：单幅值键 + 代码内方向）：
//   抬起：左 +speed_rpm，右 -speed_rpm
//   放下：左 -speed_rpm，右 +speed_rpm

// 逐项查询并检查 ConfigStatus；全部 OK 且范围有效才返回 true 并写出 out。
// 失败时 failed_key / failed_status 记录第一个非 OK 或越界的键，out 不被修改。
bool LoadReloadFeedConfig(const config::IConfigView& view,
                          ReloadFeedConfig& out,
                          const char*& failed_key,
                          config::ConfigStatus& failed_status) noexcept;

} // namespace marshland::atom10
