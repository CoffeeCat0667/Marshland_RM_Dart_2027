#pragma once

#include "IConfigView.hpp"
#include "motor_types.hpp"

namespace marshland::atom05 {

struct MotorRuntimeConfig final {
    std::array<MotorMapping, kMotorCount> motors{};
    std::uint32_t maximum_cycle_ms{0};
    std::uint32_t motor_loop_period_ms{0};
    std::uint32_t pid_period_ms{0};
    std::uint32_t feedback_timeout_ms{0};
    PersistenceLocation persistence{};
    std::uint64_t config_version{0};
};
struct ConfigLoadResult final {
    bool success{false};
    config::ConfigStatus status{config::ConfigStatus::NOT_LOADED};
    const char* key{"configuration"};
    std::uint64_t motor_index{0};
};
bool ValidateMotorRuntimeConfig(const MotorRuntimeConfig& config) noexcept;
ConfigLoadResult LoadMotorRuntimeConfig(const config::IConfigView& view,
                                        MotorRuntimeConfig& output) noexcept;

} // namespace marshland::atom05
