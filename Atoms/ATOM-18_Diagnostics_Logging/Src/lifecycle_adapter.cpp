#include "atom18/lifecycle_adapter.hpp"

namespace marshland::atom18 {

marshland::runtime::ModuleInitResult LifecycleAdapter::initialize(
    const marshland::runtime::RuntimeContext& context)
{
    (void)context;
    service_.OnRuntimeInitialize();
    marshland::runtime::ModuleInitResult result{};
    result.success = true;
    result.module_name = name();
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    service_.Shutdown();
}

} // namespace marshland::atom18
