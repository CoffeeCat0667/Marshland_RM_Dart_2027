#pragma once

#include "ILifecycle.hpp"
#include "IConfigView.hpp"
#include "sbus_core.hpp"

namespace marshland::atom03 {

class LifecycleAdapter final : public marshland::runtime::ILifecycleModule {
public:
    LifecycleAdapter(SbusCore& core,
                     const marshland::config::IConfigView& config) noexcept
        : core_(core), config_(config) {}
    marshland::runtime::ModuleInitResult initialize(
        const marshland::runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom03-SBUS"; }
private:
    SbusCore& core_;
    const marshland::config::IConfigView& config_;
};

} // namespace marshland::atom03
