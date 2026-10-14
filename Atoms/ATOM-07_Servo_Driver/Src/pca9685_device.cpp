#include "atom07_servo_driver/pca9685_device.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <thread>

#include "atom07_servo_driver/pca9685_registers.hpp"

namespace marshland::atom07 {

Pca9685Device::Pca9685Device(II2cBus& bus, std::uint8_t device_address) noexcept
    : bus_(bus), address_(device_address)
{
}

void Pca9685Device::resetFault() noexcept
{
    fault_kind_ = ServoFaultKind::None;
    last_system_error_ = 0;
}

bool Pca9685Device::finish(const I2cStatus& status, ServoFaultKind fallback_kind) noexcept
{
    if (status.ok) {
        last_system_error_ = 0;
        return true;
    }
    last_system_error_ = status.system_error;
    fault_kind_ = (status.system_error == ETIMEDOUT) ? ServoFaultKind::Timeout : fallback_kind;
    return false;
}

bool Pca9685Device::writeReg8(std::uint8_t reg, std::uint8_t value) noexcept
{
    return finish(bus_.write(address_, reg, &value, 1U), ServoFaultKind::IicError);
}

bool Pca9685Device::readReg8(std::uint8_t reg, std::uint8_t& value) noexcept
{
    return finish(bus_.read(address_, reg, &value, 1U), ServoFaultKind::IicError);
}

bool Pca9685Device::writeAllChannelsOff() noexcept
{
    const std::array<std::uint8_t, 4U> data{0U, 0U, 0U, 0U};
    return finish(bus_.write(address_, pca9685::kAllLedOnLow, data.data(), data.size()),
                  ServoFaultKind::IicError);
}

bool Pca9685Device::initialize(double frequency_hz, int resolution_bits) noexcept
{
    ready_ = false;
    fault_kind_ = ServoFaultKind::None;
    last_system_error_ = 0;

    if (resolution_bits < 1 || resolution_bits > 16) {
        fault_kind_ = ServoFaultKind::InitFailed;
        last_system_error_ = EINVAL;
        return false;
    }
    if (!bus_.isOpen()) {
        fault_kind_ = ServoFaultKind::InitFailed;
        last_system_error_ = ENODEV;
        return false;
    }

    // ---- 1. 软件复位（用户裁决 #6：允许 SWRST，单芯片场景） ------------------
    const std::uint8_t swrst = pca9685::kSoftwareResetCommand;
    if (!finish(bus_.writeRaw(pca9685::kGeneralCallAddress, &swrst, 1U),
                ServoFaultKind::InitFailed)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // ---- 2. 回读 MODE1：确认总线可读（回读校验） -----------------------------
    std::uint8_t mode1 = 0U;
    if (!readReg8(pca9685::kMode1, mode1)) {
        fault_kind_ = ServoFaultKind::InitFailed;
        return false;
    }

    // ---- 3. 唤醒 + 自动增量；MODE2 = 推挽输出 -------------------------------
    const std::uint8_t wake_mode1 = static_cast<std::uint8_t>(
        (mode1 & static_cast<std::uint8_t>(~pca9685::kMode1Sleep)) | pca9685::kMode1AutoIncrement);
    if (!writeReg8(pca9685::kMode1, wake_mode1)) {
        fault_kind_ = ServoFaultKind::InitFailed;
        return false;
    }
    if (!writeReg8(pca9685::kMode2, pca9685::kMode2TotemPole)) {
        fault_kind_ = ServoFaultKind::InitFailed;
        return false;
    }

    // ---- 4. PWM 频率（含 PRE_SCALE 回读校验） -------------------------------
    if (!setFrequency(frequency_hz)) {
        fault_kind_ = ServoFaultKind::InitFailed;
        return false;
    }

    // ---- 5. 回读 MODE1：AI 置位且 SLEEP 清除（回读校验） --------------------
    std::uint8_t verified_mode1 = 0U;
    if (!readReg8(pca9685::kMode1, verified_mode1)) {
        fault_kind_ = ServoFaultKind::InitFailed;
        return false;
    }
    if ((verified_mode1 & pca9685::kMode1AutoIncrement) == 0U ||
        (verified_mode1 & pca9685::kMode1Sleep) != 0U) {
        fault_kind_ = ServoFaultKind::InitFailed;
        last_system_error_ = EIO;
        return false;
    }

    // ---- 6. 全部通道关闭（上电后不输出未知脉宽） ----------------------------
    if (!writeAllChannelsOff()) {
        fault_kind_ = ServoFaultKind::InitFailed;
        return false;
    }

    ready_ = true;
    fault_kind_ = ServoFaultKind::None;
    return true;
}

bool Pca9685Device::setFrequency(double frequency_hz) noexcept
{
    std::uint8_t prescale = 0U;
    if (!pca9685::computePrescale(frequency_hz, prescale)) {
        fault_kind_ = ServoFaultKind::InitFailed;
        last_system_error_ = EINVAL;
        return false;
    }

    std::uint8_t old_mode1 = 0U;
    if (!readReg8(pca9685::kMode1, old_mode1)) {
        return false;
    }
    const std::uint8_t awake_mode1 = static_cast<std::uint8_t>(
        old_mode1 & static_cast<std::uint8_t>(~pca9685::kMode1Sleep));

    // 标准 PRE_SCALE 写时序
    if (!writeReg8(pca9685::kMode1, static_cast<std::uint8_t>(
            (old_mode1 & static_cast<std::uint8_t>(~pca9685::kMode1Restart)) | pca9685::kMode1Sleep))) {
        return false;
    }
    if (!writeReg8(pca9685::kPreScale, prescale)) {
        return false;
    }
    if (!writeReg8(pca9685::kMode1, awake_mode1)) {
        return false;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    if (!writeReg8(pca9685::kMode1, static_cast<std::uint8_t>(awake_mode1 | pca9685::kMode1Restart))) {
        return false;
    }

    // 回读 PRE_SCALE 校验
    std::uint8_t readback = 0U;
    if (!readReg8(pca9685::kPreScale, readback)) {
        return false;
    }
    if (readback != prescale) {
        fault_kind_ = ServoFaultKind::InitFailed;
        last_system_error_ = EIO;
        return false;
    }
    return true;
}

bool Pca9685Device::writeChannelOffTicks(std::uint8_t channel, std::uint16_t off_ticks) noexcept
{
    if (!ready_ || channel >= static_cast<std::uint8_t>(pca9685::kChannelCountMax)) {
        last_system_error_ = EINVAL;
        return false;
    }
    const std::array<std::uint8_t, 4U> data{
        0U,
        0U,
        static_cast<std::uint8_t>(off_ticks & 0xFFU),
        static_cast<std::uint8_t>((off_ticks >> 8U) & 0x0FU),
    };
    const std::uint8_t base =
        static_cast<std::uint8_t>(pca9685::kLed0OnLow + (channel * 4U));
    return finish(bus_.write(address_, base, data.data(), data.size()),
                  ServoFaultKind::IicError);
}

} // namespace marshland::atom07
