#pragma once

// 上提说明（见 Atoms/ATOM-01_Runtime/BugLists.md ATOM01-BUG-007）：
//   RuntimeState / RuntimeStep / RuntimeContext / ModuleInitResult / StepResult /
//   ILifecycleModule / toString() 已上提至 Atoms/Public/ILifecycle.hpp。
//   原因是它们是其余 18 个 ATOM 接入启动流程的唯一公共契约，留在本模块私有头
//   会形成 "Atom -> Atom" 横向耦合。本文件改为包含该公共头，依赖方向统一为
//   "Atom -> Public"。
//
//   RuntimeCoordinator 自身（模块注册、就绪汇总、失败回滚、关闭释放）仍属
//   ATOM-01 私有，不上提。
#include <cstdint>

#include "ILifecycle.hpp"

namespace marshland::runtime {

struct RuntimeStartResult {
    RuntimeState state{RuntimeState::Created};
    std::vector<StepResult> steps;

    bool succeeded() const noexcept;
};

// ---------------------------------------------------------------------------
// 模块注册结果（ATOM01-BUG-009）
//
// addModule() 原先只用 bool 表示失败，调用点无法知道失败原因，可能造成模块
// 静默未注册。本枚举提供可查询原因。不属于跨 ATOM 契约，仅 ATOM-01 私有 API。
// ---------------------------------------------------------------------------
enum class ModuleRegistrationError : std::uint8_t {
    None,
    IllegalState,   // 启动序列已开始或已结束：只能在 Created 阶段注册
    NullModule,     // module == nullptr
};

inline const char* toString(ModuleRegistrationError error) noexcept
{
    switch (error) {
    case ModuleRegistrationError::None:         return "none";
    case ModuleRegistrationError::IllegalState: return "illegal-state";
    case ModuleRegistrationError::NullModule:   return "null-module";
    }
    return "unknown";
}

struct ModuleRegistration {
    // ATOM01-BUG-005：step / required 原先没有默认值，调用方聚合初始化时漏填
    // 会读到不确定值。required 取更安全的默认值 true（必需模块失败必须致命），
    // step 取首步骤以避免未初始化比较（漏填会与既有步骤冲突而被 addModule 拒绝，
    // 而不是产生未定义行为）。
    RuntimeStep step{RuntimeStep::CreateDiagnostics};
    bool required{true};
    std::unique_ptr<ILifecycleModule> module;
};

class RuntimeCoordinator final {
public:
    explicit RuntimeCoordinator(RuntimeContext context = {});
    ~RuntimeCoordinator();

    RuntimeCoordinator(const RuntimeCoordinator&) = delete;
    RuntimeCoordinator& operator=(const RuntimeCoordinator&) = delete;
    RuntimeCoordinator(RuntimeCoordinator&&) = delete;
    RuntimeCoordinator& operator=(RuntimeCoordinator&&) = delete;

    // ATOM01-BUG-009：同一步骤允许注册多个模块（1 步骤 : N 模块），同一步骤内按
    // 注册先后顺序执行；失败原因通过 lastRegistrationError() 查询。
    bool addModule(ModuleRegistration registration);
    ModuleRegistrationError lastRegistrationError() const noexcept;

    // ATOM01-BUG-004：内部已捕获全部异常（含 reserve/push_back/结果快照拷贝的
    // 分配失败）并统一回滚为 InitFailed，因此声明为 noexcept；调用方（例如
    // AppCoordinator::Start 本身是 noexcept）不会因异常外泄而 std::terminate。
    //
    // ATOM01-BUG-003：完成一次 shutdown() 之后（Stopped）允许重启，回到
    // Created 并清空上一轮结果快照；InitFailed 不能直接重启，必须先 shutdown()。
    RuntimeStartResult start() noexcept;

    void shutdown() noexcept;

    RuntimeState state() const noexcept;

    // ATOM01-BUG-002 修复：原 canExecuteActuatorCommands() 以本地 RuntimeState::Standby
    // 作为执行器动作判据，会把 MANUAL / DEBUG / CALIBRATION / AUTO_FIRE 全部锁死。
    // 本模块只暴露"硬件生命周期事实"（state() / stepResults()）；业务闸门必须由上层
    // 结合权威 SystemMode 判定（见 Atoms/Public/AppCoordinator::RunGate）。
    // 该方法已按授权移除，避免被误用为安全闸门。
    const std::vector<StepResult>& stepResults() const noexcept;
    const RuntimeContext& context() const noexcept;

private:
    struct RegisteredModule {
        RuntimeStep step;
        bool required;
        std::unique_ptr<ILifecycleModule> module;
        bool started{false};
    };

    static ModuleInitResult exceptionResult(const char* module_name,
                                            const char* message) noexcept;
    void failStart() noexcept;

    // 以当前状态与步骤结果构造只读快照；分配失败时退化为"只有状态"，不抛异常。
    RuntimeStartResult currentResult() const noexcept;

    RuntimeContext context_;
    RuntimeState state_{RuntimeState::Created};
    ModuleRegistrationError registration_error_{ModuleRegistrationError::None};
    std::vector<RegisteredModule> modules_;
    std::vector<StepResult> step_results_;
};

} // namespace marshland::runtime
