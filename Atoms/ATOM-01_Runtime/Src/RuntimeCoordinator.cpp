// RuntimeCoordinator.cpp - 支持多线程的实现
#include "RuntimeCoordinator.hpp"

#include <iostream>

namespace marshland::runtime {

RuntimeCoordinator::~RuntimeCoordinator() {
    shutdown();
}

void RuntimeCoordinator::registerModule(ModuleRegistration registration) {
    if (state_ != RuntimeState::Created) {
        std::cerr << "[Runtime] Cannot register module after start()" << std::endl;
        return;
    }

    RegisteredModule module;
    module.name = std::move(registration.name);
    module.init = std::move(registration.init);
    module.run = std::move(registration.run);
    module.cleanup = std::move(registration.cleanup);
    modules_.push_back(std::move(module));
}

void RuntimeCoordinator::registerModule(const std::string& name,
                                       std::function<bool()> init,
                                       std::function<void()> cleanup) {
    registerModule({name, std::move(init), nullptr, std::move(cleanup)});
}

void RuntimeCoordinator::registerModule(const std::string& name,
                                       std::function<bool()> init,
                                       std::function<void(std::atomic<bool>&)> run,
                                       std::function<void()> cleanup) {
    registerModule({name, std::move(init), std::move(run), std::move(cleanup)});
}

bool RuntimeCoordinator::tryInit(RegisteredModule& module) noexcept {
    std::cout << "[Runtime] Initializing " << module.name << "..." << std::endl;

    try {
        if (!module.init || !module.init()) {
            errors_.push_back({module.name, "init returned false"});
            return false;
        }
        module.initialized = true;
        return true;
    } catch (const std::exception& e) {
        errors_.push_back({module.name, std::string("exception: ") + e.what()});
        return false;
    } catch (...) {
        errors_.push_back({module.name, "unknown exception"});
        return false;
    }
}

void RuntimeCoordinator::startThreads() noexcept {
    std::cout << "[Runtime] Starting threads..." << std::endl;

    running_ = true;

    for (auto& module : modules_) {
        if (!module.initialized || !module.run) {
            continue;  // 跳过未初始化或无 run 函数的模块
        }

        try {
            threads_.emplace_back([this, &module]() {
                std::cout << "[Runtime] Thread started: " << module.name << std::endl;
                try {
                    module.run(running_);
                } catch (const std::exception& e) {
                    std::cerr << "[Runtime] Thread " << module.name
                             << " exception: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "[Runtime] Thread " << module.name
                             << " unknown exception" << std::endl;
                }
                std::cout << "[Runtime] Thread exited: " << module.name << std::endl;
            });
        } catch (...) {
            std::cerr << "[Runtime] Failed to start thread: " << module.name << std::endl;
        }
    }

    std::cout << "[Runtime] " << threads_.size() << " threads started" << std::endl;
}

void RuntimeCoordinator::stopThreads() noexcept {
    if (threads_.empty()) {
        return;
    }

    std::cout << "[Runtime] Stopping threads..." << std::endl;

    running_ = false;

    // 等待所有线程退出
    for (auto& t : threads_) {
        try {
            if (t.joinable()) {
                t.join();
            }
        } catch (...) {
            // 忽略 join 异常
        }
    }

    threads_.clear();
    std::cout << "[Runtime] All threads stopped" << std::endl;
}

void RuntimeCoordinator::rollback() noexcept {
    std::cout << "[Runtime] Rollback: cleaning up initialized modules..." << std::endl;

    // 反序清理已初始化的模块
    for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
        if (!it->initialized || !it->cleanup) {
            continue;
        }

        try {
            std::cout << "[Runtime] Cleaning up " << it->name << std::endl;
            it->cleanup();
            it->initialized = false;
        } catch (const std::exception& e) {
            std::cerr << "[Runtime] Cleanup error for " << it->name
                     << ": " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[Runtime] Cleanup error for " << it->name << std::endl;
        }
    }
}

bool RuntimeCoordinator::start() noexcept {
    if (state_ != RuntimeState::Created) {
        std::cerr << "[Runtime] Already started or stopped" << std::endl;
        return state_ == RuntimeState::Standby;
    }

    state_ = RuntimeState::Starting;
    errors_.clear();

    // 第一阶段：顺序初始化所有模块
    for (auto& module : modules_) {
        if (!tryInit(module)) {
            state_ = RuntimeState::InitFailed;
            rollback();
            return false;
        }
    }

    // 第二阶段：启动所有线程
    startThreads();

    // 全部成功
    state_ = RuntimeState::Standby;
    std::cout << "[Runtime] All modules started successfully" << std::endl;
    return true;
}

void RuntimeCoordinator::shutdown() noexcept {
    if (state_ == RuntimeState::Stopped || state_ == RuntimeState::Created) {
        return;
    }

    std::cout << "[Runtime] Shutting down..." << std::endl;
    state_ = RuntimeState::ShuttingDown;

    // 第一步：停止所有线程
    stopThreads();

    // 第二步：反序清理所有模块
    rollback();

    state_ = RuntimeState::Stopped;
    std::cout << "[Runtime] Stopped" << std::endl;
}

} // namespace marshland::runtime
