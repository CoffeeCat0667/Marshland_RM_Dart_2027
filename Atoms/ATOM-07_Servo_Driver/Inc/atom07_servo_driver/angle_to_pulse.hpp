// ============================================================================
// angle_to_pulse.hpp — 角度 -> 脉宽 -> PCA9685A 12 位计数值（纯函数）
//
// 依据：
//   * Design/Config.md §9.1 / §9.3：脉宽范围与角度范围来自配置；
//     out_of_range_action = REJECT_AND_WARN -> 越界必须返回失败，禁止隐式钳位。
//   * Rule/ConfigSchema.md：servo.model.pulse_min_us / pulse_max_us /
//     angle_range_deg / servo.pca9685.pwm_resolution_bits / pwm_frequency_hz。
//
// 本文件为纯逻辑：不访问硬件、不依赖 IConfigView、无状态。
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland::atom07 {

struct PulseConversion final {
    bool ok{false};
    double pulse_us{0.0};
    std::uint16_t off_ticks{0};
};

// ---------------------------------------------------------------------------
// 线性映射：
//   pulse_us = pulse_min + (angle - angle_min) / (angle_max - angle_min) * (pulse_max - pulse_min)
//   off      = round(pulse_us / period_us * steps)，period_us = 1e6 / pwm_frequency_hz
//
// 失败条件（返回 ok = false，不产生任何输出）：
//   * 参数非法（范围反向、分辨率越界、频率非正、脉宽与周期矛盾）；
//   * 角度超出 [angle_min_deg, angle_max_deg]。
// ---------------------------------------------------------------------------
PulseConversion angleToOffTicks(double angle_deg,
                                int angle_min_deg,
                                int angle_max_deg,
                                int pulse_min_us,
                                int pulse_max_us,
                                int resolution_bits,
                                double pwm_frequency_hz) noexcept;

} // namespace marshland::atom07
