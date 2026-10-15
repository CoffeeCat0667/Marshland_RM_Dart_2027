// ============================================================================
// civetweb_transport.cpp — 基于 CivetWeb 的 IWsTransport 实现
//
// civetweb 类型严格限制在本文件内（公共头只暴露 IWsTransport）。
//
// 并发要点：
//   - civetweb 工作线程发起 connect/ready/data/close 回调；
//   - 服务器推送线程经 SendText/CloseSession 主动写帧；
//   - mg_websocket_write 内部已使用 mg_lock_connection，跨线程写帧无需
//     额外加锁（见 civetweb.h mg_websocket_write 注释）；
//   - mg_connection 对象来自 civetweb 上下文内的连接池，在 mg_stop 之前
//     不会被释放；close 回调从 conns_ 表摘除后，SendText 按会话号找不到
//     连接即安全失败，不存在悬垂指针。
// ============================================================================
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "civetweb.h"

#include "atom17_websocket/ws_transport.hpp"

namespace marshland::atom17 {
namespace {

class CivetwebTransport final : public IWsTransport {
public:
    CivetwebTransport() = default;
    ~CivetwebTransport() override { Stop(); }

    CivetwebTransport(const CivetwebTransport&) = delete;
    CivetwebTransport& operator=(const CivetwebTransport&) = delete;

    bool Start(const char* listen_address, std::int64_t port,
               std::int64_t io_timeout_ms, IHandler& handler) noexcept override
    {
        if (ctx_ != nullptr) {
            return false;
        }
        handler_ = &handler;
        (void)io_timeout_ms; // 设计上的单次处理上限；不映射为 civetweb 会话空闲超时
                             //（空闲断开心跳超时由服务器核心判定，Config.md §12.1）。

        const std::string listening = std::string(listen_address) + ":" +
                                      std::to_string(port);
        const char* options[] = {
            "listening_ports", listening.c_str(),
            nullptr, nullptr};

        // 不调用 mg_init_library/mg_exit_library：features=0 时 mg_init_library
        // 成功也返回 0，无法区分成败；mg_start 内部会自动完成初始化
        //（legacy init，本进程不卸载库，无影响）。
        ctx_ = mg_start(nullptr, this, options);
        if (ctx_ == nullptr) {
            return false;
        }

        // "/" 为前缀匹配，接受任意路径的 WS 握手请求。
        mg_set_websocket_handler(ctx_, "/", &OnConnect, &OnReady, &OnData,
                                 &OnClose, this);
        return true;
    }

    void Stop() noexcept override
    {
        if (ctx_ != nullptr) {
            // mg_stop 会等待全部工作线程退出；返回后不再有任何回调。
            mg_stop(ctx_);
            ctx_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            conns_.clear();
        }
        handler_ = nullptr;
    }

    bool SendText(std::uint64_t session_id, const char* data,
                  std::uint64_t size) noexcept override
    {
        mg_connection* conn = Find(session_id);
        if (conn == nullptr) {
            return false;
        }
        const int written = mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_TEXT,
                                               data, static_cast<size_t>(size));
        return written == static_cast<int>(size);
    }

    void CloseSession(std::uint64_t session_id) noexcept override
    {
        mg_connection* conn = Find(session_id);
        if (conn == nullptr) {
            return;
        }
        mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, "", 0);
    }

private:
    mg_connection* Find(std::uint64_t session_id)
    {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        const auto it = conns_.find(session_id);
        return it != conns_.end() ? it->second : nullptr;
    }

    static std::uint64_t SessionIdOf(const mg_connection* conn)
    {
        return static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(mg_get_user_connection_data(conn)));
    }

    static int OnConnect(const mg_connection* /*conn*/, void* /*cbdata*/)
    {
        // 0 = 继续握手；握手结果在 ready/close 回调中确认。
        return 0;
    }

    static void OnReady(mg_connection* conn, void* cbdata)
    {
        auto* self = static_cast<CivetwebTransport*>(cbdata);
        const std::uint64_t id =
            self->next_session_id_.fetch_add(1, std::memory_order_relaxed);
        mg_set_user_connection_data(
            conn, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
        {
            std::lock_guard<std::mutex> lock(self->conns_mutex_);
            self->conns_[id] = conn;
        }
        if (self->handler_ != nullptr) {
            self->handler_->OnSessionOpen(id);
        }
    }

    static int OnData(mg_connection* conn, int bits, char* data, size_t data_len,
                      void* cbdata)
    {
        auto* self = static_cast<CivetwebTransport*>(cbdata);
        const int opcode = bits & 0x0F;
        const std::uint64_t id = SessionIdOf(conn);

        switch (opcode) {
        case MG_WEBSOCKET_OPCODE_TEXT:
            if (self->handler_ != nullptr && id != 0) {
                self->handler_->OnSessionMessage(id, data,
                                                 static_cast<std::uint64_t>(data_len));
            }
            return 1;
        case MG_WEBSOCKET_OPCODE_PING:
            // 协议层 ping 直接回 pong（与应用层 JSON 心跳互不替代）。
            mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_PONG, data, data_len);
            return 1;
        case MG_WEBSOCKET_OPCODE_PONG:
            return 1;
        default:
            // 关闭帧及其他控制/二进制帧：本通道只承载 JSON 文本，直接断开。
            return 0;
        }
    }

    static void OnClose(const mg_connection* conn, void* cbdata)
    {
        auto* self = static_cast<CivetwebTransport*>(cbdata);
        const std::uint64_t id = SessionIdOf(conn);
        if (id != 0) {
            {
                std::lock_guard<std::mutex> lock(self->conns_mutex_);
                self->conns_.erase(id);
            }
            if (self->handler_ != nullptr) {
                self->handler_->OnSessionClose(id);
            }
        }
    }

    mg_context* ctx_{nullptr};
    IHandler* handler_{nullptr};
    std::atomic<std::uint64_t> next_session_id_{1};
    std::mutex conns_mutex_;
    std::unordered_map<std::uint64_t, mg_connection*> conns_;
};

} // namespace

std::unique_ptr<IWsTransport> CreateCivetwebTransport()
{
    return std::make_unique<CivetwebTransport>();
}

} // namespace marshland::atom17
