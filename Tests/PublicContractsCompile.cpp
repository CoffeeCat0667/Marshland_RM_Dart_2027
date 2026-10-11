#include <type_traits>

#include "AppInterfaces.hpp"
#include "IMonotonicClock.hpp"

static_assert(std::is_abstract_v<marshland::app::IInputReporter>);
static_assert(std::is_abstract_v<marshland::app::IControlAuthority>);
static_assert(std::is_abstract_v<marshland::app::ISafetyStateControl>);
static_assert(std::is_abstract_v<marshland::app::IStateReader>);
static_assert(std::is_abstract_v<marshland::app::IMonotonicClock>);
static_assert(std::is_move_constructible_v<marshland::app::StateReadView>);
static_assert(!std::is_copy_constructible_v<marshland::app::StateReadView>);
