// ============================================================================
// control_loop.hpp — ATOM-08_Control 唯一对外可见的装配入口（仅装配层使用）
//
// 功能（用户裁决，严格限定，不多做一件事）：
//   1. 经公共头 IStateMachine 取当前控制源（SBUS / WEB / AUTO）：
//        marshland::state::getGlobalStateMachine()->control_source()
//   2. 从对应控制源的**数据发布源**取 scaled_channels：
//        SBUS → sbus::SbusData   （Atoms/Public/Sbus.hpp）
//        WEB  → web::WebData     （Atoms/Public/Web.hpp）
//        AUTO → auto_fire::AutoData（Atoms/Public/Auto.hpp）
//   3. 按 PLAN §17 登记的通道 → 执行器映射查出速度/角度；
//   4. 电机直调公共头 Motor.hpp 的 IMotorControl::setRpm(node_id, rpm)；
//      舵机直调公共头 Servo.hpp 的 IServoDriver::writeChannelAngle(channel, deg)。
//
// 本模块**不**做：控制权仲裁（D4）、失联处理（D3）、配置读取（D10）、
// 状态对外发布、任何对外接口/数据；也不创建线程（D9：上层用本函数起线程）。
//
// 用户裁决记录：
//   D1 scaled 值已经是 rpm，无需再换算；
//   D2 中位/零值一律写 0 rpm 闭环（不是零力矩）；
//   D6 目标越出 ATOM-04 范围时**拒绝并保持旧目标**（不写、不钳位）；
//   D9 只封装成一个函数，线程由上层创建、stop 标志由上层给出；
//   D11 直接调用公共头 Motor.hpp 的 setRpm；
//   舵机：开火 CH7 < 0 → 发射角 180°（否则待机 90°，不做帧计数/保持计时）；
//         供弹 CH13 < 0 → 供弹位 70°（否则待机 90°）。
//
// 控制源契约（2026-10-17 磁盘现状）：
//   旧的 app::IStateReader → ArbitrationContext::control_source 链路已随 ATOM-02
//   重构失效（AppCoordinator 仍依赖已删除的 state_arbitration::StateArbitrator）。
//   现行契约是 marshland::state::IStateMachine（含全局访问器），枚举为
//   marshland::state::ControlSource { AUTO, WEB, SBUS }（**没有 NONE**）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>

#include "Auto.hpp"
#include "IStateMachine.hpp"
#include "Motor.hpp"
#include "Sbus.hpp"
#include "Servo.hpp"
#include "Web.hpp"

namespace marshland::atom08 {

// 阻塞循环：每个 period_ms 推进一次，stop_requested 置位后返回。
// 依赖全部由装配层以引用注入，本模块不持有其生命周期、不创建线程。
// period_ms 为 0 时按 motor::kDrivePeriodMs（CAN 驱动周期）节流。
// 三个数据发布源均由装配层持有；其发布者分别是 ATOM-03 / ATOM-16 / ATOM-13。
void RunControlLoop(marshland::sbus::SbusData& sbus,
                    marshland::web::WebData& web,
                    marshland::auto_fire::AutoData& auto_data,
                    marshland::motor::IMotorControl& motors,
                    marshland::servo::IServoDriver& servos, std::uint32_t period_ms,
                    const std::atomic<bool>& stop_requested) noexcept;

} // namespace marshland::atom08
