// ============================================================================
// HttpWebModule.cpp — ATOM-16 生命周期与服务实现
//
// 阶段范围（PLAN §6 第 1~5 步）：
//   * 读取 http.* 服务配置并绑定监听（缺失/非法即 fail-closed）；
//   * 请求体上限（413）、响应体上限（500）、handler 处理超时（408）；
//   * 统一错误 JSON、request_id 生成与回显；
//   * GET /api/state 全量实现（经 IStateReader）；
//   * 其余 8 条路由登记为桩，返回 DEPENDENCY_UNAVAILABLE（PLAN §7.3）。
//
// httplib.h 只在本翻译单元包含（CMake PRIVATE include）。
// ============================================================================
#include "HttpWebModule.hpp"

#include "ApiError.hpp"
#include "ConfigKeys.hpp"
#include "JsonCodec.hpp"
#include "MarshlandTypes.hpp"

#include "httplib/httplib.h"

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#endif

namespace marshland::http_web {
namespace {

// Interface §6.6：自动流程固定四发（total_shots = 4）。
// TODO(PLAN §7.5)：如 baseline 域后续给出配置键，改为经 IConfigView 读取。
constexpr std::uint64_t kTotalShots = 4;

struct ServiceSettings {
    std::string   listen_address;
    std::int64_t  port{0};
    std::int64_t  request_body_limit_bytes{0};
    std::int64_t  response_body_limit_bytes{0};
    std::int64_t  request_timeout_ms{0};
};

// 读取单个配置项；任何非 OK 状态都使初始化失败并指明键名与原因。
// 返回空串表示成功，否则为错误描述（fail-closed，禁止静默用默认值）。
std::string loadSettings(const config::IConfigView& config, ServiceSettings& out)
{
    using config::ConfigStatus;

    {
        const char* data = nullptr;
        std::uint64_t size = 0;
        const ConfigStatus st = config.getString(config_keys::kListenAddress, data, size);
        if (st != ConfigStatus::OK || data == nullptr || size == 0) {
            return std::string(config_keys::kListenAddress) + ": " + config::ToString(st);
        }
        out.listen_address.assign(data, size);
    }

    const auto readInt = [&](const char* key, std::int64_t lo, std::int64_t hi,
                             std::int64_t& dst) -> std::string {
        std::int64_t v = 0;
        const ConfigStatus st = config.getInt(key, v);
        if (st != ConfigStatus::OK) {
            return std::string(key) + ": " + config::ToString(st);
        }
        if (v < lo || v > hi) {
            return std::string(key) + ": value out of accepted range";
        }
        dst = v;
        return {};
    };

    if (auto e = readInt(config_keys::kPort, 1, 65535, out.port); !e.empty()) return e;
    if (auto e = readInt(config_keys::kRequestBodyLimitBytes, 1, (std::int64_t{1} << 30),
                         out.request_body_limit_bytes); !e.empty()) return e;
    if (auto e = readInt(config_keys::kResponseBodyLimitBytes, 1, (std::int64_t{1} << 30),
                         out.response_body_limit_bytes); !e.empty()) return e;
    if (auto e = readInt(config_keys::kRequestTimeoutMs, 1, 60000,
                         out.request_timeout_ms); !e.empty()) return e;
    return {};
}

// 网络线程调度优先级取低（Config §15：网络任务不得阻塞控制与安全任务）。
// 无权限提升 nice 值时忽略失败（best-effort）。
void lowerThreadPriority() noexcept
{
#ifdef __linux__
    if (setpriority(PRIO_PROCESS, 0, 5) != 0) {
        // best-effort：权限不足不视为错误
    }
#endif
}

} // namespace

struct HttpWebModule::Impl {
    explicit Impl(Dependencies d) : deps(d) {}

    Dependencies     deps;
    ServiceSettings  settings;
    httplib::Server  server;
    std::thread      server_thread;
    std::atomic<bool> running{false};
    std::atomic<std::uint64_t> request_counter{0};

    std::string nextRequestId()
    {
        return "w" + std::to_string(request_counter.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    // POST 请求体中的 request_id 回显；缺失/非法时生成新 id。
    std::string requestIdFromBody(const httplib::Request& req)
    {
        const JsonBody body = JsonBody::Parse(req.body);
        if (body.valid()) {
            if (auto s = body.getString("request_id"); s && !s->empty()) {
                return *s;
            }
            if (auto n = body.getInt("request_id")) {
                return std::to_string(*n);
            }
        }
        return nextRequestId();
    }

    std::string requestIdFromQuery(const httplib::Request& req)
    {
        if (req.has_param("request_id")) {
            const auto v = req.get_param_value("request_id");
            if (!v.empty()) {
                return v;
            }
        }
        return nextRequestId();
    }

    // 统一错误 JSON（Config.md:491 模板逐字结构）。
    std::string buildErrorJson(const std::string& request_id, ErrorCode code,
                               const std::string& message)
    {
        JsonWriter w;
        w.add("request_id", request_id);
        w.add("ok", false);
        w.beginObject("error");
        w.add("code", ToString(code));
        w.add("message", message);
        w.beginObject("details");
        w.endObject();
        w.endObject();
        return w.finish();
    }

    // 发送 JSON 并落实响应体上限（超限 -> 500 INTERNAL_ERROR）。
    // TODO(ATOM-18 接入后)：超限时还须记录错误日志（Config.md §11.1）。
    void sendJson(httplib::Response& res, int status, std::string body,
                  const std::string& request_id)
    {
        if (static_cast<std::int64_t>(body.size()) > settings.response_body_limit_bytes) {
            status = 500;
            body = buildErrorJson(request_id, ErrorCode::INTERNAL_ERROR,
                                  "response body exceeds configured limit");
        }
        res.status = status;
        res.set_content(std::move(body), "application/json; charset=utf-8");
    }

    void sendError(httplib::Response& res, const std::string& request_id,
                   ErrorCode code, const std::string& message)
    {
        sendJson(res, ToHttpStatus(code), buildErrorJson(request_id, code, message),
                 request_id);
    }

    // handler 分派：在独立线程中执行并以 request_timeout_ms 限时（408）。
    // httplib 的 set_read/write_timeout 是 socket 读写超时，不覆盖 handler
    // 执行时长，故限时在分派层实现。
    template <typename Fn>
    void dispatch(Fn&& fn, httplib::Response& res, const std::string& request_id)
    {
        using Result = std::pair<int, std::string>;
        auto prom = std::make_shared<std::promise<Result>>();
        std::future<Result> fut = prom->get_future();
        std::thread([prom, work = std::forward<Fn>(fn)]() mutable {
            try {
                prom->set_value(work());
            } catch (...) {
                try {
                    prom->set_value(Result{500, {}});
                } catch (...) {
                }
            }
        }).detach();

        if (fut.wait_for(std::chrono::milliseconds(settings.request_timeout_ms)) ==
            std::future_status::ready) {
            Result r = fut.get();
            if (r.second.empty() && r.first >= 500) {
                sendError(res, request_id, ErrorCode::INTERNAL_ERROR,
                          "handler failed unexpectedly");
                return;
            }
            sendJson(res, r.first, std::move(r.second), request_id);
            return;
        }
        sendError(res, request_id, ErrorCode::REQUEST_TIMEOUT,
                  "request handling exceeded configured timeout");
    }

    // GET /api/state（Config.md:473 响应字段逐字）。
    // StateReadView 持锁期间只做内存格式化，不做任何 I/O。
    std::string buildStateJson()
    {
        app::StateReadView view = deps.state_reader->ReadState();
        const state_arbitration::ArbitrationContext& ctx = view.context();
        const state_arbitration::ExternalInputs& in = ctx.inputs;

        const bool initialized = ctx.system_mode != SystemMode::UNINITIALIZED &&
                                 ctx.system_mode != SystemMode::INITIALIZING &&
                                 ctx.system_mode != SystemMode::INIT_FAILED;

        JsonWriter w;
        w.add("system_mode", ToString(ctx.system_mode));
        w.add("initialized", initialized);
        w.add("control_source", ToString(ctx.control_source));
        w.add("auto_fire_state", ToString(ctx.auto_fire_state));
        w.add("current_shot", static_cast<std::uint64_t>(view.current_shot()));
        w.add("total_shots", kTotalShots);
        w.add("stop_fire", in.stop_fire);
        w.add("estop_active", in.software_estop);
        // TODO(PLAN §7.3)：活动故障明细需 ATOM-18 诊断快照接口。
        // 故障源接通前：无故障时给 []，有故障但无法枚举时给 null（不谎报空表）。
        if (in.fault_active) {
            w.addNull("active_faults");
        } else {
            w.addEmptyArray("active_faults");
        }
        w.add("timestamp", static_cast<std::int64_t>(deps.clock->nowMicros()));
        return w.finish();
    }

    void registerRoutes()
    {
        server.set_payload_max_length(
            static_cast<std::size_t>(settings.request_body_limit_bytes));

        // 统一错误出口：handler 已写入统一错误体（ok:false）时不再覆盖；
        // 否则按 HTTP 状态生成（404->NOT_FOUND、413->PAYLOAD_TOO_LARGE 等）。
        server.set_error_handler([this](const httplib::Request& req,
                                        httplib::Response& res) {
            if (res.body.find("\"ok\":false") != std::string::npos) {
                return;
            }
            const std::string rid = requestIdFromQuery(req);
            ErrorCode code = ErrorCode::INTERNAL_ERROR;
            if (res.status == 404) code = ErrorCode::NOT_FOUND;
            else if (res.status == 413) code = ErrorCode::PAYLOAD_TOO_LARGE;
            sendJson(res, res.status, buildErrorJson(rid, code, "request failed"), rid);
        });

        server.set_exception_handler([this](const httplib::Request& req,
                                            httplib::Response& res,
                                            std::exception_ptr) {
            const std::string rid = requestIdFromQuery(req);
            sendError(res, rid, ErrorCode::INTERNAL_ERROR, "unhandled handler exception");
        });

        // --- 只读路由 -------------------------------------------------------
        server.Get("/api/state", [this](const httplib::Request& req,
                                        httplib::Response& res) {
            const std::string rid = requestIdFromQuery(req);
            if (deps.state_reader == nullptr) {
                sendError(res, rid, ErrorCode::DEPENDENCY_UNAVAILABLE,
                          "state reader not connected");
                return;
            }
            dispatch([this]() -> std::pair<int, std::string> {
                return {200, buildStateJson()};
            }, res, rid);
        });

        // --- 桩路由（PLAN §7.3：下游接口未接通，一律 503，不谎报成功） -------
        const auto stub = [this](const httplib::Request& req, httplib::Response& res,
                                 const char* pending) {
            const std::string rid = requestIdFromBody(req);
            sendError(res, rid, ErrorCode::DEPENDENCY_UNAVAILABLE, pending);
        };

        server.Get("/api/diagnostics", [this, stub](const httplib::Request& req,
                                                    httplib::Response& res) {
            const std::string rid = requestIdFromQuery(req);
            sendError(res, rid, ErrorCode::DEPENDENCY_UNAVAILABLE,
                      "diagnostics source (ATOM-18) not connected");
        });

        server.Post("/api/control/auto/start", [this, stub](const httplib::Request& req,
                                                            httplib::Response& res) {
            stub(req, res, "auto start dispatch (ATOM-13) not connected");
        });
        server.Post("/api/control/auto/stop", [this, stub](const httplib::Request& req,
                                                           httplib::Response& res) {
            stub(req, res, "stop-fire dispatch pending availability ruling (PLAN 7.2)");
        });
        server.Post("/api/control/manual", [this, stub](const httplib::Request& req,
                                                        httplib::Response& res) {
            stub(req, res, "manual control dispatch not connected");
        });
        server.Post("/api/control/estop", [this, stub](const httplib::Request& req,
                                                       httplib::Response& res) {
            stub(req, res, "software estop dispatch (ATOM-14) not connected");
        });
        server.Post("/api/control/estop/confirm", [this, stub](const httplib::Request& req,
                                                               httplib::Response& res) {
            stub(req, res, "estop confirm dispatch (ATOM-14) not connected");
        });
        server.Get("/api/parameters", [this, stub](const httplib::Request& req,
                                                   httplib::Response& res) {
            const std::string rid = requestIdFromQuery(req);
            sendError(res, rid, ErrorCode::DEPENDENCY_UNAVAILABLE,
                      "parameter source (ATOM-15) not connected");
        });
        server.Post("/api/parameters", [this, stub](const httplib::Request& req,
                                                    httplib::Response& res) {
            stub(req, res, "parameter update dispatch (ATOM-15) not connected");
        });
        server.Post("/api/calibration", [this, stub](const httplib::Request& req,
                                                     httplib::Response& res) {
            stub(req, res, "calibration dispatch (ATOM-19) not connected");
        });

        // 静态资源（§7.4 暂缓实现）：前端多文件目录落地前，GET / 等路径
        // 由统一错误出口返回 404 NOT_FOUND JSON；静态请求不触发任何机构动作。
    }
};

HttpWebModule::HttpWebModule(Dependencies deps)
    : impl_(std::make_unique<Impl>(deps))
{
}

HttpWebModule::~HttpWebModule()
{
    shutdown();
}

runtime::ModuleInitResult HttpWebModule::initialize(const runtime::RuntimeContext& context)
{
    // RuntimeContext::values 是无类型字符串字典（ILifecycle.hpp 既有形态），
    // 本模块的数值配置一律经 IConfigView 按 ConfigSchema 键名读取。
    (void)context;

    runtime::ModuleInitResult result{};
    result.module_name = name();

    if (impl_->running.load()) {
        result.error_message = "module already started";
        return result;
    }
    if (impl_->deps.config == nullptr) {
        result.error_message = "missing required dependency: IConfigView";
        return result;
    }
    if (impl_->deps.clock == nullptr) {
        result.error_message = "missing required dependency: IMonotonicClock";
        return result;
    }
    if (!impl_->deps.config->isLoaded()) {
        result.error_message = "configuration not loaded";
        return result;
    }

    if (const std::string err = loadSettings(*impl_->deps.config, impl_->settings);
        !err.empty()) {
        result.error_message = "config key error: " + err;
        return result;
    }

    impl_->registerRoutes();

    const int bound = impl_->server.bind_to_port(
        impl_->settings.listen_address, static_cast<int>(impl_->settings.port));
    if (bound <= 0) {
        result.error_message = "failed to bind " + impl_->settings.listen_address +
                               ":" + std::to_string(impl_->settings.port);
        return result;
    }

    impl_->running.store(true);
    impl_->server_thread = std::thread([this]() {
        lowerThreadPriority();
        // listen_after_bind() 阻塞至 stop()；返回值在 shutdown 路径下无意义。
        (void)impl_->server.listen_after_bind();
        impl_->running.store(false);
    });

    result.success = true;
    return result;
}

void HttpWebModule::shutdown() noexcept
{
    try {
        if (impl_->server.is_running()) {
            impl_->server.stop();
        }
        if (impl_->server_thread.joinable()) {
            impl_->server_thread.join();
        }
        impl_->running.store(false);
    } catch (...) {
        // noexcept 契约：关闭路径吞掉一切异常。
    }
}

} // namespace marshland::http_web
