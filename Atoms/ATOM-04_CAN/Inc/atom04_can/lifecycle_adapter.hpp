// ============================================================================
// lifecycle_adapter.hpp — ATOM-04 生命周期适配器（薄壳）
//
// 依据 Rule/AtomIntegration.md §2.2：唯一实现 ILifecycleModule 的类，
// 独立翻译单元，只做"初始化编排与结果翻译"，不写业务逻辑。
//
// 依赖方向：Atoms/Public/ILifecycle.hpp、IConfigView.hpp + 本模块内核。
// 不注册自己、不判断控制权、不做安全编排。
// ============================================================================
#pragma once

#include "ILifecycle.hpp"
#include "IConfigView.hpp"
#include "atom04_can/can_core.hpp"

namespace marshland::atom04 {

class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    LifecycleAdapter(CanCore& core, const config::IConfigView& config) noexcept
        : core_(core), config_(config)
    {
    }

    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom-04-can"; }

private:
    CanCore& core_;
    const config::IConfigView& config_;
};

} // namespace marshland::atom04
