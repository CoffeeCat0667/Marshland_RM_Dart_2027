// ============================================================================
// socketcan_transport.hpp — Linux SocketCAN 传输实现声明
//
// 权威来源：Design/Config.md §6.1（接口名/波特率/过滤/错误帧/经典 CAN）
//           Design/Atomic/ATOM-04_CAN.md 传输层要求（发送必须内核确认后成功）
//
// 平台边界：<linux/can.h> 只出现在 .cpp；本头文件不泄漏任何 SocketCAN 类型，
// 以便上层与 Windows 侧只做语法检查时不引入 Linux 依赖。
//
// 依赖：Atoms/Public/CanTypes.hpp、CanInterfaces.hpp 与 C++ 标准库。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>

#include "CanInterfaces.hpp"
#include "CanTypes.hpp"

namespace marshland::atom04 {

class LinuxSocketCanTransport final : public marshland::can::ICanTransport {
public:
    LinuxSocketCanTransport() noexcept = default;
    ~LinuxSocketCanTransport() override;

    LinuxSocketCanTransport(const LinuxSocketCanTransport&) = delete;
    LinuxSocketCanTransport& operator=(const LinuxSocketCanTransport&) = delete;

    marshland::can::CanIoResult open(const char* interface_name) noexcept override;
    void close() noexcept override;
    marshland::can::CanIoResult receive(marshland::can::CanFrame& out, int timeout_ms) noexcept override;
    marshland::can::CanIoResult send(const marshland::can::CanFrame& frame) noexcept override;
    marshland::can::CanBusStatus busStatus() const noexcept override;

    bool isOpen() const noexcept { return socket_fd_.load(std::memory_order_acquire) >= 0; }

    // errno -> CanError 集中映射（public 静态，便于上层配合同样的分类）。
    static marshland::can::CanError mapErrno(int error_number) noexcept;

private:
    std::atomic<int> socket_fd_{-1};
    std::atomic<bool> opened_{false};
    mutable std::atomic<unsigned long long> rx_count_{0ULL};
    mutable std::atomic<unsigned long long> tx_count_{0ULL};
    mutable std::atomic<unsigned long long> tx_fail_count_{0ULL};
    mutable std::atomic<int> last_error_code_{0};
    mutable std::atomic<int> last_errno_{0};
};

} // namespace marshland::atom04
