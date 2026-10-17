#include "StateArbitration.hpp"

#include <cstddef>
#include <mutex>

namespace marshland::state {

void StateMachine::updateControlSource() noexcept
{
    constexpr std::size_t kControlSourceChannel = 4; // CH5, not CH7.
    {
        std::lock_guard lock(sbus_data_.mutex);
        if (sbus_data_.online) {
            const auto ch5 = sbus_data_.scaled_channels[kControlSourceChannel];
            const auto source = ch5 < 0 ? ControlSource::AUTO
                              : ch5 == 0 ? ControlSource::WEB : ControlSource::SBUS;
            control_source_.store(source, std::memory_order_release);
            return;
        }
    }

    ControlSource source = ControlSource::AUTO;
    {
        std::lock_guard lock(web_data_.mutex);
        // Web has no SBUS position, including when its CH5 is positive.
        if (web_data_.online && web_data_.scaled_channels[kControlSourceChannel] >= 0) {
            source = ControlSource::WEB;
        }
    }
    control_source_.store(source, std::memory_order_release);
}

} // namespace marshland::state
