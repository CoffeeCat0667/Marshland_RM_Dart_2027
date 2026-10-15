// ============================================================================
// websocket_server.cpp — ATOM-17 服务器核心
//
// 线程模型：
//   - 推送线程（pthread，栈 1 MiB，Config 线程栈设计值）：按 push_period_ms
//     周期拷出状态快照 → 锁外序列化 → 广播；同时按心跳超时扫描会话。
//   - 传输工作线程（civetweb）：发起 OnSession* 回调。
//   - 生命周期线程（RuntimeCoordinator）：调用 Start/Stop。
// 共享会话表由 sessions_mutex_ 保护；Start/Stop 由 control_mutex_ 串行化。
// ============================================================================
#include "atom17_websocket/websocket_server.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <pthread.h>
#include <time.h>

#include "atom17_websocket/ws_config.hpp"
#include "atom17_websocket/ws_snapshot.hpp"
#include "message_builder.hpp"

namespace marshland::atom17 {
namespace {

// 推送线程栈：Config 线程设计值 1 MiB（thread.* 的配置键名尚未在
// ConfigSchema 中确认到 WebSocket 专用键，确认后可改由 IConfigView 注入）。
constexpr std::size_t kPushThreadStackBytes = 1024 * 1024;

// 每周期扫描的状态事件缓冲容量（仅用于提取最近一次控制源变更原因）。
constexpr std::size_t kEventScanCapacity = 64;

// 客户端消息错误码（Config.md §12.1 只定义 error 信封格式，未枚举取值）。
constexpr const char* kCodeBadMessage = "bad_message";
constexpr const char* kCodeVersionMismatch = "version_mismatch";
constexpr const char* kCodeReadOnlyChannel = "read_only_channel";

bool StartPushThread(pthread_t& tid, void* (*entry)(void*), void* arg) noexcept
{
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        return false;
    }
    pthread_attr_setstacksize(&attr, kPushThreadStackBytes);
    const int rc = pthread_create(&tid, &attr, entry, arg);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

void SleepMicros(std::int64_t micros) noexcept
{
    if (micros <= 0) {
        return;
    }
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(micros / 1000000);
    ts.tv_nsec = static_cast<long>((micros % 1000000) * 1000);
    // 被信号打断时按剩余时间继续睡眠。
    while (::nanosleep(&ts, &ts) != 0) {
    }
}

} // namespace

struct WebSocketServer::Impl final : IWsTransport::IHandler {
    explicit Impl(Dependencies deps) : deps_(std::move(deps)) {}
    ~Impl() { Stop(); }

    bool Start() noexcept
    {
        try {
            return StartInternal();
        } catch (...) {
            std::lock_guard<std::mutex> lock(control_mutex_);
            error_ = "unexpected exception during start";
            return false;
        }
    }

    bool StartInternal()
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (running_) {
            return true;
        }

        WsConfigLoadResult loaded = LoadWsConfig(deps_.config_view);
        if (loaded.status != config::ConfigStatus::OK) {
            error_ = "config key '" + loaded.failed_key +
                     "': " + config::ToString(loaded.status);
            return false;
        }
        config_ = std::move(loaded.config);

        if (deps_.transport) {
            transport_ = std::move(deps_.transport);
        } else {
            transport_ = CreateCivetwebTransport();
        }
        if (!transport_ ||
            !transport_->Start(config_.listen_address.c_str(), config_.port,
                               config_.io_timeout_ms, *this)) {
            error_ = "listen failed on " + config_.listen_address + ":" +
                     std::to_string(config_.port);
            transport_.reset();
            return false;
        }

        stop_requested_.store(false, std::memory_order_relaxed);
        if (!StartPushThread(push_thread_, &Impl::PushThreadEntry, this)) {
            error_ = "failed to create push thread";
            transport_->Stop();
            transport_.reset();
            return false;
        }
        push_thread_started_ = true;

        pushed_ = skipped_ = rejected_ = timed_out_ = 0;
        error_.clear();
        running_ = true;
        return true;
    }

    void Stop() noexcept
    {
        std::unique_lock<std::mutex> lock(control_mutex_);
        if (!running_) {
            return;
        }
        stop_requested_.store(true, std::memory_order_relaxed);
        if (push_thread_started_) {
            lock.unlock();
            // 推送线程可能在长时间睡眠，先唤醒等待路径不在这里做；
            // nanosleep 最长 sleep 一个推送周期（20 ms 量级），直接 join。
            pthread_join(push_thread_, nullptr);
            lock.lock();
            push_thread_started_ = false;
        }
        transport_->Stop();

        bool report_offline = false;
        {
            std::lock_guard<std::mutex> sessions_lock(sessions_mutex_);
            sessions_.clear();
            if (web_online_reported_) {
                web_online_reported_ = false;
                report_offline = true;
            }
        }
        if (report_offline && deps_.input_reporter != nullptr) {
            deps_.input_reporter->ReportWebOnline(false);
        }
        running_ = false;
    }

    // ---- IWsTransport::IHandler（传输工作线程上下文） ----------------------

    void OnSessionOpen(std::uint64_t session_id) noexcept override
    {
        const std::int64_t now = deps_.clock.nowMicros();
        bool report_online = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            sessions_[session_id] = now;
            if (sessions_.size() == 1 && !web_online_reported_) {
                web_online_reported_ = true;
                report_online = true;
            }
        }
        if (report_online && deps_.input_reporter != nullptr) {
            deps_.input_reporter->ReportWebOnline(true);
        }
    }

    void OnSessionMessage(std::uint64_t session_id, const char* data,
                          std::uint64_t size) noexcept override
    {
        TouchSession(session_id);
        HandleClientMessage(session_id, data, size);
    }

    void OnSessionClose(std::uint64_t session_id) noexcept override
    {
        RemoveSession(session_id);
    }

    // ---- 会话表 ------------------------------------------------------------

    void TouchSession(std::uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = sessions_.find(session_id);
        if (it != sessions_.end()) {
            it->second = deps_.clock.nowMicros();
        }
    }

    void RemoveSession(std::uint64_t session_id)
    {
        bool report_offline = false;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            if (sessions_.erase(session_id) == 0) {
                return;
            }
            if (sessions_.empty() && web_online_reported_) {
                web_online_reported_ = false;
                report_offline = true;
            }
        }
        if (report_offline && deps_.input_reporter != nullptr) {
            deps_.input_reporter->ReportWebOnline(false);
        }
    }

    std::vector<std::uint64_t> SessionIds()
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        std::vector<std::uint64_t> ids;
        ids.reserve(sessions_.size());
        for (const auto& [id, last] : sessions_) {
            (void)last;
            ids.push_back(id);
        }
        return ids;
    }

    // ---- 客户端消息处理 ------------------------------------------------------

    void SendJson(std::uint64_t session_id, const nlohmann::ordered_json& message)
    {
        const std::string text = message.dump();
        if (!transport_->SendText(session_id, text.data(), text.size())) {
            RemoveSession(session_id);
        }
    }

    void SendError(std::uint64_t session_id, const char* code, const char* message,
                   const nlohmann::json* request_id)
    {
        SendJson(session_id, detail::BuildError(config_, code, message,
                                                nlohmann::ordered_json::object(),
                                                request_id));
    }

    void HandleClientMessage(std::uint64_t session_id, const char* data,
                             std::uint64_t size)
    {
        nlohmann::json message;
        try {
            message = nlohmann::json::parse(data, data + size, nullptr, false);
        } catch (...) {
            message = nlohmann::json::value_t::discarded;
        }
        if (message.is_discarded() || !message.is_object()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            SendError(session_id, kCodeBadMessage,
                      "message is not a valid JSON object", nullptr);
            return;
        }

        // request_id 原样回显（字符串/数字/null 均按原 JSON 值处理）。
        const nlohmann::json* request_id = nullptr;
        const auto request_id_it = message.find("request_id");
        if (request_id_it != message.end()) {
            request_id = &(*request_id_it);
        }

        // 版本校验：客户端声明的版本必须与服务端消息版本一致。
        const auto version_it = message.find("version");
        if (version_it == message.end() || !version_it->is_string() ||
            version_it->get_ref<const std::string&>() != config_.message_version) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            SendError(session_id, kCodeVersionMismatch,
                      "message version mismatch", request_id);
            return;
        }

        const auto type_it = message.find("type");
        if (type_it == message.end() || !type_it->is_string()) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            SendError(session_id, kCodeBadMessage, "missing message type",
                      request_id);
            return;
        }
        const std::string& type = type_it->get_ref<const std::string&>();

        if (type == "ping") {
            // 应用层心跳：回复关联相同 request_id 的 pong（Config.md §12.1）。
            SendJson(session_id, detail::BuildPong(config_, request_id));
            return;
        }

        // 其余一切客户端消息（包括控制类）一律拒绝执行，提示改用 HTTP。
        rejected_.fetch_add(1, std::memory_order_relaxed);
        SendError(session_id, kCodeReadOnlyChannel,
                  "WebSocket is a read-only state channel; "
                  "use HTTP for control commands",
                  request_id);
    }

    // ---- 推送管线 ------------------------------------------------------------

    static void* PushThreadEntry(void* arg)
    {
        static_cast<Impl*>(arg)->PushLoop();
        return nullptr;
    }

    StateSnapshot CaptureSnapshot()
    {
        StateSnapshot snapshot;
        {
            // 租约内只做按值拷出；序列化与网络发送均在锁外完成。
            const app::StateReadView view = deps_.state_reader.ReadState();
            snapshot.context = view.context();
            snapshot.run_gate = view.run_gate();
            snapshot.hardware_ready = view.hardware_ready();
            snapshot.estop_latched = view.estop_latched();
            snapshot.run_id = view.run_id();
            snapshot.current_shot = view.current_shot();
        }

        state_arbitration::StateEvent events[kEventScanCapacity];
        const std::size_t count =
            deps_.state_reader.CopyEvents(events, kEventScanCapacity);
        for (std::size_t i = 0; i < count; ++i) {
            if (events[i].kind ==
                state_arbitration::EventKind::CONTROL_SOURCE_CHANGE) {
                snapshot.preempt_reason = events[i].reason;
                snapshot.has_preempt_reason = true;
            }
        }
        return snapshot;
    }

    void Broadcast(const std::string& text)
    {
        const std::vector<std::uint64_t> ids = SessionIds();
        for (const std::uint64_t id : ids) {
            if (!transport_->SendText(id, text.data(), text.size())) {
                RemoveSession(id);
            } else {
                pushed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void SweepTimeouts()
    {
        const std::int64_t now = deps_.clock.nowMicros();
        const std::int64_t timeout_us = config_.connection_timeout_ms * 1000;
        std::vector<std::uint64_t> expired;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            for (const auto& [id, last_activity] : sessions_) {
                if (now - last_activity > timeout_us) {
                    expired.push_back(id);
                }
            }
        }
        for (const std::uint64_t id : expired) {
            timed_out_.fetch_add(1, std::memory_order_relaxed);
            transport_->CloseSession(id);
            RemoveSession(id);
        }
    }

    void PushOnce()
    {
        const StateSnapshot snapshot = CaptureSnapshot();
        for (const auto& def : config_.message_types) {
            // error 仅响应式推送（拒绝/协议错误），不做周期广播。
            if (def.type == "error") {
                continue;
            }
            auto payload = detail::BuildPayload(def, snapshot, config_);
            if (!payload) {
                skipped_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const auto envelope = detail::BuildEnvelope(config_, def.type.c_str(),
                                                        std::move(*payload),
                                                        nullptr);
            Broadcast(envelope.dump());
        }
    }

    void PushLoop()
    {
        const std::int64_t period_us = config_.push_period_ms * 1000;
        std::int64_t next = deps_.clock.nowMicros();
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            next += period_us;
            const std::int64_t now = deps_.clock.nowMicros();
            if (now < next) {
                SleepMicros(next - now);
            }
            if (stop_requested_.load(std::memory_order_relaxed)) {
                break;
            }
            SweepTimeouts();
            PushOnce();
        }
    }

    Dependencies deps_;
    WsConfig config_{};
    std::unique_ptr<IWsTransport> transport_;

    std::mutex control_mutex_;
    bool running_{false};
    std::string error_{"not started"};

    std::mutex sessions_mutex_;
    std::unordered_map<std::uint64_t, std::int64_t> sessions_;  // id → 最近有效活动时间 us
    bool web_online_reported_{false};

    std::atomic<bool> stop_requested_{false};
    pthread_t push_thread_{};
    bool push_thread_started_{false};

    std::atomic<std::uint64_t> pushed_{0};
    std::atomic<std::uint64_t> skipped_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> timed_out_{0};
};

WebSocketServer::WebSocketServer(Dependencies deps)
    : impl_(std::make_unique<Impl>(std::move(deps)))
{
}

WebSocketServer::~WebSocketServer() = default;

bool WebSocketServer::Start() noexcept { return impl_->Start(); }

void WebSocketServer::Stop() noexcept { impl_->Stop(); }

bool WebSocketServer::is_running() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex_);
    return impl_->running_;
}

std::uint64_t WebSocketServer::session_count() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->sessions_mutex_);
    return impl_->sessions_.size();
}

std::uint64_t WebSocketServer::pushed_message_count() const noexcept
{
    return impl_->pushed_.load(std::memory_order_relaxed);
}

std::uint64_t WebSocketServer::skipped_push_count() const noexcept
{
    return impl_->skipped_.load(std::memory_order_relaxed);
}

std::uint64_t WebSocketServer::rejected_message_count() const noexcept
{
    return impl_->rejected_.load(std::memory_order_relaxed);
}

std::uint64_t WebSocketServer::timed_out_session_count() const noexcept
{
    return impl_->timed_out_.load(std::memory_order_relaxed);
}

const char* WebSocketServer::last_error() const noexcept
{
    std::lock_guard<std::mutex> lock(impl_->control_mutex_);
    return impl_->error_.c_str();
}

} // namespace marshland::atom17
