// ============================================================================
// charge_controller.hpp — ATOM-08 纯逻辑内核
//
// 只依赖 Atoms/Public 契约（IConfigView / IMonotonicClock / AppInterfaces /
// SbusInterfaces）与本模块私有头；不包含任何 ATOM-NN 私有头、AppCoordinator.hpp。
// 不创建线程：由上层按控制周期调用 tick(now_us)。
// ============================================================================
#pragma once

#include <cstdint>
#include <mutex>

#include "AppInterfaces.hpp"
#include "IConfigView.hpp"
#include "IMonotonicClock.hpp"
#include "SbusInterfaces.hpp"
#include "charge_config.hpp"
#include "charge_interfaces.hpp"
#include "charge_types.hpp"

namespace marshland::atom08 {

class ChargeController final {
public:
    ChargeController(const marshland::config::IConfigView& config,
                     const marshland::app::IMonotonicClock& clock,
                     IMotorCommandSink& motors,
                     const ILimitSwitchState& limits,
                     marshland::app::IControlAuthority& authority,
                     marshland::app::IStateReader& state_reader,
                     marshland::sbus::ISbusSnapshotReader& sbus,
                     IFaultReporter& faults) noexcept;

    ChargeController(const ChargeController&) = delete;
    ChargeController& operator=(const ChargeController&) = delete;

    // ---- 生命周期适配器接口（ATOM 内注册接口的实现细节） ----
    bool configure(const ChargeConfig& config) noexcept;
    void reset() noexcept;

    // ---- 命令 ----
    ChargeRequestResult requestCharge(const ChargeStartRequest& request) noexcept;
    ChargeRequestResult requestStopCharge() noexcept;
    ChargeRequestResult requestHome(const ChargeHomeRequest& request) noexcept;

    // ---- 周期推进（计时、限位判定、超时与急停/故障撤销） ----
    // 期望由装配层按 20 ms 控制周期调用（Design/Config.md §4/§15）；本模块不创建线程。
    void tick(std::int64_t now_us) noexcept;

    // ---- 查询 ----
    ChargeStatusSnapshot status() const noexcept;
    bool initialized() const noexcept;

private:
    struct ArbitrationSnapshot final {
        marshland::SystemMode mode{marshland::SystemMode::UNINITIALIZED};
        marshland::ControlSource control_source{marshland::ControlSource::NONE};
        bool fault_active{false};
        bool hardware_ready{false};
    };

    // 先取快照再立即释放 StateReadView，避免持锁期间调用其他端口（同线程自锁）
    ArbitrationSnapshot readArbitration() noexcept;
    bool reloadConfigLocked() noexcept;
    bool resolveChargeTargetsLocked(const ChargeStartRequest& request,
                                    marshland::ControlSource effective_source,
                                    std::int32_t& left_rpm, std::int32_t& right_rpm) const noexcept;
    bool targetsWithinRange(std::int32_t left_rpm, std::int32_t right_rpm) const noexcept;
    static bool isModeAllowed(marshland::SystemMode mode,
                              marshland::state_arbitration::IntentKind kind) noexcept;
    static ChargeRequestResult mapRejectReason(
        marshland::state_arbitration::RejectReason reason) noexcept;
    bool submitIntentLocked(marshland::state_arbitration::IntentKind kind,
                            marshland::ControlSource source,
                            std::uint32_t request_id,
                            std::uint64_t timestamp_us) noexcept;
    bool submitBothMotorsLocked(std::int32_t left_rpm, std::int32_t right_rpm) noexcept;
    void clearBothMotorsLocked() noexcept;
    void enterFaultLocked(ChargeFaultCode code, std::int64_t now_us,
                          ChargeFaultCode& report) noexcept;
    ChargeFaultCode doTickLocked(std::int64_t now_us) noexcept;

    const marshland::config::IConfigView& config_view_;
    const marshland::app::IMonotonicClock& clock_;
    IMotorCommandSink& motors_;
    const ILimitSwitchState& limits_;
    marshland::app::IControlAuthority& authority_;
    marshland::app::IStateReader& state_reader_;
    marshland::sbus::ISbusSnapshotReader& sbus_;
    IFaultReporter& faults_;

    mutable std::mutex mutex_;
    ChargeConfig config_{};
    std::uint64_t config_version_{0};
    bool config_valid_{false};
    ChargeState state_{ChargeState::Uninitialized};
    ChargeAction action_{ChargeAction::None};
    bool charge_completed_{false};
    bool home_completed_{false};
    bool limit_invalid_reported_{false};
    std::int64_t action_start_us_{0};
    std::int32_t left_target_rpm_{0};
    std::int32_t right_target_rpm_{0};
    ChargeFaultCode last_fault_{ChargeFaultCode::None};
    ChargeRequestResult last_result_{ChargeRequestResult::RejectedNotInitialized};
    std::uint64_t last_fault_time_us_{0};
};

} // namespace marshland::atom08