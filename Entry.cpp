// Entry.cpp - Dart Control System 主程序入口
//
// 职责：
// 1. 创建 RuntimeCoordinator 和 StateMachine
// 2. 注册所有 ATOM 模块
// 3. 启动所有模块（自动启动线程）
// 4. 主控制循环
// 5. 优雅关闭
//
// 使用的 ATOM 模块：
//   ATOM-01: RuntimeCoordinator（生命周期管理）
//   ATOM-02: StateMachine（状态机与控制权仲裁）
//   ATOM-03: SBUS（遥控器输入）
//   ATOM-04: CAN（电机通信）
//   ATOM-05: Motor PID（电机 PID 控制）
//   ATOM-07: Servo（舵机驱动）
//   ATOM-08~14: 机构控制模块
//   ATOM-15: Config（配置管理）
//   ATOM-16: HTTP（Web 接口）
//   ATOM-17: WebSocket（状态推送）
//   ATOM-18: Diagnostics（日志与诊断）

#include "Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp"
#include "Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp"
#include "ProcessExitSignals.hpp"

#include <cerrno>
#include <iostream>
#include <thread>
#include <chrono>

using namespace marshland;
using namespace std::chrono_literals;

// =============================================================================
// 全局对象
// =============================================================================

// 生产者和状态机必须使用同一份共享数据。
static sbus::SbusData g_sbus_data;
static web::WebData g_web_data;
static state::StateMachine g_state_machine{g_sbus_data, g_web_data};

// =============================================================================
// ATOM 模块适配层（示例）
// =============================================================================

// --- ATOM-15: 配置管理（只需要初始化，无独立线程）---
namespace atom15 {
    // TODO: 引入真正的 ConfigStore
    // #include "Atoms/ATOM-15_Config_Persistence/Inc/ConfigStore.hpp"
    // static ConfigStore config;

    bool init() {
        std::cout << "[ATOM-15] Loading config from dart_control.json..." << std::endl;
        // TODO: return config.load("dart_control.json");
        return true;  // 临时：假设成功
    }

    void cleanup() {
        std::cout << "[ATOM-15] Saving config..." << std::endl;
        // TODO: config.save();
    }
}

// --- ATOM-18: 日志与诊断（只需要初始化，无独立线程）---
namespace atom18 {
    // TODO: 引入真正的 Diagnostics
    // #include "Atoms/ATOM-18_Diagnostics_Logging/Inc/Logger.hpp"
    // static Logger logger;

    bool init() {
        std::cout << "[ATOM-18] Initializing logger..." << std::endl;
        // TODO: return logger.initialize("dart_control.log");
        return true;
    }

    void cleanup() {
        std::cout << "[ATOM-18] Flushing logs..." << std::endl;
        // TODO: logger.flush();
    }
}

// --- ATOM-03: SBUS 遥控器（需要独立线程读取数据）---
namespace atom03 {
    // TODO: 引入真正的 SBUS 驱动
    // #include "Atoms/ATOM-03_SBUS/Inc/SbusReceiver.hpp"
    // static SbusReceiver sbus;

    bool init() {
        std::cout << "[ATOM-03] Opening SBUS port /dev/ttyS1..." << std::endl;
        // TODO: return sbus.open("/dev/ttyS1");
        return true;
    }

    void run(std::atomic<bool>& running) {
        std::cout << "[ATOM-03] SBUS thread started" << std::endl;

        while (running) {
            // TODO: 接入 ReceiveSbus，发布到 g_sbus_data；不调用状态机更新接口。

            std::this_thread::sleep_for(10ms);
        }

        std::cout << "[ATOM-03] SBUS thread exiting" << std::endl;
    }

    void cleanup() {
        std::cout << "[ATOM-03] Closing SBUS port" << std::endl;
        // TODO: sbus.close();
    }
}

// --- ATOM-04: CAN 总线（需要独立线程收发数据）---
namespace atom04 {
    // TODO: 引入真正的 CAN 驱动
    // #include "Atoms/ATOM-04_CAN/Inc/CanBus.hpp"
    // static CanBus can;

    bool init() {
        std::cout << "[ATOM-04] Opening CAN interface can0..." << std::endl;
        // TODO: return can.open("can0");
        return true;
    }

    void run(std::atomic<bool>& running) {
        std::cout << "[ATOM-04] CAN thread started" << std::endl;

        while (running) {
            // TODO: 处理 CAN 消息
            // can.processMessages();

            std::this_thread::sleep_for(1ms);
        }

        std::cout << "[ATOM-04] CAN thread exiting" << std::endl;
    }

    void cleanup() {
        std::cout << "[ATOM-04] Closing CAN interface" << std::endl;
        // TODO: can.close();
    }
}

// --- ATOM-05: 电机 PID 控制（只需要初始化，无独立线程）---
namespace atom05 {
    // TODO: 引入真正的 PID 控制器
    // #include "Atoms/ATOM-05_Motor_PID/Inc/PidController.hpp"

    bool init() {
        std::cout << "[ATOM-05] Loading motor PID parameters..." << std::endl;
        // TODO: return pid.loadParameters();
        return true;
    }

    void cleanup() {
        std::cout << "[ATOM-05] Cleaning up PID controllers" << std::endl;
    }
}

// --- ATOM-07: 舵机驱动（只需要初始化，无独立线程）---
namespace atom07 {
    // TODO: 引入真正的舵机驱动
    // #include "Atoms/ATOM-07_Servo_Driver/Inc/ServoDriver.hpp"

    bool init() {
        std::cout << "[ATOM-07] Initializing servo driver (PCA9685A)..." << std::endl;
        // TODO: return servo.initialize();
        return true;
    }

    void cleanup() {
        std::cout << "[ATOM-07] Disabling all servos" << std::endl;
        // TODO: servo.disableAll();
    }
}

// --- ATOM-16: HTTP 服务（需要独立线程监听请求）---
namespace atom16 {
    // TODO: 引入真正的 HTTP 服务
    // #include "Atoms/ATOM-16_HTTP_Web/Inc/HttpServer.hpp"
    // static HttpServer http;

    bool init() {
        std::cout << "[ATOM-16] Starting HTTP server on port 8080..." << std::endl;
        // TODO: 注册路由
        // http.Get("/status", [](const Request&, Response& res) {
        //     res.set_content(state::toString(g_state_machine.control_source()), "text/plain");
        // });
        // return http.start("0.0.0.0", 8080);
        return true;
    }

    void run(std::atomic<bool>& running) {
        std::cout << "[ATOM-16] HTTP thread started" << std::endl;

        // TODO: 接入 RunHttpServer，发布到 g_web_data；状态机由主线程读取。
        // cpp-httplib 内部有自己的循环
        // http.listen();  // 阻塞直到 http.stop() 被调用

        while (running) {
            std::this_thread::sleep_for(100ms);
        }

        std::cout << "[ATOM-16] HTTP thread exiting" << std::endl;
    }

    void cleanup() {
        std::cout << "[ATOM-16] Stopping HTTP server" << std::endl;
        // TODO: http.stop();
    }
}

// --- ATOM-17: WebSocket 服务（需要独立线程推送状态）---
namespace atom17 {
    // TODO: 引入真正的 WebSocket 服务
    // #include "Atoms/ATOM-17_WebSocket/Inc/WebSocketServer.hpp"
    // static WebSocketServer ws;

    bool init() {
        std::cout << "[ATOM-17] Starting WebSocket server on port 8081..." << std::endl;
        // TODO: return ws.start(8081);
        return true;
    }

    void run(std::atomic<bool>& running) {
        std::cout << "[ATOM-17] WebSocket thread started" << std::endl;

        while (running) {
            // TODO: 定期推送状态更新
            // ws.broadcast(g_state_machine.control_source());

            std::this_thread::sleep_for(100ms);
        }

        std::cout << "[ATOM-17] WebSocket thread exiting" << std::endl;
    }

    void cleanup() {
        std::cout << "[ATOM-17] Stopping WebSocket server" << std::endl;
        // TODO: ws.stop();
    }
}

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

    // 基础设施模块（无独立线程）
    runtime.registerModule("ATOM-18 Diagnostics", atom18::init, atom18::cleanup);
    runtime.registerModule("ATOM-15 Config", atom15::init, atom15::cleanup);
    runtime.registerModule("ATOM-05 Motor PID", atom05::init, atom05::cleanup);
    runtime.registerModule("ATOM-07 Servo", atom07::init, atom07::cleanup);

    // 通信模块（需要独立线程）
    runtime.registerModule("ATOM-03 SBUS", atom03::init, atom03::run, atom03::cleanup);
    runtime.registerModule("ATOM-04 CAN", atom04::init, atom04::run, atom04::cleanup);

    // 网络服务模块（需要独立线程）
    runtime.registerModule("ATOM-16 HTTP", atom16::init, atom16::run, atom16::cleanup);
    runtime.registerModule("ATOM-17 WebSocket", atom17::init, atom17::run, atom17::cleanup);

    // TODO: 注册其他 ATOM 模块
    // runtime.registerModule("ATOM-08 Charge", atom08::init, atom08::cleanup);
    // runtime.registerModule("ATOM-09 Force", atom09::init, atom09::cleanup);
    // runtime.registerModule("ATOM-10 Shot", atom10::init, atom10::cleanup);
    // runtime.registerModule("ATOM-11 Fire", atom11::init, atom11::cleanup);
    // runtime.registerModule("ATOM-12 Reload Lift", atom12::init, atom12::cleanup);
    // runtime.registerModule("ATOM-13 Feed", atom13::init, atom13::cleanup);
    // runtime.registerModule("ATOM-14 Reload Lower", atom14::init, atom14::cleanup);

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
