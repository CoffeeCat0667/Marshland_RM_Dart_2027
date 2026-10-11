#pragma once

#include <cstdint>

namespace marshland::app {

class IMonotonicClock {
public:
    virtual ~IMonotonicClock() = default;
    virtual std::int64_t nowMicros() const noexcept = 0;
};

} // namespace marshland::app
