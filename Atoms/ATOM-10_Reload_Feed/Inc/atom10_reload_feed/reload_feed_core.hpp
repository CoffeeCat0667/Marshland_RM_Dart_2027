// ============================================================================
// reload_feed_core.hpp — ATOM-10 纯逻辑内核
//
// 职责（Design/Atomic/ATOM-10_Reload_Feed.md、Interface §10.6）：
//   * 换弹：左右 M3508 反向速度目标 -> 两侧均提交成功后以单调时钟计时 ->
//     到期提交两侧 0 rpm -> 均成功且动作未取消/失败才判定到位，保持零转速闭环；
//   * 供弹：待机位 -> 换弹位 -> 保持 -> 待机位；写入失败不得报告供弹完成；
//   * 手动停止 / 急停 / 单侧故障：取消动作与计时，两侧零力矩，拒绝迟到完成；
//   * 启动建立逻辑默认抬起状态与两侧 0 rpm 目标，不主动抬起。
//
// 不作为：CAN/IIC 驱动、PID、编码器解析、控制权仲裁实现、发射授权、
// 供弹后 2 s 等待（归 ATOM-13/PEFExecutor 独立计时）、换弹校准/回零/限位。
//
// 线程约定：非线程安全。命令与 poll() 须由同一线程调用（装配层控制周期
// ≤ 100 ms，Config.md §4）。
// ============================================================================
#pragma once

#include <cstdint>

#include "AppInterfaces.hpp"
#include "IMonotonicClock.hpp"
#include "MarshlandTypes.hpp"
#include "StateArbitrationTypes.hpp"

#include "reload_feed_config.hpp"
#include "reload_feed_interfaces.hpp"
#include "reload_feed_types.hpp"

namespace marshland::atom10 {

class ReloadFeedCore {
public:
    // 早期可用依赖（AppCoordinator 构造之前即可绑定）。
    struct Dependencies {
        IReloadMotorPair& motors;
        IFeedActuator& feed;
        app::IMonotonicClock& clock;
    };

    ReloadFeedCore(Dependencies deps, const ReloadFeedConfig& config) noexcept;

    // —— AppCoordinator 晚绑定（方案 C，Atoms/Public/GeneralBugs/20261015-Entry.md）——
    // IControlAuthority / IInputReporter 仅由 AppCoordinator 实现，而后者晚于模块注册
    // 才能构造；故构造期不绑定，由装配层在 AppCoordinator 构造后、Start 前调用：
    void bindControlAuthority(app::IControlAuthority& authority) noexcept;
    void bindFaultReporter(app::IInputReporter* reporter) noexcept;  // 可选，允许空
    // 未绑定期间所有需仲裁的命令按 IntentRejected（NOT_READY）拒绝（fail-safe）；
    // 零力矩等安全输出不受绑定状态影响，始终允许。

    // —— 生命周期（由唯一 ILifecycleModule 适配器调用）——
    // 建立逻辑默认抬起状态并向两侧提交 0 rpm；不主动执行抬起。
    // 任一侧提交失败则尽力两侧零力矩并返回 false（适配器据此如实上报初始化失败）。
    bool initializeOutputs() noexcept;
    // 幂等、noexcept、对未初始化安全；两侧零力矩（尽力）。
    void shutdown() noexcept;
    // 热加载：configVersion() 变化后由装配层重新装载并调用本方法。
    void updateConfig(const ReloadFeedConfig& config) noexcept;

    // —— 换弹命令（ATOM-13 流程 / 手动动作 / ATOM-14 安全恢复）——
    // 普通抬起用 IntentKind::CONTROL；安全恢复抬起用 IntentKind::SAFETY_RECOVERY。
    ReloadCommandResult requestLift(ControlSource source,
                                    state_arbitration::IntentKind kind,
                                    std::uint32_t request_id) noexcept;
    ReloadCommandResult requestLower(ControlSource source,
                                     std::uint32_t request_id) noexcept;

    // —— 供弹命令 ——
    FeedCommandResult requestFeedOnce(ControlSource source,
                                      std::uint32_t request_id) noexcept;

    // —— 停止与安全（急停、手动停止、故障不等待动作组完成边界）——
    void requestManualStop(ControlSource source) noexcept;  // 取消动作与计时，两侧零力矩
    void notifySoftwareEstopLocked() noexcept;              // 急停锁定：同上，并自闭锁普通命令
    void notifySoftwareEstopReleased() noexcept;            // 恢复完成、锁存解除
    void notifySideFault(ReloadSide side) noexcept;         // 单侧故障联动两侧零力矩

    // —— 周期推进：运行计时到期收尾、供弹保持到期与最大动作时间检查 ——
    void poll() noexcept;

    ReloadStatus reloadStatus() const noexcept;
    FeedStatus feedStatus() const noexcept;

private:
    ReloadCommandResult requestReloadAction(ReloadAction action,
                                            ControlSource source,
                                            state_arbitration::IntentKind kind,
                                            std::uint32_t request_id) noexcept;
    ReloadCommandResult failActionStart(ReloadAction action) noexcept;
    state_arbitration::IntentDecision submitIntent(ControlSource source,
                                                   state_arbitration::IntentKind kind,
                                                   std::uint32_t request_id) noexcept;
    void cancelActiveAction() noexcept;  // 使计时结果失效，拒绝迟到完成
    void zeroTorqueBoth() noexcept;      // 尽力两侧零力矩（未初始化/已关停时不发命令）
    void reportFault() noexcept;         // 经可选 IInputReporter 上报严重级故障

    Dependencies deps_;
    ReloadFeedConfig config_;
    app::IControlAuthority* authority_{nullptr};  // 晚绑定，见 bindControlAuthority
    app::IInputReporter* reporter_{nullptr};      // 可选，晚绑定

    ReloadState reload_state_{ReloadState::Lifted};  // 逻辑默认抬起，未声称物理测量
    ReloadAction active_action_{ReloadAction::None};
    ReloadAction last_failed_action_{ReloadAction::None};
    std::uint32_t action_generation_{0};
    std::uint32_t completed_actions_{0};
    std::int64_t run_deadline_micros_{0};

    FeedState feed_state_{FeedState::Standby};
    std::uint32_t completed_cycles_{0};
    bool last_cycle_failed_{false};
    std::int64_t feed_trigger_micros_{0};
    std::int64_t feed_hold_deadline_micros_{0};

    bool estop_locked_{false};
    bool outputs_active_{false};
};

} // namespace marshland::atom10
