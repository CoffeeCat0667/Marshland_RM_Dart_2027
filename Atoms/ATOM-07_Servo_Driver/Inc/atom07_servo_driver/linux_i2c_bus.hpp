// ============================================================================
// linux_i2c_bus.hpp — II2cBus 的 Linux /dev/i2c-N 实现
//
// 装配层负责构造并保证寿命；内核只接收 II2cBus&。
// 单次事务超时 = timing.iic_comm_timeout_ms（裁决：单次事务超时，200 ms）。
//
// 注意：Linux 头文件只出现在 .cpp 中，本头文件保持平台无关。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "atom07_servo_driver/i2c_bus.hpp"

namespace marshland::atom07 {

class LinuxI2cBus final : public II2cBus {
public:
    static constexpr std::size_t kMaxPayloadBytes = 32;

    LinuxI2cBus(std::string device_path,
                std::uint8_t device_address,
                unsigned transaction_timeout_ms) noexcept;
    ~LinuxI2cBus() noexcept override;

    LinuxI2cBus(const LinuxI2cBus&) = delete;
    LinuxI2cBus& operator=(const LinuxI2cBus&) = delete;

    // 打开设备并绑定从机地址；重复调用幂等。
    bool open() noexcept;

    I2cStatus write(std::uint8_t device_address, std::uint8_t reg,
                    const std::uint8_t* data, std::size_t size) noexcept override;
    I2cStatus read(std::uint8_t device_address, std::uint8_t reg,
                   std::uint8_t* out, std::size_t size) noexcept override;
    I2cStatus writeRaw(std::uint8_t device_address,
                       const std::uint8_t* data, std::size_t size) noexcept override;

    bool isOpen() const noexcept override;
    void close() noexcept override;

    int lastSystemError() const noexcept { return last_system_error_; }
    unsigned transactionTimeoutMs() const noexcept { return timeout_ms_; }

private:
    bool selectAddress(std::uint8_t device_address) noexcept;
    bool waitReady(bool readable) noexcept; // select() 超时即返回 false 并置 ETIMEDOUT

    std::string device_path_;
    std::uint8_t address_{0};
    unsigned timeout_ms_{200};
    int fd_{-1};
    int selected_address_{-1};
    int last_system_error_{0};
};

} // namespace marshland::atom07
