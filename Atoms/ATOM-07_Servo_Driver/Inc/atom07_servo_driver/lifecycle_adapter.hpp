// ============================================================================
// lifecycle_adapter.hpp — 唯一的 runtime::ILifecycleModule 实现（薄壳）
//
// 只做"配置读取 -> 内核启动/停止 + 结果翻译"，不含业务逻辑（≤ 50 行实现）。
// 注册步骤由装配层决定：RuntimeStep::InitializeIicPca9685（ATOM-07）。
// ============================================================================
#pragma once

#include "IConfigView.hpp"
#include "ILifecycle.hpp"
#include "atom07_servo_driver/servo_core.hpp"

namespace marshland::atom07 {

class LifecycleAdapter final : public marshland::runtime::ILifecycleModule {
public:
    LifecycleAdapter(ServoCore& core,
                     const marshland::config::IConfigView& config) noexcept
        : core_(core), config_(config)
    {
    }

    marshland::runtime::ModuleInitResult initialize(
        const marshland::runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom07-servo-driver"; }

private:
    ServoCore& core_;
    const marshland::config::IConfigView& config_;
};

} // namespace marshland::atom07
