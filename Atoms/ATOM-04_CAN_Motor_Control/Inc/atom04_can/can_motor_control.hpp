// ============================================================================
// can_motor_control.hpp — ATOM-04 实现类：CAN 电机闭环控制
//
// !! 对外契约不在这里 !!
//   公开面（两个入口 + MotorPidParams / MotorState + IMotorControl 端口 + 转速范围
//   与驱动周期常量）声明于 **Atoms/Public/Motor.hpp**。
//   本文件只声明**具体实现类** CanMotorControl，属 ATOM-04 的实现细节：
//   其它 ATOM 若需下达转速或读取电机状态，应依赖 marshland::motor::IMotorControl，
//   而不是本文件。
//
// 数据流：
//   setRpm(节点, 目标转速) ─┐
//                           ├─> [接收线程] 每 20ms：PID(转速误差) -> 电流 -> 组帧发送
//   0x201~0x206 反馈帧 ─────┘        └─> MotorState（转速/温度/电流/位置）供外部读取
//
// !! 与设计文档的偏差（用户 2026-10-16 裁决，详见 PLAN.md §5）!!
//   Design/Atomic/ATOM-04_CAN.md 的单一职责写明「CANAdapter 不承担机构流程判断或
//   PID 计算」，且验收条件要求「接收帧按 Config/设备协议解析」由 ATOM-05 承担。
//   本模块按用户明确指示把 PID 与反馈解析放进 ATOM-04，属**已知且经授权的越界**。
//
// !! 无安全网（用户明确选择"不要安全网，调用方自己负责"）!!
//   * stop() **不发送任何 CAN 帧**；最后一次写出的电流指令会留在电调内，直到电调
//     自身的 CAN 超时机制动作（该行为未在本模块验证）。
//   * 驱动周期内的发送失败被静默忽略，没有任何上报通道。
//   * 急停、失联、越界、堵转等安全职责全部由调用方承担。
//
// 线程安全：
//   * setRpm() / state() 可被任意线程调用（原子量 + 互斥量保护）；
//   * start() / stop() 必须串行调用，且不得与正在运行的驱动周期并发
//     （stop() 会 join 接收线程后再关闭描述符）。
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "CanTypes.hpp"
#include "Motor.hpp"
#include "atom04_can/motor_frame_codec.hpp"
#include "atom04_can/pid.hpp"

namespace marshland::atom04 {

// 实现 marshland::motor::IMotorControl 的具体类。
// 不拷贝、不移动：它持有文件描述符与线程。
class CanMotorControl final : public motor::IMotorControl {
public:
    CanMotorControl() noexcept = default;
    ~CanMotorControl() override;

    CanMotorControl(const CanMotorControl&) = delete;
    CanMotorControl& operator=(const CanMotorControl&) = delete;

    // 见 Atoms/Public/Motor.hpp 的契约说明；下面是实现侧补充。
    can::CanResult start(const char* interface_name,
                         const motor::MotorPidParams (&gains)[can::kMaxMotorNodes]) noexcept override;
    can::CanResult setRpm(std::uint32_t node_id, std::int16_t rpm) noexcept override;
    can::CanResult state(std::uint32_t node_id, motor::MotorState& out) const noexcept override;
    void stop() noexcept override;
    bool isRunning() const noexcept override
    {
        return running_.load(std::memory_order_acquire);
    }

private:
    static std::int64_t nodeIndex(std::uint32_t node_id) noexcept;
    static std::uint64_t nowMicroseconds() noexcept;

    can::CanResult openSocket(const char* interface_name) noexcept;
    void closeSocket() noexcept;
    void receiveLoop() noexcept;
    void drainFrames() noexcept;
    void driveOnce() noexcept;
    can::CanResult sendFrame(const can::CanFrame& frame) noexcept;

    std::atomic<int> socket_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread receive_thread_;

    // PID 只被接收线程触碰；目标转速与使能标志可被任意线程写入。
    std::array<Pid, can::kMaxMotorNodes> pid_{};
    std::array<std::atomic<std::int16_t>, can::kMaxMotorNodes> target_rpm_{};
    std::array<std::atomic<bool>, can::kMaxMotorNodes> enabled_{};

    // 反馈状态表：接收线程写，任意线程经 state() 读，故用互斥量保护。
    mutable std::mutex state_mutex_;
    std::array<motor::MotorState, can::kMaxMotorNodes> states_{};
};

} // namespace marshland::atom04
