// ============================================================================
// lifecycle_adapter.hpp — ATOM 内注册接口（唯一实现 ILifecycleModule 的类）
//
// 只做"配置读取 → 内核配置 → 结果翻译"；不写业务逻辑，不注册自己
// （addModule() 由顶层装配层执行，见 Rule/AtomIntegration.md §3.1(c)、§7）。
// ============================================================================
#pragma once

#include "IConfigView.hpp"
#include "ILifecycle.hpp"
#include "charge_controller.hpp"

namespace marshland::atom08 {

class LifecycleAdapter final : public marshland::runtime::ILifecycleModule {
public:
    LifecycleAdapter(ChargeController& core,
                     const marshland::config::IConfigView& config) noexcept
        : core_(core), config_(config) {}

    marshland::runtime::ModuleInitResult initialize(
        const marshland::runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom08-charge"; }

private:
    ChargeController& core_;
    const marshland::config::IConfigView& config_;
};

} // namespace marshland::atom08
