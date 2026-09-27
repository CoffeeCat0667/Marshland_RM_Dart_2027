// ============================================================================
// ILifecycle.hpp — 跨 ATOM 共享的进程生命周期契约
//
// 本文件从 Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp 上提而来：
// 原位置使其余 18 个 ATOM 必须反向包含 ATOM-01 的私有头才能接入启动流程，
// 形成 "Atom -> Atom" 的横向耦合。上提后依赖方向统一为 "Atom -> Public"。
//
// 权威来源：
//   Design/Atomic/ATOM-01_Runtime.md   内部原子操作第 5~11 条（模块初始化边界）、
//                                      第 12~14 条（readiness 汇总与待机/初始化失败）
//   Design/Interface.md                §4.3 启动顺序、§5.2 机构控制器共同约束
//   Design/Config.md                   §3.1 功能模式、§13.1 配置加载策略
//
// 归属说明：
//   * RuntimeStep / RuntimeState 属于 ATOM-01 的**硬件与服务**生命周期，
//     不是 Interface §6.2 的业务 SystemMode（SystemMode 见 MarshlandTypes.hpp）。
//     二者语义不同，禁止互相替代：
//       RuntimeState::Standby     = 启动流程的瞬态终点（"硬件已就绪"）
//       SystemMode::STANDBY       = 长期驻留的业务待机模式
//   * toString(...) 为 inline，任何仅链接 Marshland::Public 的模块都可直接使用，
//     不再需要链接 ATOM-01。
//
// 不作为：
//   * 不定义业务模式、控制源、自动流程状态（见 MarshlandTypes.hpp）。
//   * 不定义状态机、控制权仲裁与发射授权（见 ATOM-02）。
// ============================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace marshland::runtime {

// ---------------------------------------------------------------------------
// 生命周期状态（ATOM-01 私有语义，勿与 SystemMode 混用）
// ---------------------------------------------------------------------------
enum class RuntimeState {
    Created,
    Starting,
    Standby,
    InitFailed,
    ShuttingDown,
    Stopped,
};

// ---------------------------------------------------------------------------
// 启动步骤（ATOM-01_Runtime.md 内部原子操作第 1~11 条，顺序即执行顺序）
//
// 只登记**硬件与服务**初始化步骤。业务系统状态由 StateArbitrator 承担，
// 不得作为 RuntimeStep 塞进本枚举。
// ---------------------------------------------------------------------------
enum class RuntimeStep {
    CreateDiagnostics,
    LoadConfiguration,
    ValidateConfiguration,
    LoadPidParameters,
    InitializeSocketCan,
    InitializeMotorAndEncoderChannels,
    InitializeGpioAndLimitInputs,
    InitializeIicPca9685,
    InitializeServoOutputs,
    InitializeSbusAndVision,
    StartHttpAndWebSocket,
};

inline const char* toString(RuntimeState state) noexcept
{
    switch (state) {
    case RuntimeState::Created:      return "CREATED";
    case RuntimeState::Starting:     return "STARTING";
    case RuntimeState::Standby:      return "STANDBY";
    case RuntimeState::InitFailed:   return "INIT_FAILED";
    case RuntimeState::ShuttingDown: return "SHUTTING_DOWN";
    case RuntimeState::Stopped:      return "STOPPED";
    }
    return "UNKNOWN";
}

inline const char* toString(RuntimeStep step) noexcept
{
    switch (step) {
    case RuntimeStep::CreateDiagnostics:                return "create-diagnostics";
    case RuntimeStep::LoadConfiguration:                return "load-configuration";
    case RuntimeStep::ValidateConfiguration:            return "validate-configuration";
    case RuntimeStep::LoadPidParameters:                return "load-pid-parameters";
    case RuntimeStep::InitializeSocketCan:              return "initialize-socketcan";
    case RuntimeStep::InitializeMotorAndEncoderChannels:return "initialize-motor-encoder-channels";
    case RuntimeStep::InitializeGpioAndLimitInputs:     return "initialize-gpio-limit-inputs";
    case RuntimeStep::InitializeIicPca9685:             return "initialize-iic-pca9685";
    case RuntimeStep::InitializeServoOutputs:           return "initialize-servo-outputs";
    case RuntimeStep::InitializeSbusAndVision:          return "initialize-sbus-vision";
    case RuntimeStep::StartHttpAndWebSocket:            return "start-http-websocket";
    }
    return "unknown-step";
}

// ---------------------------------------------------------------------------
// 初始化上下文
//
// 注意（见 Atoms/ATOM-01_Runtime/BugLists.md ATOM01-BUG-006）：
// values 是无类型字符串字典，把配置解析完全外推给适配器，与 Config.md §17
// "配置表是唯一来源" 在类型层面缺乏保障。此处保持既有形态不变，仅上提。
// ---------------------------------------------------------------------------
struct RuntimeContext {
    // The coordinator deliberately does not parse configuration. Adapters may
    // use this path and the opaque values supplied by the owning application.
    std::string configuration_path;
    std::unordered_map<std::string, std::string> values;
};

// ---------------------------------------------------------------------------
// 单模块初始化结果
// ---------------------------------------------------------------------------
struct ModuleInitResult {
    bool success{false};
    std::string module_name;
    std::string error_message;
};

// ---------------------------------------------------------------------------
// 单步骤结果
// ---------------------------------------------------------------------------
struct StepResult {
    RuntimeStep step{RuntimeStep::CreateDiagnostics};
    bool required{true};
    bool attempted{false};
    ModuleInitResult result{};
};

// ---------------------------------------------------------------------------
// 模块生命周期接口
//
// 这是 ATOM-01_Runtime.md 内部原子操作第 5~11 条列举的全部下游模块的**唯一**
// 公共接入点。所有硬件与服务适配器实现本接口后即可通过
// RuntimeCoordinator::addModule() 接入启动流程。
// ---------------------------------------------------------------------------
class ILifecycleModule {
public:
    virtual ~ILifecycleModule() = default;

    virtual ModuleInitResult initialize(const RuntimeContext& context) = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace marshland::runtime
