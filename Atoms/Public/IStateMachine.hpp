#pragma once

#include "StateMachineTypes.hpp"

#include <atomic>

namespace marshland::state {

class IStateMachine {
public:
    virtual ~IStateMachine() = default;

    virtual ControlSource control_source() const noexcept = 0;
    // Called by the main thread every 20 ms; reads the injected shared inputs.
    virtual void updateControlSource() noexcept = 0;
};

namespace detail {
inline std::atomic<IStateMachine*>& getGlobalStateMachineRef() noexcept
{
    static std::atomic<IStateMachine*> instance{nullptr};
    return instance;
}
} // namespace detail

inline IStateMachine* getGlobalStateMachine() noexcept
{
    return detail::getGlobalStateMachineRef().load(std::memory_order_acquire);
}

// Register before starting readers; clear only after all readers have exited.
inline void setGlobalStateMachine(IStateMachine* instance) noexcept
{
    detail::getGlobalStateMachineRef().store(instance, std::memory_order_release);
}

} // namespace marshland::state
