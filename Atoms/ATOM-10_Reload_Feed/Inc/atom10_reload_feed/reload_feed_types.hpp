// ============================================================================
// reload_feed_types.hpp — ATOM-10 换弹与供弹的领域类型
//
// 权威来源：
//   Design/Atomic/ATOM-10_Reload_Feed.md（2026-10-10 基线）
//   Design/Interface.md §10.6（换弹机构动作接口）
//   2026-10-15 用户裁决：供弹机构硬件变更，不再使用角度控制（位置模型）
// ============================================================================
#pragma once

#include <cstdint>

namespace marshland::atom10 {

// 换弹机构左右侧（Config.md §5.1：右 id 3 / node 0x203，左 id 4 / node 0x204）
enum class ReloadSide : std::uint8_t {
    Left,
    Right,
};

// 换弹动作
enum class ReloadAction : std::uint8_t {
    None,
    Lift,
    Lower,
};

// 换弹机构状态（Interface §10.6：至少区分抬起、放下、运行、停止与故障）
enum class ReloadState : std::uint8_t {
    Lifted,   // 抬起到位（含启动逻辑默认抬起），保持 0 rpm 速度闭环
    Lowered,  // 放下到位，保持 0 rpm 速度闭环
    Running,  // 抬起/放下动作执行中（运行计时 + 收尾提交 0 rpm）
    Stopped,  // 手动停止/急停：已取消动作与计时，两侧零力矩
    Fault,    // 故障：联动两侧零力矩，本动作失败
};

// 供弹机构状态（位置模型；机构硬件已变更，不使用角度）
enum class FeedState : std::uint8_t {
    Standby,  // 待机位（含初始状态）
    Holding,  // 已切换到换弹位，保持计时中
    Fault,    // 本次供弹失败（写入失败或超过最大动作时间）
};

// 换弹命令拒绝原因
enum class ReloadReject : std::uint8_t {
    None,
    Busy,              // 当前有换弹动作在执行
    EstopLocked,       // 软件急停锁定，本模块自身拒绝普通出力命令
    FaultLatched,      // 机构故障态，普通命令被拒绝（恢复抬起走 SAFETY_RECOVERY）
    IntentRejected,    // 控制权仲裁拒绝（accepted == false 不得当作成功）
    MotorUnavailable,  // 任一侧电机不可用（离线/反馈异常/PID 异常）
    CommandFailed,     // 速度目标提交被拒绝或失败
};

struct ReloadCommandResult {
    bool accepted{false};
    ReloadReject reject{ReloadReject::None};
};

// 供弹命令拒绝原因
enum class FeedReject : std::uint8_t {
    None,
    Busy,            // 当前有供弹动作在执行
    EstopLocked,     // 软件急停锁定
    IntentRejected,  // 控制权仲裁拒绝
    CommandFailed,   // 切换到换弹位写入失败（不得报告供弹完成）
};

struct FeedCommandResult {
    bool accepted{false};
    FeedReject reject{FeedReject::None};
};

struct ReloadStatus {
    ReloadState state{ReloadState::Lifted};
    ReloadAction active_action{ReloadAction::None};
    std::uint32_t completed_actions{0};               // 到位次数（单调递增）
    ReloadAction last_failed_action{ReloadAction::None};

    // 控制源接管边界依据：当前双电机动作组是否仍在执行
    bool action_in_progress() const noexcept { return active_action != ReloadAction::None; }
};

struct FeedStatus {
    FeedState state{FeedState::Standby};
    std::uint32_t completed_cycles{0};        // 供弹完成次数（单调递增）
    bool last_cycle_failed{false};
    std::int64_t last_trigger_micros{0};      // 最近一次供弹触发时刻
                                              // （ATOM-13/PEFExecutor 2 s 等待的计时起点依据）
};

} // namespace marshland::atom10
