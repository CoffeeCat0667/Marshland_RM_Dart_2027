// ============================================================================
// SafetyRecoveryPorts.hpp — ATOM-14 声明的窄端口（由装配层绑定实现）
//
// 用户已裁决：Public 尚未落地的机构动作/反馈/门控/事件端口，由本目录先行
// 声明窄接口，装配层后续把 ATOM-04/05/10/11 等实现适配到这些端口。
//
// 依赖方向：仅依赖 Atoms/Public 的 CanInterfaces.hpp（TorqueOutputMode）与
// 本目录类型头。禁止包含任何 ATOM 私有头、AppCoordinator 相关头或平台头。
//
// 实现方约束：全部方法 noexcept；不得在回调中重入 ATOM-14；不得阻塞调用
// 线程超过一个控制周期。
// ============================================================================
#pragma once

#include "CanInterfaces.hpp"
#include "SafetyRecoveryTypes.hpp"

namespace marshland::safety {

// ---------------------------------------------------------------------------
// ISafetyTorqueGate —— CAN 力矩输出阶段门控
//
// 对应 Atoms/Public/CanInterfaces.hpp 注释中引用但尚未落地的 ICanTorqueGate：
// ZERO_TORQUE 与 CONTROL 互斥，同一周期只发一类帧。人工确认后切到 CONTROL
// 让获准恢复目标输出；再次急停/恢复失败立即切回 ZERO_TORQUE。
// ---------------------------------------------------------------------------
class ISafetyTorqueGate {
public:
    virtual ~ISafetyTorqueGate() = default;
    virtual bool setTorqueOutputMode(can::TorqueOutputMode mode) noexcept = 0;
    virtual can::TorqueOutputMode torqueOutputMode() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// IRecoveryActions —— 恢复动作请求端口（绑定 ATOM-10/11 等机构模块）
//
// 请求语义（Guide §3.3/§4.2，顺序与判据由本模块编排，实现方只执行单次动作）：
//   requestChargeHome   蓄力机构电机 1、2 回零
//   requestRecoveryFire 发射机构一次完整单次发射（待机->发射->保持->待机）
//   requestReloadLift   双 M3508 换弹抬起（左正转、右反转，按 Config 定时运行）
//   cancelAll           撤销全部恢复出力目标（再次急停/恢复失败时调用）
// ---------------------------------------------------------------------------
class IRecoveryActions {
public:
    virtual ~IRecoveryActions() = default;
    virtual ActionResult requestChargeHome() noexcept = 0;
    virtual ActionResult requestRecoveryFire() noexcept = 0;
    virtual ActionResult requestReloadLift() noexcept = 0;
    virtual void cancelAll() noexcept = 0;

    // 动作运行状态查询。蓄力回零的完成判据以 ILimitSwitchReader 双限位均低
    // 为准，本查询的 SUCCEEDED 仅表示动作请求已被机构完成（换弹抬起：按
    // Config 定时运行后两台 0 rpm 目标均成功提交且未取消/失败）。
    virtual ActionStatus actionStatus(RecoveryAction action) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// ILimitSwitchReader —— 蓄力限位开关读取
//
// 回零完成判据：两个限位开关均稳定为低电平（Config §8.1），不允许
// 「单限位低」或「超时即完成」。
// ---------------------------------------------------------------------------
class ILimitSwitchReader {
public:
    virtual ~ILimitSwitchReader() = default;
    virtual bool chargeLimit1Low() const noexcept = 0;
    virtual bool chargeLimit2Low() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// IMotorFeedbackReader —— 电机反馈读取（反馈字段语义归 ATOM-05）
//
// 用途：换弹抬起两台 0 rpm 目标到位判定；任一换弹电机离线/异常时联动两侧
// 零力矩。motor_id 采用 Config/dart_control.json 的 motor[].id（1~6）。
// 查询失败返回 false，out_rpm 保持调用前值不变。
// ---------------------------------------------------------------------------
class IMotorFeedbackReader {
public:
    virtual ~IMotorFeedbackReader() = default;
    virtual bool motorOnline(int motor_id) const noexcept = 0;
    virtual bool motorRpm(int motor_id, double& out_rpm) const noexcept = 0;
};

// ---------------------------------------------------------------------------
// IFaultEventSink —— 故障/恢复/锁定事件发布端口
//
// 对应 Interface §18.2 的故障事件发布与 WebSocket 推送；本模块不感知网络，
// 装配层把本端口桥接到诊断/WEB 模块。
// ---------------------------------------------------------------------------
class IFaultEventSink {
public:
    virtual ~IFaultEventSink() = default;
    virtual void onFaultEvent(const FaultRecord& record) noexcept = 0;
};

} // namespace marshland::safety
