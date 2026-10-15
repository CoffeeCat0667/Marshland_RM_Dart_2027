// ============================================================================
// force_controller.hpp — ATOM-09 纯逻辑内核
//
// 性质（PLAN.md §2.1）：无线程、无 I/O、无设备，不实现 ILifecycleModule，
// 不注册任何 RuntimeStep；由装配层构造并注入，按外部周期调用 update()。
//
// 数据来源选择（Q-06）：
//   1. 查询当前控制权归属（IStateReader::ReadState().context().control_source）；
//   2. SBUS 持有  -> 读 ISbusSnapshotReader 的 scaled_channels[11]（CH12 专用 rpm）；
//   3. WEB 持有   -> 由 force_set 经 IForceController::submitTarget(MANUAL) 推入；
//   4. AUTO 持有  -> 由 ATOM-13 经 submitTarget(TIMED) 推入。
// ============================================================================
#pragma once

#include <cstdint>
#include <mutex>

#include "AppInterfaces.hpp"
#include "ForceInterfaces.hpp"
#include "IConfigView.hpp"
#include "IMonotonicClock.hpp"
#include "SbusInterfaces.hpp"
#include "force_config.hpp"
#include "force_interfaces.hpp"

namespace marshland::atom09 {

class ForceController final : public marshland::force::IForceController {
public:
    ForceController(const marshland::config::IConfigView& config_view,
                    const marshland::app::IMonotonicClock& clock,
                    const marshland::sbus::ISbusSnapshotReader& sbus,
                    marshland::app::IStateReader& state,
                    IMotorCommandSink& motor,
                    IForceFaultSink* fault_sink) noexcept;
    ~ForceController() noexcept override = default;

    ForceController(const ForceController&) = delete;
    ForceController& operator=(const ForceController&) = delete;

    marshland::force::ForceSubmitResult submitTarget(
        const marshland::force::ForceTarget& target,
        marshland::force::ForceMode mode) noexcept override;
    void update(std::int64_t now_us) noexcept override;
    marshland::force::ForceStatus status() const noexcept override;
    void stop() noexcept override;

    // 构造期配置是否可用；失败原因见 initializationError()。
    bool ready() const noexcept;
    const char* initializationError() const noexcept;

private:
    bool safetyAllows() noexcept;
    bool controlHolderIsSbus() noexcept;
    void refreshConfigIfChanged() noexcept;
    void applySbusManual() noexcept;
    void latchFault(const char* code, const char* detail) noexcept;

    const marshland::config::IConfigView& config_view_;
    const marshland::app::IMonotonicClock& clock_;
    const marshland::sbus::ISbusSnapshotReader& sbus_;
    marshland::app::IStateReader& state_;
    IMotorCommandSink& motor_;
    IForceFaultSink* fault_sink_;

    mutable std::mutex mutex_;
    ForceConfig config_{};
    bool config_ok_{false};
    const char* init_error_{"force configuration not loaded"};
    std::uint64_t config_version_{0};
    marshland::force::ForceStatus snapshot_{};
    std::int64_t timed_start_us_{0};
    bool timed_active_{false};
};

} // namespace marshland::atom09
