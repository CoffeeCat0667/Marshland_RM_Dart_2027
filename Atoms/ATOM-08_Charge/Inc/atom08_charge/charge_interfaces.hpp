// ============================================================================
// charge_interfaces.hpp — ATOM-08 自声明的窄能力接口
//
// 依 Rule/AtomIntegration.md §6.2 方案 B：调用方只声明"我需要什么"，
// 由装配层把 ATOM-05 / ATOM-06 / 安全链路适配到这些接口。
// 本模块禁止包含 ATOM-05/06 等私有头，也禁止注入具体 AppCoordinator&。
// ============================================================================
#pragma once

#include <cstdint>

#include "charge_types.hpp"

namespace marshland::atom08 {

// ---------------------------------------------------------------------------
// 电机速度目标提交/撤销（装配层适配 ATOM-05；motor_id 由配置解析得到）
// ---------------------------------------------------------------------------
class IMotorCommandSink {
public:
    virtual ~IMotorCommandSink() = default;

    // 提交恒定速度目标；返回 false 表示提交失败（不得静默忽略）。
    virtual bool submitVelocityTarget(std::uint8_t motor_id, std::int32_t rpm) noexcept = 0;

    // 撤销速度目标（普通停止蓄力；不回零、不声称到位）。
    virtual bool clearVelocityTarget(std::uint8_t motor_id) noexcept = 0;
};

// ---------------------------------------------------------------------------
// 蓄力限位稳定状态（装配层适配 ATOM-06）
//
// 本模块**不读取** gpio.* 配置：采样、去抖、有效电平与限位↔电机映射均由 ATOM-06
// 负责，这里只要求"按电机 ID 给出该侧限位的有效性与稳定电平"。
// ---------------------------------------------------------------------------
struct LimitSwitchSample final {
    bool valid{false};  // 输入有效（读取成功且已稳定）
    bool low{false};    // 稳定电平为低（配置的有效电平）
};

class ILimitSwitchState {
public:
    virtual ~ILimitSwitchState() = default;

    // 返回 false 表示无法给出该侧限位状态（映射缺失或读取失败）。
    virtual bool readChargeLimit(std::uint8_t motor_id, LimitSwitchSample& out) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// 故障上报（装配层最终委托 marshland::app::IInputReporter::ReportFault）
// ---------------------------------------------------------------------------
class IFaultReporter {
public:
    virtual ~IFaultReporter() = default;

    virtual void reportChargeFault(ChargeFaultCode code,
                                   marshland::FaultSeverity severity) noexcept = 0;
};

} // namespace marshland::atom08
