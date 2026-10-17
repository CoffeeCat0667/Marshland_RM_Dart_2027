// ============================================================================
// ServoInternal.hpp — ATOM-07 私有实现接口（对外不可见）
//
// 本头只允许本模块（ATOM-07_Servo_Driver）的 .cpp 包含，其他 ATOM 禁止包含：
//   * PCA9685A 寄存器/位常量与器件参数；
//   * 具体实现类 Pca9685ServoDriver 与其 Impl；
//   * 角度换算、预分频计算、IIC 访问等中间函数。
//
// 调用方只依赖 Atoms/Public/Servo.hpp 的 IServoDriver 与工厂函数。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

#include "Servo.hpp"

namespace marshland::servo {
namespace detail {

// ---- PCA9685A 寄存器地址 ---------------------------------------------------
inline constexpr std::uint8_t kRegMode1 = 0x00;
inline constexpr std::uint8_t kRegMode2 = 0x01;
inline constexpr std::uint8_t kRegLed0OnLow = 0x06;   // 每通道 4 字节
inline constexpr std::uint8_t kRegAllLedOnLow = 0xFA;
inline constexpr std::uint8_t kRegPreScale = 0xFE;

// ---- MODE1 / MODE2 位 ------------------------------------------------------
inline constexpr std::uint8_t kMode1AllCall = 0x01;
inline constexpr std::uint8_t kMode1Sleep = 0x10;
inline constexpr std::uint8_t kMode1AutoIncrement = 0x20;

// ---- 软件复位（通用呼叫 0x00 写入 0x06） -----------------------------------
inline constexpr std::uint8_t kGeneralCallAddress = 0x00;
inline constexpr std::uint8_t kSoftwareResetCommand = 0x06;

// ---- 器件参数 --------------------------------------------------------------
inline constexpr double kOscillatorHz = 25000000.0;
inline constexpr int kPwmSteps = 4096;
inline constexpr std::uint8_t kPreScaleMin = 3;
inline constexpr std::uint8_t kPreScaleMax = 255;

inline constexpr std::size_t kChannelValueBytes = 4U;   // ON_L/ON_H/OFF_L/OFF_H
inline constexpr std::size_t kErrorTextCapacity = 192U;

// ---- 中间函数（仅本模块使用） ----------------------------------------------
// prescale = round(25e6 / (4096 * freq)) - 1；越界返回 false。
bool calcPrescale(double frequency_hz, std::uint8_t& out) noexcept;

// 角度 → 脉宽 → 12 位计数值（占空比）；越界返回 false，禁止隐式钳位。
bool angleToOffTicks(double angle_deg, std::uint16_t& out) noexcept;

} // namespace detail

// ---------------------------------------------------------------------------
// 具体实现类：由 MakePca9685ServoDriver() 创建，调用方只见 IServoDriver。
// ---------------------------------------------------------------------------
class Pca9685ServoDriver final : public IServoDriver {
public:
    Pca9685ServoDriver() noexcept;
    ~Pca9685ServoDriver() noexcept override;

    bool open(const char* i2c_device, std::uint8_t address) noexcept override;
    bool writeChannelAngle(std::uint8_t channel, double angle_deg) noexcept override;
    void close() noexcept override;
    bool isOpen() const noexcept override;
    const char* lastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace marshland::servo
