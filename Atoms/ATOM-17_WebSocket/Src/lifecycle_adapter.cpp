#include "atom17_websocket/lifecycle_adapter.hpp"

#include "atom17_websocket/websocket_server.hpp"

namespace marshland::atom17 {

runtime::ModuleInitResult LifecycleAdapter::initialize(
    const runtime::RuntimeContext& /*context*/)
{
    runtime::ModuleInitResult result;
    result.module_name = name();

    // 配置经构造注入的 IConfigView 查询（见 WebSocketServer），
    // 不从 RuntimeContext::values 解析数值。
    if (server_.Start()) {
        result.success = true;
        return result;
    }

    result.success = false;
    result.error_message = server_.last_error();
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    server_.Stop();
}

} // namespace marshland::atom17
