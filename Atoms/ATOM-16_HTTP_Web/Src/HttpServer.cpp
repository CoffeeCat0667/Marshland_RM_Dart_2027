// ============================================================================
// HttpServer.cpp — ATOM-16 HTTP 服务实现（实现 Public/Headers/Web.hpp 声明）
//
// 本模块功能（2026-10-17 用户裁定，仅此两项）：
//   1. 经 motor::IMotorControl::state() 读取 0x201~0x206 六个电机的实时转速
//      与温度，经 SSE（GET /api/stream）推送前端实时显示；GET /api/motors
//      提供一次性快照。
//   2. 经 state::getGlobalStateMachine() 读取控制源；仅当控制源为 WEB 时，
//      接受前端控制数据（POST /api/control/channels）写入 WebData。
//
// 附属行为：
//   * SSE 连接即 WEB 在线：仅写 WebData.online，由主线程主动读取；
//     最后一个连接断开时离线 + 通道全部清零（fail-safe）。
//   * 静态资源：static_root 非空时挂载到 /。
//
// httplib / nlohmann 只在本翻译单元包含（CMake PRIVATE）。
// ============================================================================
#include "Web.hpp"
#include "IStateMachine.hpp"

#include "httplib/httplib.h"
#include "nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace marshland::web {
namespace {

using nlohmann::json;

// 模块内部错误码（响应协议词汇，非对外契约）
enum class ErrorCode {
    INVALID_JSON, INVALID_ARGUMENT, CONTROL_PREEMPTED,
    NOT_FOUND, PAYLOAD_TOO_LARGE, DEPENDENCY_UNAVAILABLE, INTERNAL_ERROR
};

const char* ToString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::INVALID_JSON:           return "INVALID_JSON";
    case ErrorCode::INVALID_ARGUMENT:       return "INVALID_ARGUMENT";
    case ErrorCode::CONTROL_PREEMPTED:      return "CONTROL_PREEMPTED";
    case ErrorCode::NOT_FOUND:              return "NOT_FOUND";
    case ErrorCode::PAYLOAD_TOO_LARGE:      return "PAYLOAD_TOO_LARGE";
    case ErrorCode::DEPENDENCY_UNAVAILABLE: return "DEPENDENCY_UNAVAILABLE";
    case ErrorCode::INTERNAL_ERROR:         return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

int ToHttpStatus(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::INVALID_JSON:           return 400;
    case ErrorCode::INVALID_ARGUMENT:       return 400;
    case ErrorCode::CONTROL_PREEMPTED:      return 409;
    case ErrorCode::NOT_FOUND:              return 404;
    case ErrorCode::PAYLOAD_TOO_LARGE:      return 413;
    case ErrorCode::DEPENDENCY_UNAVAILABLE: return 503;
    case ErrorCode::INTERNAL_ERROR:         return 500;
    }
    return 500;
}

// SSE 推送周期：电机实时显示用 50 ms（20 Hz）
constexpr std::int64_t kSsePushPeriodMs = 50;

// 允许前端写入的通道（速度/输入通道；CH1 保留、CH5 控制权、CH6 急停、
// CH7 发射、CH14~16 保留一律不收）
bool channelWritable(int ch) noexcept
{
    switch (ch) {
    case 2: case 3: case 4:                 // 蓄力 / 换弹 / YAW（机构速度）
    case 8: case 9: case 10: case 11:       // 蓄力/换弹左右电机
    case 12: case 13:                       // 变力 rpm / 供弹输入
        return true;
    default:
        return false;
    }
}

std::optional<std::string> getString(const json& j, const char* key)
{
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? std::optional(it->get<std::string>()) : std::nullopt;
}

class Server {
public:
    Server(HttpServerConfig cfg, WebData& data, const motor::IMotorControl* motors)
        : cfg_(std::move(cfg)), data_(data), motors_(motors)
    {
    }

    // 返回 false 表示绑定失败
    bool run(const std::atomic<bool>& stop_requested)
    {
        setup();

        zeroChannels();   // 启动即清零（fail-safe）
        setOnline(false);
        if (http_.bind_to_port(cfg_.listen_address, cfg_.port) <= 0) return false;

        // 停止标志监听：置位即停止监听（SSE 连接随连接关闭自然终止）
        std::thread watcher([this, &stop_requested] {
            while (!stop_requested.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (stopped_.load(std::memory_order_acquire)) return;
            }
            http_.stop();
        });

        (void)http_.listen_after_bind();  // 阻塞至 stop()
        stopped_.store(true, std::memory_order_release);
        watcher.join();

        zeroChannels();   // 退出前清零 + 离线
        setOnline(false);
        return true;
    }

private:
    HttpServerConfig          cfg_;
    WebData&                  data_;
    const motor::IMotorControl* motors_;
    httplib::Server           http_;
    std::atomic<std::uint64_t> request_counter_{0};
    std::atomic<int>           sse_clients_{0};
    std::atomic<bool>          stopped_{false};

    // ---- WebData / 全局状态机 --------------------------------------------

    void setOnline(bool online)
    {
        std::lock_guard lock(data_.mutex);
        data_.online = online;
    }

    void zeroChannels()
    {
        std::lock_guard lock(data_.mutex);
        data_.scaled_channels.fill(0);
    }

    // ---- 响应工具 ----------------------------------------------------------

    std::string nextRequestId()
    {
        return "w" + std::to_string(request_counter_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    std::string requestId(const httplib::Request& req, const json& body)
    {
        if (!body.is_discarded()) {
            if (auto s = getString(body, "request_id"); s && !s->empty()) return *s;
        }
        if (req.has_param("request_id")) {
            const auto v = req.get_param_value("request_id");
            if (!v.empty()) return v;
        }
        return nextRequestId();
    }

    json errorBody(const std::string& rid, ErrorCode code, const std::string& message)
    {
        return {{"request_id", rid},
                {"ok", false},
                {"error", {{"code", ToString(code)}, {"message", message}, {"details", json::object()}}}};
    }

    void sendJson(httplib::Response& res, int status, json body)
    {
        std::string text = body.dump();
        if (text.size() > cfg_.response_body_limit_bytes) {   // 超限 500
            status = 500;
            text = errorBody(nextRequestId(), ErrorCode::INTERNAL_ERROR,
                             "response body exceeds configured limit").dump();
        }
        res.status = status;
        res.set_content(std::move(text), "application/json; charset=utf-8");
    }

    void sendError(httplib::Response& res, const std::string& rid,
                   ErrorCode code, const std::string& message)
    {
        sendJson(res, ToHttpStatus(code), errorBody(rid, code, message));
    }

    // ---- 功能 1：电机状态读取 ----------------------------------------------

    json buildMotorsJson()
    {
        json motors = json::array();
        if (motors_ != nullptr) {
            for (std::size_t i = 0; i < can::kMaxMotorNodes; ++i) {
                const std::uint32_t node = can::kMotorNodeMin + static_cast<std::uint32_t>(i);
                motor::MotorState st;
                const can::CanResult r = motors_->state(node, st);
                if (r.failed) {
                    motors.push_back({{"node", node}, {"online", false},
                                      {"rpm", nullptr}, {"temperature", nullptr}});
                } else {
                    motors.push_back({{"node", node},
                                      {"online", st.online},
                                      {"rpm", st.rpm},
                                      {"temperature", st.temperature}});
                }
            }
        }
        return motors;
    }

    json buildStatusJson()
    {
        json j;
        j["type"] = "status";
        if (state::IStateMachine* sm = state::getGlobalStateMachine()) {
            j["control_source"] = state::toString(sm->control_source());
        } else {
            j["control_source"] = nullptr;
        }
        j["motors"] = motors_ != nullptr ? buildMotorsJson() : json(nullptr);
        {
            std::lock_guard lock(data_.mutex);
            j["web_online"] = data_.online;
        }
        return j;
    }

    // ---- 路由 ---------------------------------------------------------------

    void setup()
    {
        http_.set_payload_max_length(cfg_.request_body_limit_bytes);

        http_.set_error_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (res.body.find("\"ok\":false") != std::string::npos) return;
            ErrorCode code = ErrorCode::INTERNAL_ERROR;
            if (res.status == 404) code = ErrorCode::NOT_FOUND;
            else if (res.status == 413) code = ErrorCode::PAYLOAD_TOO_LARGE;
            sendError(res, requestId(req, json()), code, "request failed");
        });
        http_.set_exception_handler([this](const httplib::Request& req, httplib::Response& res,
                                           std::exception_ptr) {
            sendError(res, requestId(req, json()), ErrorCode::INTERNAL_ERROR,
                      "unhandled handler exception");
        });

        // SSE 实时推送：控制源 + 六电机转速温度
        http_.Get("/api/stream", [this](const httplib::Request&, httplib::Response& res) {
            sse_clients_.fetch_add(1, std::memory_order_acq_rel);
            setOnline(true);
            res.set_chunked_content_provider(
                "text/event-stream",
                [this](std::size_t, httplib::DataSink& sink) -> bool {
                    if (!sink.is_writable()) return false;
                    const std::string frame =
                        "event: status\ndata: " + buildStatusJson().dump() + "\n\n";
                    if (!sink.write(frame.data(), frame.size())) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(kSsePushPeriodMs));
                    return true;
                },
                // 最后一个连接断开：离线 + 通道清零（fail-safe）
                [this](bool) {
                    if (sse_clients_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        setOnline(false);
                        zeroChannels();
                    }
                });
        });

        // 一次性电机快照（非 SSE 客户端用）
        http_.Get("/api/motors", [this](const httplib::Request& q, httplib::Response& s) {
            const std::string rid = requestId(q, json());
            if (motors_ == nullptr) {
                sendError(s, rid, ErrorCode::DEPENDENCY_UNAVAILABLE,
                          "motor state source not connected");
                return;
            }
            sendJson(s, 200, {{"request_id", rid}, {"ok", true},
                              {"motors", buildMotorsJson()}});
        });

        // 功能 2：前端控制数据 -> WebData（仅控制源为 WEB 时受理）
        http_.Post("/api/control/channels", [this](const httplib::Request& q, httplib::Response& s) {
            const json body = json::parse(q.body, nullptr, false);
            if (body.is_discarded()) {
                sendError(s, nextRequestId(), ErrorCode::INVALID_JSON,
                          "request body is not valid JSON");
                return;
            }
            const std::string rid = requestId(q, body);

            state::IStateMachine* sm = state::getGlobalStateMachine();
            if (sm == nullptr) {
                sendError(s, rid, ErrorCode::DEPENDENCY_UNAVAILABLE,
                          "global state machine not registered");
                return;
            }
            if (sm->control_source() != state::ControlSource::WEB) {
                sendError(s, rid, ErrorCode::CONTROL_PREEMPTED,
                          "control source is not WEB");
                return;
            }

            const auto channels = body.find("channels");
            if (channels == body.end() || !channels->is_object()) {
                sendError(s, rid, ErrorCode::INVALID_ARGUMENT,
                          "field 'channels' (object) is required");
                return;
            }

            std::lock_guard lock(data_.mutex);
            for (const auto& [key, value] : channels->items()) {
                int ch = 0;
                try {
                    ch = std::stoi(key);
                } catch (...) {
                    sendError(s, rid, ErrorCode::INVALID_ARGUMENT,
                              "channel key must be an integer string");
                    return;
                }
                if (!channelWritable(ch) || !value.is_number_integer()) {
                    sendError(s, rid, ErrorCode::INVALID_ARGUMENT,
                              "channel not writable or value not integer: " + key);
                    return;
                }
                data_.scaled_channels[static_cast<std::size_t>(ch - 1)] =
                    value.get<std::int32_t>();
            }
            data_.online = true;  // 有控制数据即视为在线活动

            sendJson(s, 200, {{"request_id", rid}, {"ok", true}, {"accepted", true}});
        });

        // 静态资源：显式路由优先于挂载点；静态请求不触发任何机构动作
        if (cfg_.static_root != nullptr && !http_.set_mount_point("/", cfg_.static_root)) {
            throw std::runtime_error("invalid static root directory");
        }
    }
};

} // namespace

int RunHttpServer(HttpServerConfig config, WebData& data,
                  const motor::IMotorControl* motors,
                  const std::atomic<bool>& stop_requested) noexcept
{
    try {
        return Server(std::move(config), data, motors).run(stop_requested) ? 0 : 1;
    } catch (...) {
        return 1;
    }
}

} // namespace marshland::web
