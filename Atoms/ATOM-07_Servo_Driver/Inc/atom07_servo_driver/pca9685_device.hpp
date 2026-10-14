// ============================================================================
// pca9685_device.hpp — PCA9685A 设备层（纯逻辑内核，经 II2cBus 访问硬件）
//
// 职责（Design/Atomic/ATOM-07_Servo_Driver.md 内部原子操作 1~6）：
//   * 打开/初始化（SWRST + 唤醒 + 自动增量 + 推挽输出）
//   * 设置 PWM 频率（标准 PRE_SCALE 时序）
//   * 单通道写入（12 位 OFF 计数值）
//   * 初始化回读校验（MODE1 / PRE_SCALE，用户裁决 #6）
//
// 约束：所有步骤返回显式成功/失败；禁止静默钳位、禁止吞掉写失败。
// ============================================================================
#pragma once

#include <cstdint>

#include "atom07_servo_driver/i2c_bus.hpp"
#include "atom07_servo_driver/servo_types.hpp"

namespace marshland::atom07 {

class Pca9685Device final {
public:
    Pca9685Device(II2cBus& bus, std::uint8_t device_address) noexcept;

    Pca9685Device(const Pca9685Device&) = delete;
    Pca9685Device& operator=(const Pca9685Device&) = delete;

    // SWRST -> 唤醒 + AI -> MODE2 推挽 -> 设置频率 -> 回读校验 -> 全部通道关闭
    bool initialize(double frequency_hz, int resolution_bits) noexcept;

    // 设置 PWM 频率（含 en: 进入 SLEEP -> 写 PRE_SCALE -> 清 SLEEP -> RESTART）并回读校验
    bool setFrequency(double frequency_hz) noexcept;

    // 写单通道 OFF 计数值（ON = 0）
    bool writeChannelOffTicks(std::uint8_t channel, std::uint16_t off_ticks) noexcept;

    bool isReady() const noexcept { return ready_; }
    int lastSystemError() const noexcept { return last_system_error_; }
    ServoFaultKind lastFaultKind() const noexcept { return fault_kind_; }
    void resetFault() noexcept;

private:
    bool writeReg8(std::uint8_t reg, std::uint8_t value) noexcept;
    bool readReg8(std::uint8_t reg, std::uint8_t& value) noexcept;
    bool writeAllChannelsOff() noexcept;
    bool finish(const I2cStatus& status, ServoFaultKind fallback_kind) noexcept;

    II2cBus& bus_;
    std::uint8_t address_{0};
    bool ready_{false};
    int last_system_error_{0};
    ServoFaultKind fault_kind_{ServoFaultKind::None};
};

} // namespace marshland::atom07
