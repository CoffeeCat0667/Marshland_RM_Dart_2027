// ============================================================================
// reload_feed_interfaces.hpp — ATOM-10 对外需要的窄接口（能力声明）
//
// 本文件只声明能力，不含实现；实现由装配层注入：
//   IReloadMotorPair <- 适配 ATOM-05（电机命令）
//   IFeedActuator    <- 适配供弹执行器驱动（2026-10-15 用户裁决供弹硬件变更、
//                       不再用角度控制后，由装配层决定具体实现方）
//
// 禁止包含任何其他 ATOM 的私有头或 AppCoordinator.hpp。
// ============================================================================
#pragma once

#include "reload_feed_types.hpp"

namespace marshland::atom10 {

// 换弹双电机命令窄接口。
// 约定（Rule/ATOM5ConnectProject.md §9）：
//   * 分侧提交速度目标，任一侧被拒绝或失败时由调用方撤销整对目标；
//   * 接口实现内没有换弹动作计时器，计时由 ATOM-10 负责；
//   * 正常到位必须提交 0 rpm（VELOCITY 有效闭环目标），不得以 DISABLED/零力矩替代；
//   * 两个调用不是事务，不承诺跨电机物理同步原子写入或回滚。
class IReloadMotorPair {
public:
    virtual ~IReloadMotorPair() = default;

    // 提交速度目标（rpm，含 0）。返回 false 表示被拒绝或提交失败。
    virtual bool submitVelocityRpm(ReloadSide side, int rpm) noexcept = 0;

    // 请求该侧零力矩（等价 stopMotor 通道能力）。零转速闭环 ≠ 零力矩，不得互换。
    virtual bool requestZeroTorque(ReloadSide side) noexcept = 0;

    // 侧可用性：在线、反馈有效、无离线/编码器/PID 异常。
    virtual bool isSideUsable(ReloadSide side) const noexcept = 0;
};

// 供弹执行器窄接口（位置模型，不使用角度）。
// 返回 false 表示写入失败（如 IIC 失败）；写入失败不得报告供弹完成。
class IFeedActuator {
public:
    virtual ~IFeedActuator() = default;

    // 切换到换弹位
    virtual bool moveToReloadPosition() noexcept = 0;

    // 切换回待机位
    virtual bool moveToStandbyPosition() noexcept = 0;
};

} // namespace marshland::atom10
