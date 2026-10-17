#pragma once

namespace marshland::state {

enum class ControlSource {
    AUTO = 0,
    WEB = 1,
    SBUS = 2
};

inline const char* toString(ControlSource source) noexcept
{
    switch (source) {
    case ControlSource::AUTO: return "AUTO";
    case ControlSource::WEB: return "WEB";
    case ControlSource::SBUS: return "SBUS";
    }
    return "UNKNOWN";
}

} // namespace marshland::state
