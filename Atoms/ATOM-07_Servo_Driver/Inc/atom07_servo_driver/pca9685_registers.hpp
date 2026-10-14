// ============================================================================
// pca9685_registers.hpp — PCA9685A 寄存器与预分频换算（纯常量 + 纯函数）
//
// 参考来源：Lib/PCA9685A（供应商库副本，仅作参考，不参与编译）。
//   * 预分频时序：进入 SLEEP -> 写 PRE_SCALE -> 清 SLEEP -> 等待振荡器 -> RESTART
//   * 计数值为 12 位（0..4095），换算基于内部 25 MHz 振荡器。
//
// 本文件为 header-only 纯逻辑：不访问硬件、不依赖 IConfigView。
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>

namespace marshland::atom07::pca9685 {

// ---- 寄存器地址 -------------------------------------------------------------
inline constexpr std::uint8_t kMode1       = 0x00;
inline constexpr std::uint8_t kMode2       = 0x01;
inline constexpr std::uint8_t kLed0OnLow   = 0x06;  // LED0_ON_L；每通道 4 字节
inline constexpr std::uint8_t kAllLedOnLow = 0xFA;
inline constexpr std::uint8_t kPreScale    = 0xFE;

// ---- MODE1 / MODE2 位 -------------------------------------------------------
inline constexpr std::uint8_t kMode1Sleep         = 0x10;
inline constexpr std::uint8_t kMode1AutoIncrement = 0x20;
inline constexpr std::uint8_t kMode1Restart       = 0x80;
inline constexpr std::uint8_t kMode2TotemPole     = 0x04;

// ---- 软件复位（SWRST，通用呼叫地址 0x00 写入 0x06） --------------------------
inline constexpr std::uint8_t kGeneralCallAddress  = 0x00;
inline constexpr std::uint8_t kSoftwareResetCommand = 0x06;

// ---- 芯片能力 ---------------------------------------------------------------
inline constexpr int kPwmSteps        = 4096;      // 12 位
inline constexpr int kChannelCountMax = 16;
inline constexpr std::uint8_t kPrescaleMin = 3;
inline constexpr std::uint8_t kPrescaleMax = 255;
inline constexpr double kOscillatorHz = 25000000.0;

// Config.md §9.1：PWM 频率 50 Hz。供应商库支持范围作为可接受区间上界依据。
inline constexpr double kMinFrequencyHz = 24.0;
inline constexpr double kMaxFrequencyHz = 1526.0;

// ---------------------------------------------------------------------------
// prescale = round(25e6 / (4096 * freq)) - 1，必须落在 [3, 255]
// 频率不在可支持区间或换算越界时返回 false（不静默钳位）。
// ---------------------------------------------------------------------------
inline bool computePrescale(double frequency_hz, std::uint8_t& out) noexcept
{
    if (!(frequency_hz >= kMinFrequencyHz) || !(frequency_hz <= kMaxFrequencyHz)) {
        return false;
    }
    const double raw = std::round(kOscillatorHz /
                                  (static_cast<double>(kPwmSteps) * frequency_hz)) - 1.0;
    if (raw < static_cast<double>(kPrescaleMin) || raw > static_cast<double>(kPrescaleMax)) {
        return false;
    }
    out = static_cast<std::uint8_t>(raw);
    return true;
}

// 由 prescale 反算实际频率（Hz），用于回读校验与诊断。
inline double actualFrequency(std::uint8_t prescale) noexcept
{
    return kOscillatorHz /
           (static_cast<double>(kPwmSteps) * (static_cast<double>(prescale) + 1.0));
}

} // namespace marshland::atom07::pca9685
