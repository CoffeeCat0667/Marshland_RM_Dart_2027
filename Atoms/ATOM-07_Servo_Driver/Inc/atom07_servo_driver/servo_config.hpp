// ============================================================================
// servo_config.hpp — ATOM-07 配置视图（IConfigView -> 纯数据）
//
// 依据：
//   * Rule/ConfigSchema.md §2.10（servo.pca9685.* / servo.model.* / servo.channels[]）
//   * Design/Config.md §9（数值唯一来源）
//   * Rule/AtomIntegration.md §5（必须检查 ConfigStatus；禁止臆造默认值；
//     [待定]/null 必须如实失败，禁止替换为猜测值）
//
// 归口裁决（用户 2026-10-14）：舵机资源以 servo.channels[] 为唯一来源；
//   供弹舵机 = 通道 4，发射舵机 = 通道 5。本文件只读取配置，不硬编码 4/5。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "IConfigView.hpp"
#include "atom07_servo_driver/servo_types.hpp"

namespace marshland::atom07 {

struct ServoSlotConfig final {
    int servo_id{0};            // servo.channels[].id；null（发射舵机）时为 0
    ServoMechanism mechanism{ServoMechanism::Unknown};
    int channel{-1};            // servo.channels[].channel；null -> -1（未接入）

    bool wired() const noexcept { return channel >= 0; }
};

struct ServoConfig final {
    std::string i2c_device;          // servo.pca9685.i2c_device
    int device_address{0};           // servo.pca9685.address（解析后的 7 位地址）
    int channel_count{0};            // servo.pca9685.channel_count
    int pwm_resolution_bits{0};      // servo.pca9685.pwm_resolution_bits
    double pwm_frequency_hz{0.0};    // servo.pca9685.pwm_frequency_hz
    int channel_min{0};              // servo.pca9685.channel_range[0]
    int channel_max{-1};             // servo.pca9685.channel_range[1]
    int pulse_min_us{0};             // servo.model.pulse_min_us
    int pulse_max_us{0};             // servo.model.pulse_max_us
    int angle_min_deg{0};            // servo.model.angle_range_deg[0]
    int angle_max_deg{0};            // servo.model.angle_range_deg[1]
    std::uint32_t iic_comm_timeout_ms{0}; // timing.iic_comm_timeout_ms（单次事务超时）
    std::vector<ServoSlotConfig> slots;   // servo.channels[]

    bool channelInRange(int channel) const noexcept
    {
        return channel >= channel_min && channel <= channel_max;
    }
};

// 失败时返回具体键名与 ConfigStatus，由调用方（适配器）组合可定位的错误消息；
// 本结构不含堆分配，函数可为 noexcept。
struct ServoConfigLoadResult final {
    bool success{false};
    const char* failed_key{nullptr};
    marshland::config::ConfigStatus status{marshland::config::ConfigStatus::NOT_LOADED};
};

ServoConfigLoadResult LoadServoConfig(const marshland::config::IConfigView& view,
                                      ServoConfig& output) noexcept;

// 按 servo.channels[].id 查找（id <= 0 时返回 nullptr）。
const ServoSlotConfig* FindSlotById(const ServoConfig& config, int servo_id) noexcept;

// 按机构查找第一个匹配槽位。
const ServoSlotConfig* FindSlotByMechanism(const ServoConfig& config,
                                           ServoMechanism mechanism) noexcept;

} // namespace marshland::atom07
