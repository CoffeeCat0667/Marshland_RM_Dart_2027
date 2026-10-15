// ============================================================================
// lifecycle_adapter.hpp — ILifecycleModule 适配（模块内部）
//
// 参考 ATOM-03 LifecycleAdapter：构造函数注入依赖，接入 RuntimeCoordinator
// 的 RuntimeStep::CreateDiagnostics 步骤。
// initialize() 不依赖配置就绪（缓冲模式），因此恒返回成功；
// 关闭时冲刷队列并停止写线程。
// ============================================================================
#pragma once

#include "ILifecycle.hpp"
#include "atom18/diagnostics_service.hpp"

namespace marshland::atom18 {

class LifecycleAdapter final : public marshland::runtime::ILifecycleModule {
public:
    explicit LifecycleAdapter(DiagnosticsService& service) noexcept
        : service_(service)
    {
    }

    marshland::runtime::ModuleInitResult initialize(
        const marshland::runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom18-diagnostics-logging"; }

private:
    DiagnosticsService& service_;
};

} // namespace marshland::atom18
