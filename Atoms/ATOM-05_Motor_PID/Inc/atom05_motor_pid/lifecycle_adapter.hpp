#pragma once

#include "ILifecycle.hpp"
#include "motor_core.hpp"

namespace marshland::atom05 {

enum class LifecycleStage : std::uint8_t { LOAD_PID, INITIALIZE_CHANNELS };
class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    LifecycleAdapter(MotorCore& core, const config::IConfigView& config,
                     LifecycleStage stage) noexcept
        : core_(core), config_(config), stage_(stage) {}
    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override;
private:
    MotorCore& core_;
    const config::IConfigView& config_;
    LifecycleStage stage_;
};

} // namespace marshland::atom05
