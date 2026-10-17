// ============================================================================
// Servo.cpp — ATOM-07_Servo_Driver 实现（最小驱动）
//
// 只做三件事：
//   1. open(i2c_device, address)：打开 IIC 设备（/dev/i2c-N + ioctl(I2C_SLAVE)），
//      复位并初始化 PCA9685A，写入 50Hz 的 PRE_SCALE；
//   2. writeChannelAngle(channel, angle_deg)：外部传入通道与角度 → 脉宽 → 12 位
//      计数值（占空比）→ 写该通道 ON/OFF 寄存器；
//   3. close()：全通道停止输出并关闭文件描述符。
//
// 契约声明见 Atoms/Public/Servo.hpp；私有接口与寄存器常量见
// Inc/atom07_servo_driver/ServoInternal.hpp。
// 目标平台：Ubuntu ARM（Linux IIC）。
// ============================================================================
#include "atom07_servo_driver/ServoInternal.hpp"

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/i2c-dev.h>
#endif

namespace marshland::servo {
namespace {

using namespace detail;

// 复位/唤醒后的振荡器稳定时间（数据手册要求 ≥ 500 us）。
constexpr auto kOscillatorSettle = std::chrono::microseconds(500);

void setError(char (&buffer)[kErrorTextCapacity], const char* text) noexcept
{
    const char* message = (text == nullptr) ? "servo driver error" : text;
    // 空指针与空串统一为可读文本，避免把 "(null)" 暴露给调用方。
    if (message[0] == '\0') {
        message = "servo driver error";
    }
    std::snprintf(buffer, kErrorTextCapacity, "%s", message);
}

void setErrnoError(char (&buffer)[kErrorTextCapacity], const char* prefix, int error) noexcept
{
    const char* reason = std::strerror(error);
    std::snprintf(buffer, kErrorTextCapacity, "%s (errno=%d: %s)",
                  (prefix == nullptr) ? "IIC operation failed" : prefix, error,
                  (reason == nullptr) ? "unknown error" : reason);
}

} // namespace

namespace detail {

bool calcPrescale(double frequency_hz, std::uint8_t& out) noexcept
{
    if (!std::isfinite(frequency_hz) || frequency_hz <= 0.0) {
        return false;
    }
    const double raw =
        std::round(kOscillatorHz / (static_cast<double>(kPwmSteps) * frequency_hz)) - 1.0;
    if (raw < static_cast<double>(kPreScaleMin) || raw > static_cast<double>(kPreScaleMax)) {
        return false;
    }
    out = static_cast<std::uint8_t>(raw);
    return true;
}

bool angleToOffTicks(double angle_deg, std::uint16_t& out) noexcept
{
    if (!std::isfinite(angle_deg)) {
        return false;
    }
    if (angle_deg < static_cast<double>(kAngleMinDeg) ||
        angle_deg > static_cast<double>(kAngleMaxDeg)) {
        return false;
    }
    const double span_deg = static_cast<double>(kAngleMaxDeg - kAngleMinDeg);
    const double pulse_us = static_cast<double>(kPulseMinUs) +
                            (angle_deg - static_cast<double>(kAngleMinDeg)) / span_deg *
                                static_cast<double>(kPulseMaxUs - kPulseMinUs);
    const double period_us = 1.0e6 / kPwmFrequencyHz;
    const double ticks = std::round(pulse_us / period_us * static_cast<double>(kPwmSteps));
    if (ticks < 0.0 || ticks >= static_cast<double>(kPwmSteps)) {
        return false;
    }
    out = static_cast<std::uint16_t>(ticks);
    return true;
}

} // namespace detail

// ---------------------------------------------------------------------------
// 实现细节：IIC 设备 + PCA9685A 寄存器写入
// ---------------------------------------------------------------------------
struct Pca9685ServoDriver::Impl final {
    int fd{-1};
    std::uint8_t address{0};
    bool ready{false};
    char error[kErrorTextCapacity]{};

    void selectAddress(std::uint8_t value) noexcept
    {
        if (fd < 0 || address == value) {
            return;
        }
        if (::ioctl(fd, I2C_SLAVE, static_cast<long>(value)) < 0) {
            const int saved = errno;
            setErrnoError(error, "ioctl(I2C_SLAVE) failed", saved);
            return;
        }
        address = value;
    }

    bool writeRegs(std::uint8_t reg, const std::uint8_t* data, std::size_t size) noexcept
    {
        if (fd < 0) {
            setError(error, "IIC device is not open");
            return false;
        }
        std::uint8_t frame[1U + kChannelValueBytes]{};
        if (size > kChannelValueBytes) {
            setError(error, "register write payload too large");
            return false;
        }
        frame[0] = reg;
        for (std::size_t i = 0; i < size; ++i) {
            frame[1U + i] = data[i];
        }

        const std::size_t total = 1U + size;
        std::size_t written = 0U;
        while (written < total) {
            const ssize_t result = ::write(fd, frame + written, total - written);
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                // 先保存 errno：后续的格式化输出会覆盖它。
                const int saved = errno;
                setErrnoError(error, "IIC register write failed", saved);
                return false;
            }
            if (result == 0) {
                setError(error, "IIC register write returned 0 bytes");
                return false;
            }
            written += static_cast<std::size_t>(result);
        }
        return true;
    }

    void release() noexcept
    {
        ready = false;
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        address = 0;
    }

    bool initialize(std::uint8_t device_address) noexcept
    {
        // 1) 软件复位（通用呼叫 0x00 ← 0x06），清除上一次运行残留。
        selectAddress(kGeneralCallAddress);
        const std::uint8_t reset_command = kSoftwareResetCommand;
        if (!writeRegs(0x00, &reset_command, 1U)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // 2) 切到目标器件地址；设置自动增量并进入 SLEEP 以便写 PRE_SCALE。
        selectAddress(device_address);
        const std::uint8_t mode1_sleep = static_cast<std::uint8_t>(kMode1AutoIncrement |
                                                                   kMode1Sleep);
        if (!writeRegs(kRegMode1, &mode1_sleep, 1U)) {
            return false;
        }

        // 3) MODE2：全调用地址使能，输出为推挽（配合外部驱动的舵机负载）。
        const std::uint8_t mode2 = kMode1AllCall;
        if (!writeRegs(kRegMode2, &mode2, 1U)) {
            return false;
        }

        // 4) 写入 50Hz 对应的 PRE_SCALE。
        std::uint8_t prescale = 0;
        if (!calcPrescale(kPwmFrequencyHz, prescale)) {
            setError(error, "50Hz PWM cannot be represented by PRE_SCALE");
            return false;
        }
        if (!writeRegs(kRegPreScale, &prescale, 1U)) {
            return false;
        }

        // 5) 清 SLEEP 唤醒振荡器，等待其稳定。
        const std::uint8_t mode1_active = kMode1AutoIncrement;
        if (!writeRegs(kRegMode1, &mode1_active, 1U)) {
            return false;
        }
        std::this_thread::sleep_for(kOscillatorSettle);

        // 6) 全通道关闭：上电默认输出为低，显式清零避免残留占空比。
        const std::uint8_t zero[kChannelValueBytes] = {0U, 0U, 0U, 0U};
        if (!writeRegs(kRegAllLedOnLow, zero, kChannelValueBytes)) {
            return false;
        }

        ready = true;
        error[0] = '\0';
        return true;
    }
};

// ---------------------------------------------------------------------------
// Pca9685ServoDriver
// ---------------------------------------------------------------------------
Pca9685ServoDriver::Pca9685ServoDriver() noexcept : impl_(std::make_unique<Impl>())
{
}

Pca9685ServoDriver::~Pca9685ServoDriver() noexcept
{
    close();
}

bool Pca9685ServoDriver::open(const char* i2c_device, std::uint8_t address) noexcept
{
    if (impl_ == nullptr) {
        return false;
    }
    Impl& s = *impl_;

    s.release();
    s.error[0] = '\0';

    if (i2c_device == nullptr || i2c_device[0] == '\0') {
        setError(s.error, "IIC device path is null or empty");
        return false;
    }
    if (address == 0U || address > 0x7FU) {
        setError(s.error, "IIC address must be a 7-bit value in [0x01, 0x7F]");
        return false;
    }

    const int fd = ::open(i2c_device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        const int saved = errno;
        setErrnoError(s.error, "open IIC device failed", saved);
        return false;
    }
    s.fd = fd;

    if (!s.initialize(address)) {
        s.release();
        return false;
    }
    return true;
}

bool Pca9685ServoDriver::writeChannelAngle(std::uint8_t channel, double angle_deg) noexcept
{
    if (impl_ == nullptr) {
        return false;
    }
    Impl& s = *impl_;

    if (!s.ready || s.fd < 0) {
        setError(s.error, "servo driver is not open (call open() first)");
        return false;
    }
    if (channel >= kChannelCount) {
        std::snprintf(s.error, kErrorTextCapacity,
                      "channel %u out of range [0, %u]", static_cast<unsigned>(channel),
                      static_cast<unsigned>(kChannelCount - 1U));
        return false;
    }

    std::uint16_t off_ticks = 0;
    if (!angleToOffTicks(angle_deg, off_ticks)) {
        std::snprintf(s.error, kErrorTextCapacity,
                      "angle %.1f out of range [%d, %d] deg", angle_deg, kAngleMinDeg,
                      kAngleMaxDeg);
        return false;
    }

    // ON = 0（周期起点），OFF = 计数值 → 占空比 = off_ticks / 4096。
    const std::uint8_t value[kChannelValueBytes] = {
        0x00U,
        0x00U,
        static_cast<std::uint8_t>(off_ticks & 0xFFU),
        static_cast<std::uint8_t>((off_ticks >> 8U) & 0x0FU),
    };
    const std::uint8_t reg =
        static_cast<std::uint8_t>(kRegLed0OnLow + static_cast<std::uint8_t>(channel * 4U));
    if (!s.writeRegs(reg, value, kChannelValueBytes)) {
        s.ready = false; // IIC 故障后必须重新 open()，避免在下一次调用中静默继续
        return false;
    }

    s.error[0] = '\0';
    return true;
}

void Pca9685ServoDriver::close() noexcept
{
    if (impl_ == nullptr) {
        return;
    }
    Impl& s = *impl_;
    if (s.fd >= 0 && s.ready) {
        const std::uint8_t zero[kChannelValueBytes] = {0U, 0U, 0U, 0U};
        if (!s.writeRegs(kRegAllLedOnLow, zero, kChannelValueBytes)) {
            // 关闭路径上的失败只记录，不改变 close() 的幂等语义。
        }
    }
    s.release();
}

bool Pca9685ServoDriver::isOpen() const noexcept
{
    return impl_ != nullptr && impl_->ready;
}

const char* Pca9685ServoDriver::lastError() const noexcept
{
    if (impl_ == nullptr || impl_->error[0] == '\0') {
        return nullptr;
    }
    return impl_->error;
}

std::unique_ptr<IServoDriver> MakePca9685ServoDriver() noexcept
{
    try {
        return std::make_unique<Pca9685ServoDriver>();
    } catch (...) {
        return nullptr;
    }
}

} // namespace marshland::servo
