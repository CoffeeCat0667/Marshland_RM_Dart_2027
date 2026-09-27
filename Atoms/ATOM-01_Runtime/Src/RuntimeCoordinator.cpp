#include "RuntimeCoordinator.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace marshland::runtime {

namespace {

const char* unknownModuleName() noexcept
{
    return "unknown-module";
}

const char* exceptionMessage(const std::exception& exception) noexcept
{
    return exception.what() != nullptr ? exception.what() : "module initialization threw an exception";
}

} // namespace

// toString(RuntimeState) 与 toString(RuntimeStep) 已随类型上提至
// Atoms/Public/ILifecycle.hpp（inline 实现），见 ATOM01-BUG-007。

bool RuntimeStartResult::succeeded() const noexcept
{
    return state == RuntimeState::Standby;
}

RuntimeCoordinator::RuntimeCoordinator(RuntimeContext context)
    : context_(std::move(context))
{
}

RuntimeCoordinator::~RuntimeCoordinator()
{
    shutdown();
}

bool RuntimeCoordinator::addModule(ModuleRegistration registration)
{
    if (state_ != RuntimeState::Created) {
        registration_error_ = ModuleRegistrationError::IllegalState;
        return false;
    }
    if (registration.module == nullptr) {
        registration_error_ = ModuleRegistrationError::NullModule;
        return false;
    }

    // ATOM01-BUG-009：不再拒绝重复 RuntimeStep。设计文档第 5~11 条按"能力域"列举
    // 步骤，同一能力域天然可能由多个 ATOM 适配器共同承担（例如
    // InitializeSbusAndVision = ATOM-03 + ATOM-12，StartHttpAndWebSocket =
    // ATOM-16 + ATOM-17），因此采用"1 步骤 : N 模块"。
    //
    // 插入点是第一个 step 严格大于本模块的位置，所以同一步骤内保持注册先后顺序
    // （FIFO），不同步骤仍按 RuntimeStep 升序执行；关闭/回滚逆序遍历即为正确顺序。
    RegisteredModule candidate{
        registration.step,
        registration.required,
        std::move(registration.module),
        false,
    };

    const auto insertion_point = std::find_if(
        modules_.begin(),
        modules_.end(),
        [&candidate](const RegisteredModule& existing) {
            return static_cast<int>(existing.step) > static_cast<int>(candidate.step);
        });
    modules_.insert(insertion_point, std::move(candidate));

    registration_error_ = ModuleRegistrationError::None;
    return true;
}

ModuleRegistrationError RuntimeCoordinator::lastRegistrationError() const noexcept
{
    return registration_error_;
}

RuntimeStartResult RuntimeCoordinator::currentResult() const noexcept
{
    RuntimeStartResult result{};
    result.state = state_;
    try {
        result.steps = step_results_;
    } catch (...) {
        // ATOM01-BUG-004：诊断快照复制失败时退化为"只有状态"，绝不外泄异常。
        result.steps.clear();
    }
    return result;
}

RuntimeStartResult RuntimeCoordinator::start() noexcept
{
    // 区分"非 Created 状态下的既有结果快照"和"真正的启动序列"：
    // 只有后者失败时才需要回滚并释放已启动模块。
    bool sequence_started = false;

    try {
        if (state_ == RuntimeState::Stopped) {
            // ATOM01-BUG-003 修复（允许重启）：一次干净的 shutdown() 之后允许重新
            // 进入启动序列。回到 Created 并清空上一轮结果快照；各模块的 started
            // 标志已由 shutdown() 复位，此处再兜底一次。
            //
            // InitFailed 不在此列：Interface.md §7.1 没有 INIT_FAILED -> INITIALIZING
            // 这条边，因此"重启前必须先有一次显式 shutdown()"是安全前提。
            state_ = RuntimeState::Created;
            step_results_.clear();
            for (auto& registered : modules_) {
                registered.started = false;
            }
        }

        if (state_ != RuntimeState::Created) {
            return RuntimeStartResult{state_, step_results_};
        }

        sequence_started = true;
        state_ = RuntimeState::Starting;
        step_results_.clear();
        step_results_.reserve(modules_.size());

        for (auto& registered : modules_) {
            StepResult step_result{};
            step_result.step = registered.step;
            step_result.required = registered.required;
            step_result.attempted = true;

            try {
                step_result.result = registered.module->initialize(context_);
                if (step_result.result.module_name.empty()) {
                    const char* module_name = registered.module->name();
                    step_result.result.module_name = module_name != nullptr
                        ? module_name
                        : unknownModuleName();
                }
            } catch (const std::exception& exception) {
                step_result.result = exceptionResult(registered.module->name(), exceptionMessage(exception));
            } catch (...) {
                step_result.result = exceptionResult(
                    registered.module->name(),
                    "module initialization threw an unknown exception");
            }

            const bool step_succeeded = step_result.result.success;
            const bool step_required = step_result.required;
            registered.started = step_succeeded;

            // reserve() 已保证容量，且 StepResult 各成员均为 nothrow-move，
            // 因此这一步不再产生"扩容分配"或"元素拷贝"失败。
            step_results_.push_back(std::move(step_result));

            if (step_required && !step_succeeded) {
                failStart();
                return RuntimeStartResult{state_, step_results_};
            }
        }

        state_ = RuntimeState::Standby;
        return RuntimeStartResult{state_, step_results_};
    } catch (...) {
        // ATOM01-BUG-004：reserve()/push_back()/结果快照拷贝等分配点失败时，
        // 统一回滚为 InitFailed 并释放已初始化模块。本函数声明为 noexcept，
        // 异常绝不允许外泄到调用方。
        if (sequence_started) {
            failStart();
        }
        return currentResult();
    }
}

void RuntimeCoordinator::shutdown() noexcept
{
    if (state_ == RuntimeState::Stopped || state_ == RuntimeState::Created) {
        if (state_ == RuntimeState::Created) {
            state_ = RuntimeState::Stopped;
        }
        return;
    }

    state_ = RuntimeState::ShuttingDown;
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (!it->started || it->module == nullptr) {
            continue;
        }
        try {
            it->module->shutdown();
        } catch (...) {
            // shutdown() is a best-effort, noexcept lifecycle boundary. A
            // later diagnostic layer may report failures from the adapter.
        }
        it->started = false;
    }
    state_ = RuntimeState::Stopped;
}

RuntimeState RuntimeCoordinator::state() const noexcept
{
    return state_;
}

const std::vector<StepResult>& RuntimeCoordinator::stepResults() const noexcept
{
    return step_results_;
}

const RuntimeContext& RuntimeCoordinator::context() const noexcept
{
    return context_;
}

ModuleInitResult RuntimeCoordinator::exceptionResult(const char* module_name,
                                                     const char* message) noexcept
{
    ModuleInitResult result{};
    result.success = false;
    result.module_name = module_name != nullptr ? module_name : unknownModuleName();
    result.error_message = message != nullptr ? message : "module initialization failed";
    return result;
}

void RuntimeCoordinator::failStart() noexcept
{
    state_ = RuntimeState::InitFailed;
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (!it->started || it->module == nullptr) {
            continue;
        }
        try {
            it->module->shutdown();
        } catch (...) {
            // Preserve INIT_FAILED and continue releasing other modules.
        }
        it->started = false;
    }
}

} // namespace marshland::runtime


