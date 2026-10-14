// ============================================================================
// HttpWebModule.hpp — ATOM-16 HTTP 服务与 Web 前端（生命周期模块）
//
// 职责（Design/Atomic/ATOM-16_HTTP_Web.md「单一职责」）：
//   承载静态 Web 前端和 HTTP 请求，将每个请求分派为领域命令或查询；
//   不直接操作硬件。
//
// 接入方式：
//   实现 marshland::runtime::ILifecycleModule，由 RuntimeCoordinator 在
//   RuntimeStep::StartHttpAndWebSocket 步骤启动（Interface §4.3 第 8 步）。
//
// 依赖注入：
//   全部经构造函数注入 Public 抽象接口（指针可空）；缺失的下游能力在
//   运行期表现为 DEPENDENCY_UNAVAILABLE（503），绝不谎报成功（PLAN §7.3）。
//   本模块不持有、也不包含 AppCoordinator 等装配层私有头。
// ============================================================================
#pragma once

#include <memory>

#include "AppInterfaces.hpp"
#include "IConfigView.hpp"
#include "ILifecycle.hpp"
#include "IMonotonicClock.hpp"

namespace marshland::http_web {

class HttpWebModule final : public runtime::ILifecycleModule {
public:
    struct Dependencies {
        const config::IConfigView* config{nullptr};              // 必需：读取 http.* 配置
        const app::IMonotonicClock* clock{nullptr};              // 必需：响应 timestamp / 意图时间戳
        app::IStateReader* state_reader{nullptr};                // /api/state、/api/diagnostics
        app::IControlAuthority* control_authority{nullptr};      // 控制写路径
        app::IInputReporter* input_reporter{nullptr};            // web_online / estop / stop_fire 上报
    };

    explicit HttpWebModule(Dependencies deps);
    ~HttpWebModule() override;

    HttpWebModule(const HttpWebModule&) = delete;
    HttpWebModule& operator=(const HttpWebModule&) = delete;

    // 读取 http.* 服务配置（缺失/非法即失败，fail-closed），绑定监听地址与
    // 端口，注册路由，并在独立低优先级线程中启动服务。绑定失败返回
    // success=false + 原因，由 RuntimeCoordinator 决定回滚策略。
    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;

    // 停止服务并回收线程。幂等，可在任意状态调用。
    void shutdown() noexcept override;

    const char* name() const noexcept override { return "atom_16_http_web"; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace marshland::http_web
