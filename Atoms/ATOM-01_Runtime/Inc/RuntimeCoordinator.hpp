// RuntimeCoordinator.hpp - 支持多线程的生命周期管理器
//
// 设计理念：
// 1. 最小改动：只添加线程管理，不改变现有接口
// 2. 易于集成：每个 ATOM 只需提供一个 init() 和 run() 函数
// 3. 生命周期：init (顺序) → startThreads (并行) → stopThreads → shutdown (反序)
//
// 使用方式：
//   RuntimeCoordinator runtime;
//   runtime.registerModule("SBUS", init_fn, run_fn, cleanup_fn);
//   runtime.start();    // 自动启动所有线程
//   runtime.shutdown(); // 自动停止所有线程并清理
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace marshland::runtime {

enum class RuntimeState {
    Created,
    Starting,
    Standby,
    InitFailed,
    ShuttingDown,
    Stopped
};

inline const char* toString(RuntimeState state) noexcept {
    switch (state) {
        case RuntimeState::Created:      return "created";
        case RuntimeState::Starting:     return "starting";
        case RuntimeState::Standby:      return "standby";
        case RuntimeState::InitFailed:   return "init-failed";
        case RuntimeState::ShuttingDown: return "shutting-down";
        case RuntimeState::Stopped:      return "stopped";
    }
    return "unknown";
}

struct InitError {
    std::string step_name;
    std::string message;
};

// 模块注册结构
struct ModuleRegistration {
    std::string name;

    // 初始化函数（在主线程顺序执行）
    // 返回 true 表示成功，false 表示失败
    std::function<bool()> init;

    // 运行函数（在独立线程中执行，可选）
    // running 为 false 时应该退出循环
    std::function<void(std::atomic<bool>& running)> run;

    // 清理函数（在主线程反序执行）
    std::function<void()> cleanup;
};

class RuntimeCoordinator final {
public:
    RuntimeCoordinator() = default;
    ~RuntimeCoordinator();

    RuntimeCoordinator(const RuntimeCoordinator&) = delete;
    RuntimeCoordinator& operator=(const RuntimeCoordinator&) = delete;

    // 注册模块（在 start() 之前调用）
    // 模块按注册顺序初始化，按相反顺序清理
    void registerModule(ModuleRegistration registration);

    // 便捷重载：注册只需要初始化的模块（无独立线程）
    void registerModule(const std::string& name,
                       std::function<bool()> init,
                       std::function<void()> cleanup);

    // 便捷重载：注册需要独立线程的模块
    void registerModule(const std::string& name,
                       std::function<bool()> init,
                       std::function<void(std::atomic<bool>&)> run,
                       std::function<void()> cleanup);

    // 启动所有模块
    // 1. 顺序执行所有模块的 init()
    // 2. 为有 run() 的模块启动独立线程
    // 返回 true 表示全部成功（state = Standby）
    // 返回 false 表示失败（state = InitFailed），错误信息在 errors()
    bool start() noexcept;

    // 关闭所有模块
    // 1. 停止所有线程（设置 running_ = false 并 join）
    // 2. 反序执行所有模块的 cleanup()
    void shutdown() noexcept;

    // 状态查询
    RuntimeState state() const noexcept { return state_; }
    bool succeeded() const noexcept { return state_ == RuntimeState::Standby; }
    const std::vector<InitError>& errors() const noexcept { return errors_; }

private:
    struct RegisteredModule {
        std::string name;
        std::function<bool()> init;
        std::function<void(std::atomic<bool>&)> run;
        std::function<void()> cleanup;
        bool initialized{false};
    };

    RuntimeState state_{RuntimeState::Created};
    std::vector<InitError> errors_;
    std::vector<RegisteredModule> modules_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};

    bool tryInit(RegisteredModule& module) noexcept;
    void startThreads() noexcept;
    void stopThreads() noexcept;
    void rollback() noexcept;
};

} // namespace marshland::runtime
