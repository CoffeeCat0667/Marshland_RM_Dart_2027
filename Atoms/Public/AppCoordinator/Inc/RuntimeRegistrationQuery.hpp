#pragma once

#include <concepts>
#include <cstddef>
#include <optional>

namespace marshland::app::detail {

// Keep the gateway buildable while ATOM-01 implements its registration query.
template <typename Runtime>
std::optional<std::size_t> RegisteredModuleCount(const Runtime& runtime) noexcept
{
    if constexpr (requires {
        { runtime.registeredModuleCount() } noexcept -> std::same_as<std::size_t>;
    }) {
        return runtime.registeredModuleCount();
    } else {
        return std::nullopt;
    }
}

} // namespace marshland::app::detail
