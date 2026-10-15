// ============================================================================
// fire_config.hpp — ATOM-11 配置加载
//
// 键名唯一来源 Rule/ConfigSchema.md；数值以用户裁决与 Design/Config.md 为准。
// 发射舵机为 PWM 舵机、无 id（用户裁决）：按 mechanism == "FIRE" 在
// servo.channels 数组中定位槽位（禁止硬编码下标），角度键：
//   servo.channels[].standby_deg  待机角（当前 90）
//   servo.channels[].action_deg   发射角（当前 180）
// 时序键：
//   timing.fire_angle_hold_ms     发射角保持时间（100 ms，唯一权威键；
//                                 fire.hold_ms 不读取）
//   timing.fire_max_action_ms     发射机构最大动作时间（300 ms）
//
// 任何键查询非 ConfigStatus::OK 即如实失败，禁止臆造默认值。
// ============================================================================
#pragma once

#include <string>

#include "IConfigView.hpp"

namespace marshland::atom11 {

struct FireConfig {
    int standby_deg{0};
    int fire_deg{0};
    int hold_ms{0};
    int max_action_ms{0};
};

struct FireConfigLoadResult {
    bool success{false};
    std::string error_message;
};

FireConfigLoadResult LoadFireConfig(const marshland::config::IConfigView& view,
                                    FireConfig& out);

} // namespace marshland::atom11
