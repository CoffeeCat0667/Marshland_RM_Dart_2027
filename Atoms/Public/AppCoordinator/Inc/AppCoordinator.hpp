// ============================================================================
// AppCoordinator.hpp — 顶层装配与桥接层（唯一的一处）
//
// 分层职责（不得互相侵入）：
//
//   [底层] marshland::runtime::RuntimeCoordinator   —— 硬件与服务拉起
//          * 按 RuntimeStep 顺序初始化 SocketCAN / 电机 / 编码器 / GPIO /
//            IIC / PCA9685A / 舵机 / SBUS / 视觉 / HTTP / WebSocket
//          * 只暴露硬件生命周期事实（state() / stepResults()）
//          * **不**承载业务待机语义：RuntimeState::Standby 只是启动流程终点
//
//   [上层] marshland::state_arbitration::StateArbitrator —— 业务权威
//          * 系统模式、控制源选择、自动流程状态、急停锁存、故障输入、发射授权
//          * 本层不感知硬件是否已拉起
//
//   [本层] AppCoordinator —— 二者之间唯一的桥
//          * 启动结果 -> SystemMode 的**唯一**映射点（applyStartupOutcome）
//          * 外部输入的**唯一**写入点（委托 StateArbitrator 的字段级 setter）
//          * 运行闸门 = 硬件就绪 AND 未急停锁存 AND 无阻断故障 AND 模式可操作
//
// 本层保持"薄"。以下事项**不属于**本层，禁止在此实现：
//   1. 模块注册与排序（由上层对 RuntimeCoordinator 完成）
//   2. 控制权仲裁与发射授权判定（由 StateArbitrator 承担）
//   3. 软件急停的动作编排：人工确认（ConfirmSoftwareEstop）与解除生效
//      （ReleaseSoftwareEstopLatch）的调用时机由 ATOM-14 SafetyManager 掌握。
//      本层**只读** is_estop_latched() 用于门控，不代为确认或解除；
//      本层确实没有提供确认/解除入口，这是刻意的边界而非遗漏。
//   4. 一般故障自动恢复编排（属 ATOM-14）
//   5. 四发自动流程推进（属 ATOM-13 PEFExecutor）
//   6. 硬件驱动、CAN/IIC 报文构造
//
// 依赖方向：
//   AppCoordinator -> RuntimeCoordinator、StateArbitrator、Public 基础类型
//   ATOM-01 ~ ATOM-19 --X--> AppCoordinator   （禁止反向包含；本文件不是共享契约）
//
// 生命周期前置条件：
//   必须**先**完成模块注册，再调用 Start()。Start() 只负责"拉起 + 映射"。
//   **不支持运行中重启**（产品裁决 2026-09-27）：Start() 只允许成功调用一次，
//   之后必须重启进程。详见 kRestartNotSupported。
//
// 缺陷状态（与 Atoms/ATOM-02_State_Arbitration/BugLists.md 同步）：
//   ATOM02-BUG-001 自动流程 FORCE -> SHOT_N_YAW 缺边
//       -> 已在 ATOM-02 源头修复（ShotStage 消歧 + current_shot），本层不补偿
//   ATOM02-BUG-002 发射授权以控制源 rank 比较代替授权优先级
//       -> 已按方案 B 修复：授权函数只做前置条件复核，
//          优先级由 StateArbitrator::SubmitIntent 的 FIRE 分支仲裁
//   ATOM02-BUG-003 手动/调试模式的 fire_once 无合法提交路径
//       -> 已修复：StateArbitrator::AuthorizeManualFire()
//   ATOM02-BUG-004 ConfirmSoftwareEstop() 立即清零急停变量
//       -> 已修复：确认与解除分离；ATOM-14 完成解除动作后调用
//          ReleaseSoftwareEstopLatch()，本层不得据此提前恢复执行器
//          （本层的对应措施见 RunGateReason::ESTOP_LATCHED）
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "MarshlandTypes.hpp"
#include "RuntimeCoordinator.hpp"
#include "StateArbitration.hpp"

namespace marshland::app {

// ---------------------------------------------------------------------------
// 单调时钟：Interface.md §3.3 要求所有等待、超时与控制周期使用单调时钟，
// 不得使用可能被系统校时影响的墙上时钟。工程此前没有该抽象，故在本层引入。
// ---------------------------------------------------------------------------
class IMonotonicClock {
public:
    virtual ~IMonotonicClock() = default;
    virtual std::int64_t nowMicros() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// 运行闸门结果
//
// 必须能区分拒绝来自哪一层，否则排查时无法定位。
// 判据顺序即安全优先级：急停锁存 > 阻断故障 > 初始化/模式状态。
// ---------------------------------------------------------------------------
enum class RunGateReason : std::uint8_t {
    ALLOWED,
    HARDWARE_NOT_READY,    // 底层启动未成功，或已 Shutdown
    ESTOP_LATCHED,         // 急停锁存未解除（含人工确认后、解除动作完成前）
    FAULT_ACTIVE,          // 存在阻断级故障（SEVERE / ESTOP，见 ATOM-02 IsFaultBlocking）
    MODE_NOT_OPERATIONAL,  // 业务模式不在可操作集合内
    NOT_INITIALIZED        // 从未成功 Start（SystemMode 仍为 UNINITIALIZED）
};

inline const char* ToString(RunGateReason reason) noexcept
{
    switch (reason) {
    case RunGateReason::ALLOWED:              return "ALLOWED";
    case RunGateReason::HARDWARE_NOT_READY:   return "HARDWARE_NOT_READY";
    case RunGateReason::ESTOP_LATCHED:        return "ESTOP_LATCHED";
    case RunGateReason::FAULT_ACTIVE:         return "FAULT_ACTIVE";
    case RunGateReason::MODE_NOT_OPERATIONAL: return "MODE_NOT_OPERATIONAL";
    case RunGateReason::NOT_INITIALIZED:      return "NOT_INITIALIZED";
    }
    return "UNKNOWN";
}

class AppCoordinator final {
public:
    // 错误码（与既有风格的短常量保持一致；后续可迁移至统一 Result 类型）
    static constexpr int kOk = 0;
    static constexpr int kInvalidArgument = -1;      // clock == nullptr
    static constexpr int kRuntimeStartFailed = -2;   // 硬件/服务启动失败
    static constexpr int kAlreadyStarted = -3;       // 重复调用 Start()（尚未 Shutdown）
    static constexpr int kPreconditionFailed = -4;   // 状态机拒绝了 INITIALIZING 迁移
    // 运行中重启不支持（产品裁决 2026-09-27，见 Atoms/ATOM-02_State_Arbitration/
    // BugLists.md ATOM02-BUG-015）：Shutdown() 之后再次 Start() 一律拒绝，
    // 必须重启进程。这样即使底层 RuntimeCoordinator 仍支持 Stopped -> Created，
    // 本层也不会把它变成一个业务上不可用的"半初始化"状态。
    static constexpr int kRestartNotSupported = -5;

    // detail 为可选输出；调用方传 nullptr 表示不关心细节。
    AppCoordinator(runtime::RuntimeCoordinator& runtime,
                   state_arbitration::StateArbitrator& authority,
                   IMonotonicClock& clock) noexcept;

    AppCoordinator(const AppCoordinator&) = delete;
    AppCoordinator& operator=(const AppCoordinator&) = delete;
    AppCoordinator(AppCoordinator&&) = delete;
    AppCoordinator& operator=(AppCoordinator&&) = delete;

    // 析构即释放：调用 Shutdown()，避免出现"硬件已关、本层仍声称已启动"。
    // 本层不拥有 StateArbitrator，业务模式的复位由持有者决定（见 Shutdown）。
    ~AppCoordinator() noexcept;

    // 拉起硬件与服务，并把启动结果映射为系统模式。
    // 成功：SystemMode -> STANDBY，hardware_ready() == true
    // 失败：SystemMode -> INIT_FAILED，hardware_ready() == false
    int Start(runtime::ModuleInitResult* detail = nullptr) noexcept;

    // 停止硬件与服务。不改动业务模式（业务模式清理由上层决定）。
    //
    // 产品裁决（2026-09-27）：**不支持运行中重启**。一次 Shutdown() 之后再次
    // Start() 会以 kRestartNotSupported 被拒绝，必须重启进程。
    // 注意底层 RuntimeCoordinator 自身仍支持 Stopped -> Created 重启，本层
    // 刻意不暴露该能力，以免产生"硬件起来了、业务侧没重新初始化"的半初始化状态。
    void Shutdown() noexcept;

    // ---- 运行闸门（本层唯一的目的：让上层有一个统一的判据） --------------
    // 注意：本闸门是**普通执行器动作**的闸门。RECOVERY 与 SOFTWARE_ESTOP 下由
    // ATOM-14 发起的"安全动作"（零力矩、换弹抬起、蓄力回零）走
    // IntentKind::SAFETY 豁免路径，不经过本闸门。
    RunGateReason RunGate() const noexcept;
    bool CanRunActuators() const noexcept;

    // 硬件是否已就绪。这是 RuntimeState::Standby 的**正确**用途：
    // 表示"底层已拉起"，绝不表示业务待机。
    bool hardware_ready() const noexcept { return hardware_ready_; }

    // 启动后只读快照，供诊断接口使用
    const runtime::RuntimeStartResult& startup() const noexcept { return startup_; }
    const std::vector<runtime::StepResult>& stepResults() const noexcept
    {
        return runtime_.stepResults();
    }

    // ---- 外部输入上报：所有输入的**唯一**写入点 ---------------------------
    //
    // 直接委托 StateArbitrator 的字段级 setter。本层**不持有**输入副本：
    //   1. 副本会漂移——典型场景是本层副本 software_estop=false 与仲裁器锁存后
    //      的 true 不一致，诊断据此会误判"急停已解除"；
    //   2. 整体替换式提交会把其他来源写入的字段清零（ATOM02-BUG-005）。
    // 各来源一律经由此处上报，不得直接调用 authority_.SetXxx()，
    // 否则两条写入路径会互相覆盖。
    //
    // 本层不解释这些字段，只做搬运；判定权始终在 StateArbitrator。
    void ReportSbus(bool online, bool auto_mode) noexcept;
    void ReportWebOnline(bool online) noexcept;
    void ReportYawArrived(bool arrived) noexcept;
    void ReportStopFire(bool set) noexcept;
    void ReportFault(bool active, FaultSeverity severity) noexcept;
    void ReportSoftwareEstop(bool active) noexcept;

    // 单调时钟。状态机生成事件时使用输入快照的时间戳（事件时间为 0 会退化为
    // 沿用上一次输入的时间），因此各来源在完成一轮上报后调用一次即可。
    void ReportClock() noexcept;

    // 最近一次已提交给状态机的输入快照（只读，供诊断与自检）。
    // 取自仲裁器本身，因此与判定依据始终一致。
    const state_arbitration::ExternalInputs& lastInputs() const noexcept
    {
        return authority_.inputs();
    }

private:
    // 启动结果 -> 业务系统模式的唯一映射点
    void applyStartupOutcome(bool hardware_start_ok,
                             runtime::ModuleInitResult* detail) noexcept;

    runtime::RuntimeCoordinator& runtime_;
    state_arbitration::StateArbitrator& authority_;
    IMonotonicClock& clock_;

    runtime::RuntimeStartResult startup_{};

    // ReportClock() 使用的单调递增序号（每实例独立，不共享全局状态）
    std::uint64_t report_sequence_{0};

    bool hardware_ready_{false};
    bool started_{false};
};

} // namespace marshland::app
