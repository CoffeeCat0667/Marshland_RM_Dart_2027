// ============================================================================
// i2c_bus.hpp — IIC 总线窄接口（可注入、可替换测试替身）
//
// 设计要点：
//   * 所有事务返回显式状态（成功/失败 + system_error），禁止静默失败；
//   * 单次事务超时由实现负责（timing.iic_comm_timeout_ms，裁决：单次事务超时）；
//   * 内核不直接持有 Linux fd，硬件访问全部经本接口；
//   * 本接口由装配层注入实现（生产环境为 LinuxI2cBus，判定时为测试替身）。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>

namespace marshland::atom07 {

struct I2cStatus final {
    bool ok{false};
    int system_error{0}; // errno；0 表示无错误
};

class II2cBus {
public:
    virtual ~II2cBus() = default;

    II2cBus(const II2cBus&) = delete;
    II2cBus& operator=(const II2cBus&) = delete;

    // 写寄存器：先发送寄存器地址，再发送 size 字节数据。
    virtual I2cStatus write(std::uint8_t device_address, std::uint8_t reg,
                            const std::uint8_t* data, std::size_t size) noexcept = 0;

    // 读寄存器：先发送寄存器地址，再读取 size 字节。
    virtual I2cStatus read(std::uint8_t device_address, std::uint8_t reg,
                           std::uint8_t* out, std::size_t size) noexcept = 0;

    // 无寄存器地址的裸写（用于 SWRST 通用呼叫：地址 0x00 + 单字节 0x06）。
    virtual I2cStatus writeRaw(std::uint8_t device_address,
                               const std::uint8_t* data, std::size_t size) noexcept = 0;

    virtual bool isOpen() const noexcept = 0;
    virtual void close() noexcept = 0;

protected:
    II2cBus() = default;
};

} // namespace marshland::atom07
