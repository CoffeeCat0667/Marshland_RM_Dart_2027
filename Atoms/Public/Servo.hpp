// ============================================================================
// Servo.hpp — PCA9685A/IIC 舵机驱动的跨 ATOM 公共契约
//
// 对外只有三件事（中间函数、寄存器常量、具体实现类均不出现在本文件）：
//   1. open(i2c_device, address)：打开 IIC 设备、初始化 PCA9685A 并设定 50Hz PWM；
//   2. writeChannelAngle(channel, angle_deg)：外部传入通道号与角度，换算为合法
//      占空比后写该通道；
//   3. close()：释放 IIC 设备并使各通道停止输出。
//
// 实现位于 ATOM-07_Servo_Driver/（私有头 + Src/Servo.cpp），
// 由装配层经 MakePca9685ServoDriver() 取得实例后注入调用方。
//
// 保持叶子契约：本文件不包含任何 ATOM 依赖，只需 C++ 标准库。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>

namespace marshland::servo {

// ---- 舵机口径（供调用方校验与换算参照，模块内不重复定义） -------------------
inline constexpr double kPwmFrequencyHz = 50.0;   // 舵机标准 50Hz（周期 20 ms）
inline constexpr int kAngleMinDeg = 0;            // 角度下限（度）
inline constexpr int kAngleMaxDeg = 180;          // 角度上限（度）
inline constexpr int kPulseMinUs = 500;           // 0 度对应脉宽
inline constexpr int kPulseMaxUs = 2500;          // 180 度对应脉宽
inline constexpr std::uint8_t kChannelCount = 16; // PCA9685A 通道数上界

// ---------------------------------------------------------------------------
// 舵机驱动契约
//
//   * open() 即“打开 IIC 设备 + 初始化器件 + 设定 50Hz”，成功后驱动可用；
//   * writeChannelAngle() 的通道号与角度均由外部传入；角度越界、通道非法或
//     设备未打开时返回 false，且不产生 IIC 写入（禁止隐式钳位）；
//   * 全部方法 noexcept，错误经 lastError() 以文本给出（nullptr = 无错误）。
// ---------------------------------------------------------------------------
class IServoDriver {
public:
    virtual ~IServoDriver() = default;

    IServoDriver(const IServoDriver&) = delete;
    IServoDriver& operator=(const IServoDriver&) = delete;

    // 打开 IIC 设备并完成初始化；i2c_device 如 "/dev/i2c-7"，
    // address 为 7 位器件地址（如 0x40）。重复调用会先释放上一次的设备。
    virtual bool open(const char* i2c_device, std::uint8_t address) noexcept = 0;

    // 向指定通道写入角度（度）。channel 必须 < kChannelCount；
    // angle_deg 必须落在 [kAngleMinDeg, kAngleMaxDeg]，否则拒绝且不写总线。
    virtual bool writeChannelAngle(std::uint8_t channel, double angle_deg) noexcept = 0;

    // 释放 IIC 设备；幂等，未打开时调用安全。
    virtual void close() noexcept = 0;

    // 是否已成功打开并初始化。
    virtual bool isOpen() const noexcept = 0;

    // 最近一次失败原因；nullptr 表示无错误。
    virtual const char* lastError() const noexcept = 0;

protected:
    IServoDriver() = default;
};

// 工厂：返回生产实现；分配失败返回 nullptr。
std::unique_ptr<IServoDriver> MakePca9685ServoDriver() noexcept;

} // namespace marshland::servo
