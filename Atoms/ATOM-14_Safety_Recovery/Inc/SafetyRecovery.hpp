// ============================================================================
// SafetyRecovery.hpp — ATOM-14 安全、故障与恢复管理
//
// 职责（Design/Atomic/ATOM-14_Safety_Recovery.md）：
//   故障登记/查询/更新/清除与事件发布、普通命令安全闸门、一般故障自动恢复
//   编排、软件急停锁定与人工解除编排、零力矩安全帧的持续发送调度。
//
// 依赖方向（不可协商）：
//   仅依赖 Atoms/Public 契约与本目录声明的窄端口。禁止包含
//   AppCoordinator / StateArbitrator / RuntimeCoordinator 具体头，禁止持有
//   其具体引用，禁止直接访问 SocketCAN / I2C / 电机寄存器 / 舵机通道。
//
// 零力矩帧的发送调度：ATOM-04 发送线程在 ZERO_TORQUE 门控下按 Config 周期
// 持续发两组全零帧；本模块只负责按安全阶段切换 ISafetyTorqueGate，不构造
// 帧、不直接发帧。确认后的恢复阶段切 CONTROL 让获准目标输出，门控互斥保证
// 不用全零帧覆盖恢复出力。
//
// 并发：update() 与公开 API 由装配层统一同步域串行调用；CAN 回调
// （ICanSafetyNotifier / ICanEventSink）来自 CAN 线程，只置原子标志，由
// update() 消费。故障存储由内部互斥锁保护。
// ============================================================================
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "AppInterfaces.hpp"
#include "CanInterfaces.hpp"
#include "IConfigView.hpp"
#include "IMonotonicClock.hpp"
#include "SafetyRecoveryPorts.hpp"
#include "SafetyRecoveryTypes.hpp"

namespace marshland::safety {

class SafetyRecovery final : public can::ICanSafetyNotifier,
                             public can::ICanEventSink {
public:
    // 注入端口集合。除标注「可空」外均为必需；构造不执行任何硬件交互。
    struct Ports final {
        app::ISafetyStateControl* safety_state{nullptr};     // 急停确认/解除（仅本模块持有）
        app::IControlAuthority* control_authority{nullptr};  // SAFETY / SAFETY_RECOVERY 意图提交
        app::IInputReporter* input_reporter{nullptr};        // 故障归约上报（ReportFault）
        const app::IStateReader* state_reader{nullptr};      // 权威状态读取（借读，不保存内部引用）
        const app::IMonotonicClock* clock{nullptr};          // 单调时钟（等待/超时/周期）
        const config::IConfigView* config{nullptr};          // 配置查询（恢复参数、电机映射）
        ISafetyTorqueGate* torque_gate{nullptr};             // 可空：未绑定时无法切换出力阶段
        IRecoveryActions* recovery_actions{nullptr};         // 可空：未绑定时恢复请求直接失败
        const ILimitSwitchReader* limit_switches{nullptr};   // 可空：未绑定时回零判据不成立
        const IMotorFeedbackReader* motor_feedback{nullptr}; // 可空：未绑定时按离线处理
        IFaultEventSink* fault_events{nullptr};              // 可空：未绑定时仅登记不推送
    };

    explicit SafetyRecovery(Ports ports) noexcept;

    SafetyRecovery(const SafetyRecovery&) = delete;
    SafetyRecovery& operator=(const SafetyRecovery&) = delete;

    // ---- 周期推进（装配层按控制周期调用） -----------------------------------
    void update() noexcept;

    // ---- 急停入口（装配层转发 WEB/SBUS 人工确认请求） ------------------------
    // 只在 ESTOP_LOCKED 阶段有效；确认成功后进入 ESTOP_RECOVERY 并启动解除
    // 动作。确认不等于解除：解除以 ReleaseSoftwareEstopLatch() 成功为准。
    void onEstopConfirmRequest() noexcept;

    // ---- 一般故障恢复触发 -----------------------------------------------------
    // 登记了可自动恢复的故障即挂起恢复意图；update() 在根因清除（无严重/急停
    // 级活动故障）、接口有效且非急停锁定后才真正启动，急停锁定期间不推进。
    void requestGeneralRecovery() noexcept;

    // ---- 安全闸门（装配层/执行器在普通命令前查询） ---------------------------
    // 锁定期内由本模块拒绝普通命令；AppCoordinator::RunGate 只是第二道防线。
    bool isOrdinaryCommandAllowed() const noexcept;

    // ---- 故障管理（Interface §18.2） -----------------------------------------
    // code 必须命中 Config §14.2 目录，否则拒绝登记（不自造故障码）。
    bool registerFault(const char* code, const char* source) noexcept;
    bool clearFault(const char* code) noexcept;
    bool acknowledgeFault(const char* code) noexcept;
    std::size_t copyActiveFaults(FaultRecord* output, std::size_t capacity) const noexcept;
    // 历史故障：环形保留 1000 条，最新优先（output[0] 为最新）。
    std::size_t copyFaultHistory(FaultRecord* output, std::size_t capacity) const noexcept;
    std::size_t activeFaultCount() const noexcept;

    // ---- CAN 事件入口（can::ICanSafetyNotifier / ICanEventSink，CAN 线程） ---
    void onZeroTorqueSendFailed(const can::CanIoResult& result) noexcept override;
    void onCanEvent(const can::CanEvent& event) noexcept override;

    // ---- 状态查询 -------------------------------------------------------------
    SafetyPhase phase() const noexcept { return phase_; }
    RecoveryStage recoveryStage() const noexcept { return recovery_stage_; }
    bool portsReady() const noexcept;

private:
    void tryLoadConfig() noexcept;
    void drainCanEvents() noexcept;

    void enterLockdown() noexcept;
    void enterFaultLock() noexcept;
    void advanceEstopRecovery() noexcept;
    void advanceGeneralRecovery() noexcept;
    void abortGeneralRecovery() noexcept;
    void maybeStartGeneralRecovery() noexcept;

    bool submitSafetyRecoveryIntent() noexcept;
    bool reloadMotorsOnline() const noexcept;
    bool chargeHomeLimitsLow() const noexcept;
    bool hasBlockingActiveFault() const noexcept;
    void reportFaultRollup() noexcept;

    Ports ports_;

    SafetyPhase phase_{SafetyPhase::NORMAL};
    RecoveryStage recovery_stage_{RecoveryStage::NONE};
    bool estop_latched_{false};          // update() 内缓存的权威锁存状态
    bool recovery_pending_{false};       // 存在待执行的一般故障恢复意图

    // 配置快照（tryLoadConfig 惰性加载；默认值与 dart_control.json 一致）
    bool config_loaded_{false};
    std::uint32_t recovery_fire_total_{3};
    int motor_charge_right_{1};
    int motor_charge_left_{2};
    int motor_reload_right_{3};
    int motor_reload_left_{4};

    std::uint32_t recovery_fires_remaining_{0};
    std::uint32_t request_id_{0};

    // CAN 线程 -> update() 的事件标志（只置位，故障登记在 update 内完成）
    std::atomic<bool> pending_zero_torque_failed_{false};
    std::atomic<bool> pending_can_bus_error_{false};
    std::atomic<std::uint32_t> pending_motor_offline_mask_{0};

    // 故障存储：活动表 + 环形历史（1000 条，最新优先读出）
    static constexpr std::size_t kMaxActiveFaults = 64;
    mutable std::mutex fault_mutex_;
    std::array<FaultRecord, kMaxActiveFaults> active_faults_{};
    std::size_t active_fault_count_{0};
    std::array<FaultRecord, kFaultHistoryCapacity> history_{};
    std::size_t history_head_{0};    // 下一次写入位置
    std::size_t history_count_{0};
};

} // namespace marshland::safety
