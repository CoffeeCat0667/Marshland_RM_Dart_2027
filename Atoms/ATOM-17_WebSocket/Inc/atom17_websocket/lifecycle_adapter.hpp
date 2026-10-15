// ============================================================================
// lifecycle_adapter.hpp — ATOM-17 生命周期适配器
//
// 实现 marshland::runtime::ILifecycleModule（Atoms/Public/ILifecycle.hpp），
// 由上层装配层注册到 RuntimeStep::StartHttpAndWebSocket，遵循 Entry.cpp 的
// 既有组装模式（构造核心与适配器 → addModule → 检查注册结果）。
// ============================================================================
#pragma once

#include "ILifecycle.hpp"

namespace marshland::atom17 {

class WebSocketServer;

class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    // server 的生命期必须长于本适配器（与 Entry.cpp 中其他模块同一约定）。
    explicit LifecycleAdapter(WebSocketServer& server) noexcept : server_(server) {}

    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom17_websocket"; }

private:
    WebSocketServer& server_;
};

} // namespace marshland::atom17
