// ============================================================================
// Motor.hpp — CAN 电机闭环控制跨 ATOM 公共契约
//
// 归属：Atoms/Public/。生产方：ATOM-04_CAN_Motor_Control。
// 消费方：装配层，以及任何需要下达目标转速或读取电机状态的 ATOM。
//
// 为什么公开契约必须放在 Atoms/Public/ 而不是模块目录内：
//   契约放在 `ATOM-04_.../Inc/atom04_can/` 里，会让其它 ATOM 误以为它是该模块的
//   私有实现细节，从而不敢依赖、或各写一份重复实现。放在共享契约区后，"哪些是
//   对外公开面"在目录层面即可判定，无需阅读模块源码。
//   本文件**只有声明**，没有实现；实现见
//   Atoms/ATOM-04_CAN_Motor_Control/Src/can_motor_control.cpp。
//
// 依赖方向（不可协商）：仅 Atoms/Public/CanTypes.hpp 与 C++ 标准库。
//   禁止包含 AppCoordinator.hpp、任何 ATOM 私有头、任何 SocketCAN 平台头。
//
// !! 安全声明（用户 2026-10-16 裁决）!!
//   实现方 ATOM-04 **不提供任何安全网**：stop() 不发送零力矩帧；驱动周期内的发送
//   失败被静默忽略、无任何上报通道。急停、失联、堵转等安全职责由调用方承担。
// ============================================================================
#pragma once

#include <cstdint>

#include "CanTypes.hpp"

namespace marshland::motor {

// ---------------------------------------------------------------------------
// 目标转速范围与驱动周期（用户 2026-10-16 指定）
// ---------------------------------------------------------------------------
inline constexpr std::int16_t kMinTargetRpm = -6800;
inline constexpr std::int16_t kMaxTargetRpm = 6800;
inline constexpr int kDrivePeriodMs = 20;

// ---------------------------------------------------------------------------
// 单个电机的一组 PID 参数：在 start() 一次性传入，数组顺序对应节点 0x201~0x206。
// ---------------------------------------------------------------------------
struct MotorPidParams final {
    double kp{0.0};
    double ki{0.0};
    double kd{0.0};
    double max{0.0};   // 电流指令上限
    double min{0.0};   // 电流指令下限
};

// ---------------------------------------------------------------------------
// 单个电机的对外状态快照（由 state() 原子拷出，可跨线程读取）。
// 反馈字节布局见 Atoms/ATOM-04_CAN_Motor_Control/Inc/atom04_can/motor_frame_codec.hpp。
// ---------------------------------------------------------------------------
struct MotorState final {
    std::uint32_t node_id{0};        // 0x201~0x206
    bool online{false};              // 是否至少收到过一帧合法反馈（不随超时清零）
    std::int16_t rpm{0};             // 反馈字节 2..3：实际转速
    std::int16_t rotor_position{0};  // 反馈字节 0..1：转子机械角度（0~8191）
    std::int16_t current{0};         // 反馈字节 4..5：实际电流
    std::uint8_t temperature{0};     // 反馈字节 6：温度（℃）
    std::uint64_t timestamp_us{0};   // 该帧的接收时刻（单调时钟）
};

// ---------------------------------------------------------------------------
// IMotorControl —— 电机闭环控制公共端口
//
// 两个功能入口（用户指定）：
//   1. start()  以独立线程启动 CAN 接收；线程内按 kDrivePeriodMs 周期执行 PID，
//               并把结果作为电流写入 0x200 / 0x1FF 控制帧发出。
//   2. setRpm() 设置目标转速；真实 CAN 帧里的电流值由实现内部换算，不由此函数直接发出。
// 另有两个物理上无法省略的辅助：
//   state()     读取状态结构体（没有它就无法"暴露结构体"）；
//   stop()/isRunning()  生命周期（没有它就无法终止接收线程）。
//
// 线程契约：
//   * setRpm() / state() 可被任意线程调用；
//   * start() / stop() 必须串行调用，且不得与正在运行的驱动周期并发。
// ---------------------------------------------------------------------------
class IMotorControl {
public:
    virtual ~IMotorControl() = default;

    // gains 共 can::kMaxMotorNodes (6) 组，顺序对应节点 0x201~0x206。
    // 任一组 max <= min、或存在非有限值时返回 PARAM_INVALID（不做静默降级）。
    // 幂等：已启动时再次调用返回 OK，不会创建第二个线程。
    virtual can::CanResult start(
        const char* interface_name,
        const MotorPidParams (&gains)[can::kMaxMotorNodes]) noexcept = 0;

    // node_id 必须落在 0x201~0x206；rpm 必须落在 [kMinTargetRpm, kMaxTargetRpm]，
    // 越界返回 PARAM_INVALID。未启动时返回 NOT_OPEN。
    // 首次成功调用即视为使能该电机的闭环；在此之前该电机的电流输出恒为 0。
    virtual can::CanResult setRpm(std::uint32_t node_id, std::int16_t rpm) noexcept = 0;

    // 原子地拷出某电机的状态快照；node_id 非法返回 PARAM_INVALID。
    virtual can::CanResult state(std::uint32_t node_id, MotorState& out) const noexcept = 0;

    // 幂等、noexcept。只停止线程并关闭接口，**不发送任何 CAN 帧**（见文件头安全声明）。
    virtual void stop() noexcept = 0;

    virtual bool isRunning() const noexcept = 0;
};

} // namespace marshland::motor
