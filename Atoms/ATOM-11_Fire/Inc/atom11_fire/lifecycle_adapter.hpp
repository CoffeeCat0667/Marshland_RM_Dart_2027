// ============================================================================
// lifecycle_adapter.hpp — ATOM-11 唯一的 ILifecycleModule 实现
//
// 职责（Rule/AtomIntegration.md §2：内核 + 生命周期适配器两层）：
//   配置读取 → 内核就绪 → 结果翻译；
//   另负责「Public 契约查询 → 内核入参」翻译：每个调用周期经
//   IStateReader::ReadState() 读控制权与急停/故障事实，按持有控制权的来源
//   （SBUS/WEB → AuthorizeManualFire；AUTO → AuthorizeFire）查询授权结论，
//   组装 FireFacts 传入内核（用户裁决）。不含发射时机或授权的业务判断。
//
// 依赖注入（全部由装配层完成）：
//   IConfigView        配置查询；
//   IStateReader       读控制权与安全事实；
//   IControlAuthority  查询发射授权结论（本模块不提交 ControlIntent）；
//   ServoCore          ATOM-07 舵机写入（直连，用户裁决）；
//   IFireEventOutlet   完成事件与故障事实出口，装配层最终委托
//                      IInputReporter::ReportFault(...) 等。
// ============================================================================
#pragma once

#include <cstdint>

#include "AppInterfaces.hpp"
#include "IConfigView.hpp"
#include "ILifecycle.hpp"
#include "atom07_servo_driver/servo_core.hpp"
#include "atom11_fire/fire_action.hpp"

namespace marshland::atom11 {

// 完成事件与故障事实的出口；由装配层实现（不新增 Public 头，用户裁决）。
class IFireEventOutlet {
public:
    virtual ~IFireEventOutlet() = default;
    virtual void onFireCompleted(std::uint64_t now_us) noexcept = 0;
    virtual void onFireFault(const char* fault_code, std::uint64_t now_us) noexcept = 0;
};

class LifecycleAdapter final : public marshland::runtime::ILifecycleModule,
                               private IServoAngleWriter,
                               private IFireEventSink {
public:
    LifecycleAdapter(const marshland::config::IConfigView& config,
                     const marshland::app::IStateReader& state_reader,
                     marshland::app::IControlAuthority& authority,
                     marshland::atom07::ServoCore& servo,
                     IFireEventOutlet* outlet = nullptr) noexcept;

    marshland::runtime::ModuleInitResult initialize(
        const marshland::runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom11-fire"; }

    // 调用方（ATOM-13 / ATOM-14）驱动入口：拉取 FireFacts 后推进内核。
    FireStepOutcome beginOnce(std::uint64_t now_us);
    FireStepOutcome tick(std::uint64_t now_us);
    FireActionState state() const noexcept { return core_.state(); }

private:
    FireFacts readFacts();

    bool writeFireTarget(double angle_deg) noexcept override;
    void onFireCompleted(std::uint64_t now_us) noexcept override;
    void onFireFault(const char* fault_code, std::uint64_t now_us) noexcept override;

    const marshland::config::IConfigView& config_;
    const marshland::app::IStateReader& state_reader_;
    marshland::app::IControlAuthority& authority_;
    marshland::atom07::ServoCore& servo_;
    IFireEventOutlet* outlet_;

    FireActionCore core_;
    bool initialized_{false};
};

} // namespace marshland::atom11
