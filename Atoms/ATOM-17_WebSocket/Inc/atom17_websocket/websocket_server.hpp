// ============================================================================
// websocket_server.hpp — ATOM-17 WebSocket 只读状态服务器
//
// 职责（Design/Atomic/ATOM-17_WebSocket.md）：
//   - 独立监听（websocket.listen_address:websocket.port，默认 0.0.0.0:10002），
//     接受握手并创建只读订阅；
//   - 应用层 JSON 心跳：校验客户端 ping，回复关联相同 request_id 的 pong；
//     连续 3 个心跳周期（connection_timeout_ms）无有效数据判定断开并清理；
//   - 按 push_period_ms 周期把状态快照序列化为统一信封广播给全部订阅客户端；
//   - 收到控制类消息一律拒绝执行，返回改用 HTTP 的 error 信封；
//   - 会话数变化经 IInputReporter::ReportWebOnline 上报（唯一写入方）。
//
// 不职责：不执行任何控制命令，不改变执行器状态，不做控制权仲裁。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>

#include "AppInterfaces.hpp"
#include "IConfigView.hpp"
#include "IMonotonicClock.hpp"
#include "ws_transport.hpp"

namespace marshland::atom17 {

class WebSocketServer {
public:
    struct Dependencies {
        const config::IConfigView& config_view;
        const app::IMonotonicClock& clock;
        const app::IStateReader& state_reader;
        // 可空：装配层未接线时降级为不上报，不影响其余功能。
        app::IInputReporter* input_reporter{nullptr};
        // 可空：默认使用 CreateCivetwebTransport()；测试可注入替身。
        std::unique_ptr<IWsTransport> transport{nullptr};
    };

    explicit WebSocketServer(Dependencies deps);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&) = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    // 加载配置并开始监听。幂等：已运行时直接返回 true。
    // 失败返回 false，last_error() 给出首个失败原因。
    bool Start() noexcept;

    // 停止推送线程与监听，清理全部会话。幂等。
    // 不得在传输回调线程（OnSession*）内调用。
    void Stop() noexcept;

    bool is_running() const noexcept;
    std::uint64_t session_count() const noexcept;

    // 诊断计数（单调递增，自 Start 起累计）：
    std::uint64_t pushed_message_count() const noexcept;    // 成功广播的消息帧数
    std::uint64_t skipped_push_count() const noexcept;      // 因无数据源跳过的类型次数
    std::uint64_t rejected_message_count() const noexcept;  // 被拒绝的客户端消息数
    std::uint64_t timed_out_session_count() const noexcept; // 心跳超时清理的会话数

    const char* last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace marshland::atom17
