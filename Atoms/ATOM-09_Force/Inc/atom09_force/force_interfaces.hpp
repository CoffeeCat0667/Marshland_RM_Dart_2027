// ============================================================================
// force_interfaces.hpp — ATOM-09 声明的最小依赖能力（模块私有窄接口）
//
// 依据 Rule/AtomIntegration.md §6.2「优先接口注入」：
//   调用方只声明"我需要什么"，由装配层把 ATOM-05 / ATOM-14 适配到这些端口。
//   因此 ATOM-05 / ATOM-14 无需包含 ATOM-09 的任何头文件。
//
// 约束：
//   * 全部方法 noexcept，且必须在控制周期预算内返回；
//   * 实现方禁止回调 ATOM-09（避免形成环）。
// ============================================================================
#pragma once

#include <cstdint>

#include "ForceTypes.hpp"

namespace marshland::atom09 {

// 变力电机的四个最小能力（对应 PLAN.md §3.3）。
class IMotorCommandSink {
public:
    virtual ~IMotorCommandSink() = default;

    // 提交速度闭环目标（rpm）。返回 false 表示外层拒绝或提交失败。
    virtual bool submitVelocityTarget(int motor_id, std::int32_t rpm) noexcept = 0;

    // 撤销该电机目标并走**零力矩**锁存路径（不是 0 rpm 闭环目标）。
    virtual bool stopMotor(int motor_id) noexcept = 0;

    // 读取该电机最近一次有效反馈；false 表示当前无有效反馈。
    virtual bool readFeedback(int motor_id, marshland::force::ForceMotorFeedback& out) noexcept = 0;

    // 该电机是否已锁存电机/编码器/PID/CAN 故障（判定归 ATOM-05）。
    virtual bool faultLatched(int motor_id) noexcept = 0;
};

// 只上报"变力动作失败"这一事实。
//
// 刻意**不携带故障码**：Config.md §14.2 的故障码分别属于 ATOM-05/04/14，
// 本模块不得臆造跨模块故障码（Q-17）。明细以文本形式提供，由装配层映射。
class IForceFaultSink {
public:
    virtual ~IForceFaultSink() = default;
    virtual void onForceActionFailed(const char* detail) noexcept = 0;
};

} // namespace marshland::atom09
