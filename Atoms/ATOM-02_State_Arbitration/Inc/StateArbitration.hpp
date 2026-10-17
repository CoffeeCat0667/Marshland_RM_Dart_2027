#pragma once

#include "../../Public/IStateMachine.hpp"
#include "../../Public/Sbus.hpp"
#include "../../Public/Web.hpp"

#include <atomic>

namespace marshland::state {

class StateMachine final : public IStateMachine {
public:
    StateMachine(const sbus::SbusData& sbus_data, const web::WebData& web_data) noexcept
        : sbus_data_(sbus_data), web_data_(web_data)
    {
    }

    ControlSource control_source() const noexcept override
    {
        return control_source_.load(std::memory_order_acquire);
    }

    void updateControlSource() noexcept override;

private:
    const sbus::SbusData& sbus_data_;
    const web::WebData& web_data_;
    std::atomic<ControlSource> control_source_{ControlSource::AUTO};
};

} // namespace marshland::state
