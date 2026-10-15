// ============================================================================
// charge_config.hpp — ATOM-08 配置读取（键名唯一来源：Rule/ConfigSchema.md）
//
// 只经 marshland::config::IConfigView 查询，禁止自解析 JSON；
// 必须区分 ConfigStatus，非 OK 不使用该值；缺失项如实失败，禁止臆造默认值。
// 本模块**不读取** gpio.*（限位采样/去抖/判据归 ATOM-06）。
// ============================================================================
#pragma once

#include <cstdint>

#include "IConfigView.hpp"

namespace marshland::atom08 {

// 未映射的 SBUS 通道（sbus.functions[] 中 channel 为 null 或该项缺失）
inline constexpr std::int32_t kUnmappedChannel = -1;

struct ChargeConfig final {
    // timing.*
    std::int64_t charge_time_us{0};        // timing.charge_time_s → 完成判据 X
    std::int64_t charge_max_action_us{0};  // timing.charge_max_action_ms → 蓄力最大动作时间
    std::int64_t home_max_action_us{0};    // timing.charge_home_max_action_ms → 回零最大动作时间

    // charge.*
    std::int32_t charge_speed_rpm{0};      // charge.charge_speed_rpm（含方向符号）
    std::int32_t home_speed_rpm{0};        // charge.home_speed_rpm（含方向符号）
    std::int32_t target_range_rpm{0};      // charge.target_range（对称 ±；0 表示未配置，不做范围校验）

    // motor[]：按 role 解析，禁止硬编码电机 ID
    std::uint8_t left_motor_id{0};         // motor[].role == "charge_left"
    std::uint8_t right_motor_id{0};        // motor[].role == "charge_right"

    // sbus.functions[]：按功能名解析通道下标（0 基），禁止硬编码 CH 编号
    std::int32_t sbus_manual_charge_channel{kUnmappedChannel};        // "manual_charge"
    std::int32_t sbus_manual_charge_left_channel{kUnmappedChannel};   // "manual_charge_left"
    std::int32_t sbus_manual_charge_right_channel{kUnmappedChannel};  // "manual_charge_right"
};

struct ChargeConfigLoadResult final {
    bool success{false};
    // 指向静态键名或静态说明，运行期不分配内存
    const char* error_message{"charge configuration not loaded"};
};

ChargeConfigLoadResult LoadChargeConfig(const marshland::config::IConfigView& view,
                                        ChargeConfig& output) noexcept;

} // namespace marshland::atom08
