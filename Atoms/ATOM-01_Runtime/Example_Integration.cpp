// 示例：如何使用新的 RuntimeCoordinator 集成其他 ATOM
//
// 每个 ATOM 模块只需要提供三个函数：
//   1. init()    - 初始化（在主线程顺序执行）
//   2. run()     - 运行循环（在独立线程执行，可选）
//   3. cleanup() - 清理（在主线程反序执行）

#include "RuntimeCoordinator.hpp"
#include "../ATOM-02_State_Arbitration/Inc/StateArbitration.hpp"
#include <iostream>
#include <chrono>

using namespace marshland;
using namespace std::chrono_literals;

// =============================================================================
// 示例 ATOM 模块
// =============================================================================

// --- ATOM-03: SBUS 模块（需要独立线程读取数据）---
namespace atom03 {
    // 假设的 SBUS 类
    struct SbusReceiver {
        bool open(const char* port) {
            std::cout << "[SBUS] Opening port: " << port << std::endl;
            return true;
        }

        void close() {
            std::cout << "[SBUS] Closing port" << std::endl;
        }

        bool readFrame(/* frame data */) {
            // 读取一帧数据
            return true;
        }
    };

    SbusReceiver sbus;

    // 初始化函数
    bool init() {
        return sbus.open("/dev/ttyS1");
    }

    // 运行循环（在独立线程中）
    void run(std::atomic<bool>& running) {
        while (running) {
            if (sbus.readFrame()) {
                // 解析帧并写共享 SbusData；状态机由主线程读取。
            }
            std::this_thread::sleep_for(10ms);
        }
    }

    // 清理函数
    void cleanup() {
        sbus.close();
    }
}

// --- ATOM-04: CAN 模块（需要独立线程收发数据）---
namespace atom04 {
    struct CanBus {
        bool open(const char* interface) {
            std::cout << "[CAN] Opening interface: " << interface << std::endl;
            return true;
        }

        void close() {
            std::cout << "[CAN] Closing interface" << std::endl;
        }

        void processMessages() {
            // 处理 CAN 消息
        }
    };

    CanBus can;

    bool init() {
        return can.open("can0");
    }

    void run(std::atomic<bool>& running) {
        while (running) {
            can.processMessages();
            std::this_thread::sleep_for(1ms);
        }
    }

    void cleanup() {
        can.close();
    }
}

// --- ATOM-15: 配置模块（只需要初始化，不需要独立线程）---
namespace atom15 {
    struct ConfigStore {
        bool load(const char* path) {
            std::cout << "[Config] Loading: " << path << std::endl;
            return true;
        }

        void shutdown() {
            std::cout << "[Config] Saving changes" << std::endl;
        }
    };

    ConfigStore config;

    bool init() {
        return config.load("dart_control.json");
    }

    void cleanup() {
        config.shutdown();
    }
}

// --- ATOM-16: HTTP 服务（需要独立线程监听）---
namespace atom16 {
    struct HttpServer {
        bool start(const char* host, int port) {
            std::cout << "[HTTP] Starting server on " << host << ":" << port << std::endl;
            return true;
        }

        void listen() {
            // HTTP 服务循环（阻塞）
        }

        void stop() {
            std::cout << "[HTTP] Stopping server" << std::endl;
        }
    };

    HttpServer http;

    bool init() {
        return http.start("0.0.0.0", 8080);
    }

    void run(std::atomic<bool>& running) {
        // cpp-httplib 内部有自己的循环，这里简化处理
        while (running) {
            std::this_thread::sleep_for(100ms);
        }
    }

    void cleanup() {
        http.stop();
    }
}

// --- ATOM-17: WebSocket 服务（需要独立线程推送）---
namespace atom17 {
    struct WebSocketServer {
        bool start(int port) {
            std::cout << "[WebSocket] Starting on port " << port << std::endl;
            return true;
        }

        void pushUpdate(/* state data */) {
            // 推送状态更新到所有连接的客户端
        }

        void stop() {
            std::cout << "[WebSocket] Stopping" << std::endl;
        }
    };

    WebSocketServer ws;
    state::StateMachine* state_machine = nullptr;

    bool init() {
        return ws.start(8081);
    }

    void run(std::atomic<bool>& running) {
        while (running) {
            if (state_machine) {
                ws.pushUpdate(/* state_machine->control_source() */);
            }
            std::this_thread::sleep_for(100ms);
        }
    }

    void cleanup() {
        ws.stop();
    }
}

// =============================================================================
// 主程序：集成所有模块
// =============================================================================

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  Dart Control System (Minimal ATOM01)" << std::endl;
    std::cout << "========================================" << std::endl;

    sbus::SbusData sbus_data;
    web::WebData web_data;
    state::StateMachine state{sbus_data, web_data};
    atom17::state_machine = &state;

    // 创建 RuntimeCoordinator
    runtime::RuntimeCoordinator runtime;

    // 注册所有模块（按依赖顺序）
    //
    // 注册模式 1：只需要初始化的模块（无独立线程）
    runtime.registerModule("Config", atom15::init, atom15::cleanup);

    // 注册模式 2：需要独立线程的模块
    runtime.registerModule("SBUS", atom03::init, atom03::run, atom03::cleanup);
    runtime.registerModule("CAN", atom04::init, atom04::run, atom04::cleanup);
    runtime.registerModule("HTTP", atom16::init, atom16::run, atom16::cleanup);
    runtime.registerModule("WebSocket", atom17::init, atom17::run, atom17::cleanup);

    // 启动所有模块
    if (!runtime.start()) {
        std::cerr << "\n[Main] Failed to start runtime:" << std::endl;
        for (const auto& err : runtime.errors()) {
            std::cerr << "  - " << err.step_name << ": " << err.message << std::endl;
        }
        return 1;
    }

    std::cout << "\n[Main] System running. Press Ctrl+C to exit." << std::endl;

    // 主循环（在主线程执行控制逻辑）
    while (true) {
        state.updateControlSource();
        std::this_thread::sleep_for(20ms);

        // 临时：10 秒后退出
        static int count = 0;
        if (++count > 500) {
            std::cout << "\n[Main] Demo timeout, shutting down..." << std::endl;
            break;
        }
    }

    // 关闭所有模块
    runtime.shutdown();

    std::cout << "\n[Main] Exit" << std::endl;
    return 0;
}

// =============================================================================
// 输出示例
// =============================================================================
/*
========================================
  Dart Control System (Minimal ATOM01)
========================================
[Runtime] Initializing Config...
[Config] Loading: dart_control.json
[Runtime] Initializing SBUS...
[SBUS] Opening port: /dev/ttyS1
[Runtime] Initializing CAN...
[CAN] Opening interface: can0
[Runtime] Initializing HTTP...
[HTTP] Starting server on 0.0.0.0:8080
[Runtime] Initializing WebSocket...
[WebSocket] Starting on port 8081
[Runtime] Starting threads...
[Runtime] Thread started: SBUS
[Runtime] Thread started: CAN
[Runtime] Thread started: HTTP
[Runtime] Thread started: WebSocket
[Runtime] 4 threads started
[Runtime] All modules started successfully
[State] → STANDBY

[Main] System running. Press Ctrl+C to exit.

[Main] Demo timeout, shutting down...
[Runtime] Shutting down...
[Runtime] Stopping threads...
[Runtime] Thread exited: SBUS
[Runtime] Thread exited: CAN
[Runtime] Thread exited: HTTP
[Runtime] Thread exited: WebSocket
[Runtime] All threads stopped
[Runtime] Rollback: cleaning up initialized modules...
[Runtime] Cleaning up WebSocket
[WebSocket] Stopping
[Runtime] Cleaning up HTTP
[HTTP] Stopping server
[Runtime] Cleaning up CAN
[CAN] Closing interface
[Runtime] Cleaning up SBUS
[SBUS] Closing port
[Runtime] Cleaning up Config
[Config] Saving changes
[Runtime] Stopped

[Main] Exit
*/
