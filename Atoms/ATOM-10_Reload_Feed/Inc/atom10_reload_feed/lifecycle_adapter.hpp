// ============================================================================
// lifecycle_adapter.hpp — ATOM-10 唯一的 ILifecycleModule 实现（薄壳）
//
// 无业务逻辑：initialize 只做"装载配置 -> 应用到内核 -> 建立初始输出"，
// shutdown 委托内核（幂等、noexcept、对未初始化安全）。
// 注册（RuntimeStep::InitializeServoOutputs、required 取值）由装配层/启动策略决定。
// ============================================================================
#pragma once

#include "IConfigView.hpp"
#include "ILifecycle.hpp"

#include "reload_feed_core.hpp"

namespace marshland::atom10 {

class ReloadFeedLifecycleAdapter final : public runtime::ILifecycleModule {
public:
    ReloadFeedLifecycleAdapter(ReloadFeedCore& core,
                               const config::IConfigView& config_view) noexcept;

    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom_10_reload_feed"; }

private:
    ReloadFeedCore& core_;
    const config::IConfigView& config_view_;
    bool initialized_{false};
};

} // namespace marshland::atom10
