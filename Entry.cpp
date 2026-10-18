// Entry.cpp - Dart Control System 主程序入口
//
// 职责（本工程只做装配，不实现任何原子功能）：
// 1. 创建 RuntimeCoordinator 和 StateMachine
// 2. 用适配层把 ATOM 模块注册进 RuntimeCoordinator（不含 main 以外的业务逻辑）
// 3. 启动所有模块：init 顺序执行，随后并行起线程
// 4. 主控制循环：20 ms 推进控制源选择（ATOM-02）
// 5. 优雅关闭：停线程后反序 cleanup
//
// 已接入的 ATOM 模块（入口均已在各自模块内实现，本文件只调用）：
//   ATOM-01: RuntimeCoordinator（生命周期管理）
//   ATOM-02: StateMachine（控制源选择，主线程 20 ms 更新）
//   ATOM-03: SBUS 遥控输入      —— sbus::ReceiveSbus（装配层起线程）
//   ATOM-04: CAN 电机闭环       —— atom04::CanMotorControl（模块内自建接收线程）
//   ATOM-07: PCA9685A 舵机      —— servo::IServoDriver
//   ATOM-08: 通道 → 执行器控制循环 —— atom08::RunControlLoop（装配层起线程）
//   ATOM-13: 固定四发自动流程    —— auto_fire::RunAutoFire（当前占位实现）
//   ATOM-15: 配置与持久化        —— atom15::ConfigStore
//   ATOM-16: HTTP / Web 前端     —— web::RunHttpServer（装配层起线程）
//   ATOM-18: 日志与诊断          —— log::Init / log::Log
//
// 未接入（本仓库没有对应实现目录）：ATOM-05/06/09/10/11/12/14/17/19。
//
// 运行时参数来源：端点参数全部由各模块在 init 阶段经 ATOM-15 的 getInt/getString
// 从 ./Config/dart_control.json 读出并校验；任一键缺失、类型不符或越界即启动失败，
// 不使用兜底值、不做隐式钳位。本文件只保留两个配置里不存在的常量：配置文件自身
// 的路径（引导用）与 Web 前端目录（http.static_resource_provider=MAIN_PROGRAM，
// 前端由主程序提供，配置里只有声明没有路径）。

#include "Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp"
#include "Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp"
#include "Atoms/Public/Auto.hpp"
#include "Atoms/Public/Log.hpp"
#include "Atoms/Public/Motor.hpp"
#include "Atoms/Public/Sbus.hpp"
#include "Atoms/Public/Servo.hpp"
#include "Atoms/Public/Web.hpp"
#include "AutoFire.hpp"
#include "ProcessExitSignals.hpp"
#include "atom04_can/can_motor_control.hpp"
#include "atom08_control/control_loop.hpp"
#include "atom15_config/config_store.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace marshland;
using namespace std::chrono_literals;

// =============================================================================
// 引导常量（配置里没有这个键，故只能放在装配层）
// =============================================================================
namespace cfg {
inline constexpr const char* kConfigPath = "./Config/dart_control.json";     // persistence.directory/file_name
} // namespace cfg

// =============================================================================
// 运行时参数（各模块 init 经 ATOM-15 读出后填充，零默认值）
// =============================================================================
struct Endpoints final {
    // ATOM-03
    std::string sbus_serial_device;
    std::uint8_t sbus_channel_scale_x{0};
    std::uint16_t sbus_frame_bytes{0};
    std::uint16_t sbus_channel_center{0};
    std::uint16_t sbus_channel_minimum{0};
    std::uint16_t sbus_channel_maximum{0};
    std::uint32_t sbus_read_timeout_ms{0};
    // ATOM-04
    std::string can_interface;
    // ATOM-07
    std::string servo_i2c_device;
    std::uint8_t servo_i2c_address{0};
    // ATOM-16
    std::string http_listen_address;
    std::int32_t http_port{0};
    std::size_t http_request_body_limit_bytes{0};
    std::size_t http_response_body_limit_bytes{0};
    bool http_has_static_root{false};
    // ATOM-08
    std::uint32_t control_loop_period_ms{0};
};

// =============================================================================
// 全局对象（寿命必须长于所有线程和适配层）
// =============================================================================

// 生产者和状态机必须使用同一份共享数据。
static sbus::SbusData g_sbus_data;
static web::WebData g_web_data;
static state::StateMachine g_state_machine{g_sbus_data, g_web_data};

// ATOM 模块实例：全部在静态存储期创建，由 RuntimeCoordinator 负责 init/cleanup。
static atom15::ConfigStore g_config;                       // ATOM-15，无独立线程
static motor::MotorPidParams g_motor_gains[can::kMaxMotorNodes]{};  // ATOM-15 读出，ATOM-04 使用
static atom04::CanMotorControl g_motors;                   // ATOM-04，模块内自建接收线程
static std::unique_ptr<servo::IServoDriver> g_servo;       // ATOM-07，ATOM-08 写入角度
static auto_fire::AutoData g_auto_data;                    // ATOM-13，ATOM-08 的 AUTO 数据源
static Endpoints g_endpoints;                              // 配置读出的运行时参数

// =============================================================================
// 装配层：每个 ATOM 的 init / run / cleanup 适配
// =============================================================================
namespace assembly {

// 桥接函数：RuntimeCoordinator 向下传 running（true = 运行），而 ATOM 的阻塞入口
// 要求 stop_requested（true = 停止），两者语义相反。
//   * 直接把 running 转发给模块，阻塞入口会在第一步就返回；
//   * 而 shutdown 时必须先让模块的 stop 置位再 join，否则 join 永久卡住。
// 所以这里用模块自有的 stop 标志驱动阻塞入口，装配层线程只负责观察 running，
// 并在退出前保证“先置 stop、再 join”。
template <typename BlockingEntry>
void RunBridged(std::atomic<bool>& running, const char* module_name, BlockingEntry entry)
{
    std::atomic<bool> stop_requested{false};
    std::thread worker([&entry, &stop_requested] { entry(stop_requested); });

    while (running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(20ms);
    }

    stop_requested.store(true, std::memory_order_release);
    worker.join();
    std::cout << "[Entry] " << module_name << " worker joined" << std::endl;
}

// ---- 配置读取助手 ----------------------------------------------------------
// 非 OK、或越出调用方给出的物理范围，一律记录 ERROR 并让该模块 init 失败；
// 不使用兜底值、不做隐式钳位（配置本身已由 ATOM-15 在 load() 时做过 schema 校验）。
const char* ConfigStatusText(atom15::ConfigStatus status) noexcept
{
    switch (status) {
    case atom15::ConfigStatus::OK:           return "OK";
    case atom15::ConfigStatus::NOT_LOADED:   return "NOT_LOADED";
    case atom15::ConfigStatus::MISSING:      return "MISSING";
    case atom15::ConfigStatus::INVALID_TYPE: return "INVALID_TYPE";
    }
    return "UNKNOWN";
}

bool ReadConfigString(const char* key, std::string& out)
{
    const auto status = g_config.getString(key, out);
    if (status != atom15::ConfigStatus::OK) {
        log::Log(log::Level::ERROR,
                 std::string("ATOM-15 config ") + key + ": " + ConfigStatusText(status));
        return false;
    }
    return true;
}

bool ReadConfigInt(const char* key, std::int64_t minimum, std::int64_t maximum, std::int64_t& out)
{
    const auto status = g_config.getInt(key, out);
    if (status != atom15::ConfigStatus::OK) {
        log::Log(log::Level::ERROR,
                 std::string("ATOM-15 config ") + key + ": " + ConfigStatusText(status));
        return false;
    }
    if (out < minimum || out > maximum) {
        log::Log(log::Level::ERROR, std::string("ATOM-15 config ") + key + "=" +
                                        std::to_string(out) + " out of range [" +
                                        std::to_string(minimum) + ", " + std::to_string(maximum) +
                                        "]");
        return false;
    }
    return true;
}

// --- ATOM-18: 日志与诊断（只需要初始化，无独立线程）---
namespace atom18 {
bool init()
{
    log::Init(log::Level::INFO);
    log::Log(log::Level::INFO, "ATOM-18 logging initialized");
    return true;
}

void cleanup()
{
    log::Log(log::Level::INFO, "ATOM-18 logging stopped");
}
} // namespace atom18

// --- ATOM-15: 配置与持久化（只需要初始化，无独立线程）---
namespace atom15 {
// 从配置读出 6 组 PID（motor[0..5]，节点 0x201~0x206 顺序）并换算成 ATOM-04 契约。
bool loadMotorGains()
{
    for (std::size_t index = 0; index < can::kMaxMotorNodes; ++index) {
        const auto motor_id = static_cast<std::int64_t>(index) + 1;
        const std::string suffix = " (motor " + std::to_string(motor_id) + ")";
        const auto read = g_config.readPid(motor_id);
        if (!read.success) {
            log::Log(log::Level::ERROR, "ATOM-15 PID read failed" + suffix + ": " + read.message);
            return false;
        }

        // Config 只给对称的 output_limit；契约要求 max > min 的电流上下限，故取 ±limit。
        const double limit = static_cast<double>(read.value.output_limit);
        g_motor_gains[index] = motor::MotorPidParams{static_cast<double>(read.value.kp),
                                                     static_cast<double>(read.value.ki),
                                                     static_cast<double>(read.value.kd), limit,
                                                     -limit};
        if (!read.value.enabled) {
            log::Log(log::Level::WARN, "ATOM-15 PID disabled in config" + suffix);
        }
    }
    return true;
}

bool init()
{
    const auto result = g_config.load(cfg::kConfigPath);
    if (!result.success) {
        for (const auto& diagnostic : result.diagnostics) {
            log::Log(log::Level::ERROR,
                     "ATOM-15 config: " + diagnostic.key + " " + diagnostic.message);
        }
        log::Log(log::Level::ERROR,
                 std::string("ATOM-15 config load failed: ") + cfg::kConfigPath + " (error " +
                     std::to_string(static_cast<int>(result.error)) + ")");
        return false;
    }
    if (!loadMotorGains()) {
        return false;
    }
    log::Log(log::Level::INFO, std::string("ATOM-15 config loaded: ") + cfg::kConfigPath +
                                   " version " + std::to_string(g_config.configVersion()));
    return true;
}

void cleanup()
{
    // 配置文件只在本模块的 updateRuntime/writePid 中持久化；关闭时没有待保存状态。
}
} // namespace atom15

// --- ATOM-03: SBUS 遥控输入（阻塞接收，需要装配层线程）---
namespace atom03 {
bool init()
{
    std::int64_t scale = 0;
    std::int64_t frame_bytes = 0;
    std::int64_t center = 0;
    std::int64_t minimum = 0;
    std::int64_t maximum = 0;
    std::int64_t read_timeout = 0;

    if (!ReadConfigString("sbus.serial_device", g_endpoints.sbus_serial_device)) return false;
    if (!ReadConfigInt("sbus.channel_scale_x", 1, 10, scale)) return false;
    if (!ReadConfigInt("sbus.frame_bytes", 1, 1024, frame_bytes)) return false;
    if (!ReadConfigInt("sbus.joystick_center", 0, 65535, center)) return false;
    if (!ReadConfigInt("sbus.valid_min", 0, 65535, minimum)) return false;
    if (!ReadConfigInt("sbus.valid_max", 0, 65535, maximum)) return false;
    if (!ReadConfigInt("timing.sbus_receive_period_ms", 1, 1000, read_timeout)) return false;

    // 这里只做类型落位；帧长下限、minimum<center<maximum 等语义约束由 ATOM-03 自己判定。
    g_endpoints.sbus_channel_scale_x = static_cast<std::uint8_t>(scale);
    g_endpoints.sbus_frame_bytes = static_cast<std::uint16_t>(frame_bytes);
    g_endpoints.sbus_channel_center = static_cast<std::uint16_t>(center);
    g_endpoints.sbus_channel_minimum = static_cast<std::uint16_t>(minimum);
    g_endpoints.sbus_channel_maximum = static_cast<std::uint16_t>(maximum);
    g_endpoints.sbus_read_timeout_ms = static_cast<std::uint32_t>(read_timeout);

    log::Log(log::Level::INFO, "ATOM-03 SBUS target: " + g_endpoints.sbus_serial_device);
    return true;
}

void run(std::atomic<bool>& running)
{
    RunBridged(running, "ATOM-03 SBUS", [](const std::atomic<bool>& stop_requested) {
        const bool stopped_cleanly =
            sbus::ReceiveSbus(g_endpoints.sbus_serial_device.c_str(), g_sbus_data, stop_requested,
                              g_endpoints.sbus_channel_scale_x, g_endpoints.sbus_frame_bytes,
                              g_endpoints.sbus_channel_center, g_endpoints.sbus_channel_minimum,
                              g_endpoints.sbus_channel_maximum, g_endpoints.sbus_read_timeout_ms);
        if (!stopped_cleanly) {
            log::Log(log::Level::ERROR, "ATOM-03 SBUS receiver exited on device error");
        }
    });
}

void cleanup()
{
    // 线程已由 RuntimeCoordinator 停止；串口在 ReceiveSbus 返回时自行关闭。
}
} // namespace atom03

// --- ATOM-07: 舵机驱动（只需要初始化，无独立线程）---
namespace atom07 {
// servo.pca9685.address 在配置里是字符串（如 "0x40"），按 7 位 IIC 地址解析。
bool readI2cAddress()
{
    std::string address_text;
    if (!ReadConfigString("servo.pca9685.address", address_text)) {
        return false;
    }
    char* end = nullptr;
    const long address = std::strtol(address_text.c_str(), &end, 0);
    if (end == address_text.c_str() || *end != '\0' || address < 0 || address > 0x7F) {
        log::Log(log::Level::ERROR,
                 "ATOM-07 servo.pca9685.address is not a 7-bit address: " + address_text);
        return false;
    }
    g_endpoints.servo_i2c_address = static_cast<std::uint8_t>(address);
    return true;
}

bool init()
{
    if (!ReadConfigString("servo.pca9685.i2c_device", g_endpoints.servo_i2c_device)) return false;
    if (!readI2cAddress()) return false;

    g_servo = servo::MakePca9685ServoDriver();
    if (!g_servo) {
        log::Log(log::Level::ERROR, "ATOM-07 servo driver allocation failed");
        return false;
    }
    if (!g_servo->open(g_endpoints.servo_i2c_device.c_str(), g_endpoints.servo_i2c_address)) {
        const char* error = g_servo->lastError();
        log::Log(log::Level::ERROR, std::string("ATOM-07 servo open failed: ") +
                                        g_endpoints.servo_i2c_device + " addr " +
                                        std::to_string(g_endpoints.servo_i2c_address) +
                                        (error != nullptr ? std::string(": ") + error
                                                          : std::string{}));
        g_servo.reset();
        return false;
    }
    log::Log(log::Level::INFO, "ATOM-07 servo ready: " + g_endpoints.servo_i2c_device);
    return true;
}

void cleanup()
{
    if (g_servo) {
        g_servo->close();
        g_servo.reset();
    }
}
} // namespace atom07

// --- ATOM-04: CAN 电机闭环（模块内自建接收/驱动线程，无装配层线程）---
namespace atom04 {
// can.interface 在配置里写作 "CAN0"（设计用的大写总线名），而 SocketCAN 的
// SIOCGIFINDEX 只认内核接口名（can0，大小写敏感）。这里只做大小写归一，并明确
// 记录归一前后，避免 ATOM-04 在启动时以 SIOCGIFINDEX 失败。
bool normalizeInterface()
{
    if (!ReadConfigString("can.interface", g_endpoints.can_interface)) return false;

    std::string lowered = g_endpoints.can_interface;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered != g_endpoints.can_interface) {
        log::Log(log::Level::WARN, "ATOM-04 can.interface '" + g_endpoints.can_interface +
                                       "' normalized to kernel name '" + lowered + "'");
        g_endpoints.can_interface = lowered;
    }
    return true;
}

bool init()
{
    if (!normalizeInterface()) return false;

    const auto result = g_motors.start(g_endpoints.can_interface.c_str(), g_motor_gains);
    if (result.failed) {
        log::Log(log::Level::ERROR, std::string("ATOM-04 CAN start failed: ") +
                                        can::toString(result.error) + " errno=" +
                                        std::to_string(result.system_errno));
        return false;
    }
    log::Log(log::Level::INFO, "ATOM-04 CAN ready: " + g_endpoints.can_interface);
    return true;
}

void cleanup()
{
    // ATOM-04 不提供安全网：stop() 不发送任何 CAN 帧，最后写入的电流会留在电调里。
    // 装配层在停机前先提交全零转速目标，给 PID 两个驱动周期收敛后再停止接收线程，
    // 避免电机保持最后一次目标转速。这不是急停/安全流程（属 ATOM-14）。
    for (std::uint32_t offset = 0; offset < can::kMaxMotorNodes; ++offset) {
        (void)g_motors.setRpm(can::kMotorNodeMin + offset, 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2 * motor::kDrivePeriodMs));
    g_motors.stop();
}
} // namespace atom04

// --- ATOM-13: 固定四发自动流程（阻塞入口，需要装配层线程）---
namespace atom13 {
bool init()
{
    return true;
}

void run(std::atomic<bool>& running)
{
    RunBridged(running, "ATOM-13 AutoFire", [](const std::atomic<bool>& stop_requested) {
        (void)auto_fire::RunAutoFire(g_auto_data, stop_requested);
    });
}

void cleanup()
{
}
} // namespace atom13

// --- ATOM-16: HTTP 服务（阻塞入口，需要装配层线程）---
namespace atom16 {
// 前端静态资源目录（相对运行目录）。部署时把 Atoms/ATOM-16_HTTP_Web/Web 放到可执行文件旁边。
// 配置里只有 http.static_resource_provider = "MAIN_PROGRAM"（声明由主程序提供），没有路径键。
inline constexpr const char* kStaticRoot = "./Web";

bool init()
{
    std::int64_t port = 0;
    std::int64_t request_limit = 0;
    std::int64_t response_limit = 0;

    if (!ReadConfigString("http.listen_address", g_endpoints.http_listen_address)) return false;
    if (!ReadConfigInt("http.port", 1, 65535, port)) return false;
    if (!ReadConfigInt("http.request_body_limit_bytes", 1, 1 << 30, request_limit)) return false;
    if (!ReadConfigInt("http.response_body_limit_bytes", 1, 1 << 30, response_limit)) return false;

    g_endpoints.http_port = static_cast<std::int32_t>(port);
    g_endpoints.http_request_body_limit_bytes = static_cast<std::size_t>(request_limit);
    g_endpoints.http_response_body_limit_bytes = static_cast<std::size_t>(response_limit);

    // 静态目录由 RunHttpServer 在 setup() 里挂载：挂载失败会抛异常并让 HTTP 线程直接退出。
    // 所以先在这里判存在性，缺目录时只关闭静态服务，JSON API 照常可用。
    std::error_code filesystem_error;
    g_endpoints.http_has_static_root = std::filesystem::is_directory(kStaticRoot, filesystem_error);
    if (!g_endpoints.http_has_static_root) {
        log::Log(log::Level::WARN,
                 std::string("ATOM-16 static root not found: ") +
                     std::filesystem::absolute(kStaticRoot, filesystem_error).string() +
                     " (JSON API only)");
    }
    return true;
}

void run(std::atomic<bool>& running)
{
    web::HttpServerConfig config;
    config.listen_address = g_endpoints.http_listen_address;
    config.port = g_endpoints.http_port;
    config.request_body_limit_bytes = g_endpoints.http_request_body_limit_bytes;
    config.response_body_limit_bytes = g_endpoints.http_response_body_limit_bytes;
    config.static_root = g_endpoints.http_has_static_root ? kStaticRoot : nullptr;

    RunBridged(running, "ATOM-16 HTTP", [config](const std::atomic<bool>& stop_requested) {
        const int result = web::RunHttpServer(config, g_web_data, &g_motors, stop_requested);
        if (result != 0) {
            log::Log(log::Level::ERROR,
                     "ATOM-16 HTTP server exited with " + std::to_string(result) +
                         " (1 = bind failed or static root rejected)");
        }
    });
}

void cleanup()
{
    // 监听线程由 RunHttpServer 在停止标志置位后自行退出并释放。
}
} // namespace atom16

// --- ATOM-08: 控制源通道 → 电机/舵机（阻塞入口，需要装配层线程）---
namespace atom08 {
bool init()
{
    if (!g_servo) {
        log::Log(log::Level::ERROR, "ATOM-08 requires ATOM-07 servo driver");
        return false;
    }

    std::int64_t period_ms = 0;
    if (!ReadConfigInt("timing.motor_loop_period_ms", 1, 1000, period_ms)) return false;
    g_endpoints.control_loop_period_ms = static_cast<std::uint32_t>(period_ms);
    return true;
}

void run(std::atomic<bool>& running)
{
    RunBridged(running, "ATOM-08 Control", [](const std::atomic<bool>& stop_requested) {
        marshland::atom08::RunControlLoop(g_sbus_data, g_web_data, g_auto_data, g_motors,
                                          *g_servo, g_endpoints.control_loop_period_ms,
                                          stop_requested);
    });
}

void cleanup()
{
}
} // namespace atom08

} // namespace assembly

// =============================================================================
// 主程序
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Dart Control System" << std::endl;
    std::cout << "  Cocoon-Dart2027 (Marshland)" << std::endl;
    std::cout << "========================================" << std::endl;

    // 在创建工作线程前阻塞退出信号，统一由主线程等待和回收。
    app::ProcessExitSignals exit_signals;
    if (const int error = exit_signals.Prepare(); error != 0) {
        std::cerr << "[Main] Failed to prepare exit signals: " << error << std::endl;
        return 1;
    }

    // -------------------------------------------------------------------------
    // 阶段 1：创建 RuntimeCoordinator
    // -------------------------------------------------------------------------
    runtime::RuntimeCoordinator runtime;
    state::setGlobalStateMachine(&g_state_machine);

    // -------------------------------------------------------------------------
    // 阶段 2：注册所有 ATOM 模块（按依赖顺序）
    // -------------------------------------------------------------------------
    std::cout << "\n[Main] Registering modules..." << std::endl;

    // 基础设施模块（无独立线程）：日志 → 配置 → 舵机 → CAN，失败即整体启动失败。
    runtime.registerModule("ATOM-18 Log", assembly::atom18::init, assembly::atom18::cleanup);
    runtime.registerModule("ATOM-15 Config", assembly::atom15::init, assembly::atom15::cleanup);
    runtime.registerModule("ATOM-07 Servo", assembly::atom07::init, assembly::atom07::cleanup);
    runtime.registerModule("ATOM-04 CAN", assembly::atom04::init, assembly::atom04::cleanup);

    // 数据源与控制循环（装配层起线程）：SBUS / 自动流程 / HTTP 先起，控制循环最后起。
    runtime.registerModule("ATOM-03 SBUS", assembly::atom03::init, assembly::atom03::run,
                           assembly::atom03::cleanup);
    runtime.registerModule("ATOM-13 AutoFire", assembly::atom13::init, assembly::atom13::run,
                           assembly::atom13::cleanup);
    runtime.registerModule("ATOM-16 HTTP", assembly::atom16::init, assembly::atom16::run,
                           assembly::atom16::cleanup);
    runtime.registerModule("ATOM-08 Control", assembly::atom08::init, assembly::atom08::run,
                           assembly::atom08::cleanup);

    // -------------------------------------------------------------------------
    // 阶段 3：启动所有模块
    // -------------------------------------------------------------------------
    std::cout << "\n[Main] Starting all modules..." << std::endl;

    if (!runtime.start()) {
        std::cerr << "\n[Main] ❌ Failed to start runtime:" << std::endl;
        for (const auto& err : runtime.errors()) {
            std::cerr << "  - " << err.step_name << ": " << err.message << std::endl;
        }

        state::setGlobalStateMachine(nullptr);
        return 1;
    }

    std::cout << "\n[Main] ✅ System started successfully" << std::endl;
    std::cout << "[Main] Control source: " << state::toString(g_state_machine.control_source()) << std::endl;
    std::cout << "[Main] Press Ctrl+C to exit" << std::endl;

    // -------------------------------------------------------------------------
    // 阶段 4：主控制循环（在主线程执行）
    // -------------------------------------------------------------------------
    constexpr auto kControlPeriod = 20ms;
    auto next_update = std::chrono::steady_clock::now();
    int exit_signal = 0;
    int exit_code = 0;
    while (exit_signal == 0) {
        g_state_machine.updateControlSource();
        next_update += kControlPeriod;

        // 更新超时时跳过已错过的周期，不忙循环追赶。
        const auto now = std::chrono::steady_clock::now();
        if (next_update <= now) {
            next_update = now + kControlPeriod;
        }
        const int error = exit_signals.WaitFor(exit_signal, next_update - now);
        if (error != 0 && error != EAGAIN && error != EINTR) {
            std::cerr << "[Main] Failed to wait for exit signal: " << error << std::endl;
            exit_code = 1;
            break;
        }
    }
    if (exit_signal != 0) {
        std::cout << "[Main] Received signal " << exit_signal << std::endl;
    }

    // -------------------------------------------------------------------------
    // 阶段 5：优雅关闭
    // -------------------------------------------------------------------------
    std::cout << "\n[Main] Shutting down gracefully..." << std::endl;

    // 停止所有模块（自动停止所有线程并反序清理）
    runtime.shutdown();
    state::setGlobalStateMachine(nullptr);

    std::cout << "[Main] Exit" << std::endl;
    return exit_code;
}

// =============================================================================
// 附录：集成其他 ATOM 模块的模板
// =============================================================================

/*

模板 A：需要独立线程的模块
--------------------------------

// ATOM-XX/Inc/Module.hpp
#pragma once
#include <atomic>

namespace atom_xx {
    bool init();
    void run(std::atomic<bool>& running);
    void cleanup();
}

// ATOM-XX/Src/Module.cpp
#include "Module.hpp"

namespace atom_xx {
    static MyModule module;

    bool init() {
        std::cout << "[ATOM-XX] Initializing..." << std::endl;
        return module.initialize();
    }

    void run(std::atomic<bool>& running) {
        std::cout << "[ATOM-XX] Thread started" << std::endl;

        while (running) {
            module.doWork();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        std::cout << "[ATOM-XX] Thread exiting" << std::endl;
    }

    void cleanup() {
        std::cout << "[ATOM-XX] Cleaning up..." << std::endl;
        module.shutdown();
    }
}

// Entry.cpp 中注册
runtime.registerModule("ATOM-XX", atom_xx::init, atom_xx::run, atom_xx::cleanup);

注意：模块入口若是“阻塞到 stop_requested 置位”的形式（ReceiveSbus、
RunControlLoop、RunAutoFire、RunHttpServer 都是），必须经 assembly::RunBridged
桥接，不能直接把 running 当 stop_requested 传进去（语义相反）。


模板 B：只需要初始化的模块
--------------------------------

// ATOM-XX/Inc/Module.hpp
#pragma once

namespace atom_xx {
    bool init();
    void cleanup();
}

// ATOM-XX/Src/Module.cpp
#include "Module.hpp"

namespace atom_xx {
    static MyModule module;

    bool init() {
        std::cout << "[ATOM-XX] Initializing..." << std::endl;
        return module.initialize();
    }

    void cleanup() {
        std::cout << "[ATOM-XX] Cleaning up..." << std::endl;
        module.shutdown();
    }
}

// Entry.cpp 中注册
runtime.registerModule("ATOM-XX", atom_xx::init, atom_xx::cleanup);


配置取数（ATOM-15 已加载后）
--------------------------------

// 各模块 init 内按 Rule/ConfigSchema.md 的键名取值；非 OK 即让 init 失败：
std::int64_t value = 0;
if (g_config.getInt("timing.motor_loop_period_ms", value) != atom15::ConfigStatus::OK) {
    return false;
}
// 不允许用默认值兜底，也不允许对越界参数做隐式钳位。


使用第三方库自带线程的模块（如 cpp-httplib）
--------------------------------------------------

namespace atom_http {
    httplib::Server server;

    bool init() {
        // 注册路由
        server.Get("/status", [](const Request&, Response& res) {
            res.set_content("OK", "text/plain");
        });
        return true;
    }

    void run(std::atomic<bool>& running) {
        // cpp-httplib 的 listen() 是阻塞的，需要在独立线程中运行
        // 它会一直运行直到 server.stop() 被调用
        server.listen("0.0.0.0", 8080);
    }

    void cleanup() {
        server.stop();
    }
}

runtime.registerModule("HTTP", atom_http::init, atom_http::run, atom_http::cleanup);

*/
