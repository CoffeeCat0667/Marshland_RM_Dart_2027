// ============================================================================
// lifecycle_adapter.cpp — ATOM-11 生命周期适配器实现
//
// 仅做「配置读取 → 内核就绪 → 结果翻译」与「Public 契约查询 → FireFacts」
// 翻译，不含发射时机或授权的业务判断；授权判定由 ATOM-02 做出。
// ============================================================================
#include "atom11_fire/lifecycle_adapter.hpp"

namespace marshland::atom11 {

LifecycleAdapter::LifecycleAdapter(
    const marshland::config::IConfigView& config,
    const marshland::app::IStateReader& state_reader,
    marshland::app::IControlAuthority& authority,
    marshland::atom07::ServoCore& servo, IFireEventOutlet* outlet) noexcept
    : config_(config)
    , state_reader_(state_reader)
    , authority_(authority)
    , servo_(servo)
    , outlet_(outlet)
    , core_(static_cast<IServoAngleWriter&>(*this), static_cast<IFireEventSink*>(this))
{
}

marshland::runtime::ModuleInitResult LifecycleAdapter::initialize(
    const marshland::runtime::RuntimeContext&)
{
    marshland::runtime::ModuleInitResult result{};
    result.module_name = name();
    FireConfig config{};
    const auto loaded = LoadFireConfig(config_, config);
    if (!loaded.success || !core_.configure(config)) {
        result.error_message = loaded.success ? "fire config rejected by core"
                                              : loaded.error_message;
        return result;
    }
    initialized_ = true;
    result.success = true;
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    initialized_ = false;
}

FireFacts LifecycleAdapter::readFacts()
{
    FireFacts facts{};
    marshland::ControlSource holder = marshland::ControlSource::NONE;
    {
        // StateReadView 约定：持租约期间不做端口调用；拷贝所需字段后先释放。
        auto view = state_reader_.ReadState();
        facts.software_estop = view.estop_latched();
        facts.fault_active = view.inputs().fault_active;
        holder = view.context().control_source;
    }
    switch (holder) {
    case marshland::ControlSource::SBUS:
    case marshland::ControlSource::WEB:
        facts.authorization = authority_.AuthorizeManualFire(holder);
        break;
    case marshland::ControlSource::AUTO:
        facts.authorization = authority_.AuthorizeFire(holder);
        break;
    case marshland::ControlSource::NONE:
        facts.authorization.allowed = false;
        facts.authorization.reason =
            marshland::state_arbitration::RejectReason::NO_CONTROL_AUTHORITY;
        break;
    }
    return facts;
}

FireStepOutcome LifecycleAdapter::beginOnce(std::uint64_t now_us)
{
    if (!initialized_) {
        return FireStepOutcome::REJECTED_NOT_READY;
    }
    return core_.begin(readFacts(), now_us);
}

FireStepOutcome LifecycleAdapter::tick(std::uint64_t now_us)
{
    if (!initialized_) {
        return FireStepOutcome::REJECTED_NOT_READY;
    }
    return core_.tick(readFacts(), now_us);
}

bool LifecycleAdapter::writeFireTarget(double angle_deg) noexcept
{
    const auto result =
        servo_.writeTarget(marshland::atom07::ServoMechanism::Fire, angle_deg);
    return result.ok();
}

void LifecycleAdapter::onFireCompleted(std::uint64_t now_us) noexcept
{
    if (outlet_ != nullptr) {
        outlet_->onFireCompleted(now_us);
    }
}

void LifecycleAdapter::onFireFault(const char* fault_code,
                                   std::uint64_t now_us) noexcept
{
    if (outlet_ != nullptr) {
        outlet_->onFireFault(fault_code, now_us);
    }
}

} // namespace marshland::atom11
