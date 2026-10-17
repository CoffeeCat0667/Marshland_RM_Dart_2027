# ATOM-01/02 最小化重构方案

**目标**: 将 ATOM-01 和 ATOM-02 精简到最小可用状态，保留核心功能，删除过度抽象

**当前状态**:
- ATOM-01: ~400+ 行（RuntimeCoordinator + 生命周期接口）
- ATOM-02: ~1,063 行（StateArbitration + 复杂状态机）
- 总计: ~1,500 行

**目标状态**: 
- ATOM-01: ~150-200 行（简单启动管理器）
- ATOM-02: ~400-500 行（精简状态机）
- **总减少: ~600-950 行 (40-63%)**

---

## 第一部分：ATOM-01 精简方案

### 当前问题

1. **过度抽象的生命周期接口**
```cpp
// 当前：每个模块都要实现复杂接口
class ILifecycleModule {
    virtual ModuleInitResult initialize(const RuntimeContext&) = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

// 问题：
// - 19 个模块都要写适配器
// - RuntimeContext 传递冗余信息
// - ModuleInitResult 结构体过度设计
```

2. **复杂的步骤枚举**
```cpp
enum class RuntimeStep {
    CreateDiagnostics,
    LoadConfig,
    ValidateConfig,
    LoadMotorPid,
    InitializeCan,
    InitializeMotors,
    InitializeGpio,
    InitializeI2c,
    InitializeServos,
    InitializeSbusAndVision,
    StartHttpAndWebSocket,
    // ... 12 个步骤
};
```

3. **注册机制过度工程化**
```cpp
struct ModuleRegistration {
    RuntimeStep step;
    bool required;
    std::unique_ptr<ILifecycleModule> module;
};
coordinator.addModule({RuntimeStep::LoadConfig, true, std::move(config)});
```

---

### 精简方案：直接调用序列

#### 新设计：Application 类

```cpp
// Application.hpp
#pragma once
#include <string>
#include <vector>
#include <functional>

namespace marshland {

enum class SystemState {
    UNINITIALIZED,
    RUNNING,
    FAILED
};

struct InitError {
    std::string step_name;
    std::string message;
};

class Application {
public:
    Application() = default;
    ~Application() { shutdown(); }
    
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    
    // 启动所有模块（返回 false 表示失败）
    bool start(std::string& error_message);
    
    // 关闭所有模块
    void shutdown();
    
    // 查询状态
    SystemState state() const { return state_; }
    const std::vector<InitError>& errors() const { return errors_; }
    
private:
    SystemState state_{SystemState::UNINITIALIZED};
    std::vector<InitError> errors_;
    
    // 已启动的模块（用于反序关闭）
    std::vector<std::function<void()>> cleanup_handlers_;
    
    // 工具函数：记录步骤
    bool tryStep(const char* step_name, std::function<bool()> fn);
};

} // namespace marshland
```

#### 实现：线性启动序列

```cpp
// Application.cpp
#include "Application.hpp"
#include "ConfigStore.hpp"
#include "CanBus.hpp"
#include "SbusReceiver.hpp"
#include "HttpServer.hpp"
#include "WebSocketServer.hpp"
#include <iostream>

namespace marshland {

bool Application::tryStep(const char* step_name, std::function<bool()> fn) {
    std::cout << "[Init] " << step_name << "..." << std::endl;
    try {
        if (!fn()) {
            errors_.push_back({step_name, "step returned false"});
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        errors_.push_back({step_name, e.what()});
        return false;
    }
}

bool Application::start(std::string& error_message) {
    if (state_ != SystemState::UNINITIALIZED) {
        error_message = "already started";
        return false;
    }
    
    // 1. 加载配置
    static ConfigStore config;
    if (!tryStep("Load Config", [&]() {
        if (!config.load("dart_control.json")) {
            return false;
        }
        cleanup_handlers_.push_back([&]() { config.shutdown(); });
        return true;
    })) {
        goto fail;
    }
    
    // 2. 初始化 CAN 总线
    static CanBus can_bus;
    if (!tryStep("Initialize CAN", [&]() {
        std::string can_interface = config.get<std::string>("can.interface").value_or("can0");
        if (!can_bus.open(can_interface)) {
            return false;
        }
        cleanup_handlers_.push_back([&]() { can_bus.close(); });
        return true;
    })) {
        goto fail;
    }
    
    // 3. 初始化 SBUS
    static SbusReceiver sbus;
    if (!tryStep("Initialize SBUS", [&]() {
        std::string sbus_port = config.get<std::string>("sbus.port").value_or("/dev/ttyS1");
        if (!sbus.open(sbus_port)) {
            return false;
        }
        cleanup_handlers_.push_back([&]() { sbus.close(); });
        return true;
    })) {
        goto fail;
    }
    
    // 4. 启动 HTTP 服务
    static HttpServer http;
    if (!tryStep("Start HTTP Server", [&]() {
        int port = config.get<int>("http.port").value_or(8080);
        if (!http.start("0.0.0.0", port)) {
            return false;
        }
        cleanup_handlers_.push_back([&]() { http.stop(); });
        return true;
    })) {
        goto fail;
    }
    
    // 5. 启动 WebSocket
    static WebSocketServer ws;
    if (!tryStep("Start WebSocket", [&]() {
        int port = config.get<int>("websocket.port").value_or(8081);
        if (!ws.start("0.0.0.0", port)) {
            return false;
        }
        cleanup_handlers_.push_back([&]() { ws.stop(); });
        return true;
    })) {
        goto fail;
    }
    
    // 全部成功
    state_ = SystemState::RUNNING;
    std::cout << "[Init] All modules started successfully" << std::endl;
    return true;
    
fail:
    // 失败：反序关闭已启动的模块
    state_ = SystemState::FAILED;
    error_message = "Initialization failed at: " + errors_.back().step_name;
    shutdown();
    return false;
}

void Application::shutdown() {
    if (state_ == SystemState::UNINITIALIZED) {
        return;
    }
    
    std::cout << "[Shutdown] Stopping modules..." << std::endl;
    
    // 反序调用清理函数
    for (auto it = cleanup_handlers_.rbegin(); it != cleanup_handlers_.rend(); ++it) {
        try {
            (*it)();
        } catch (...) {
            // 忽略关闭时的异常
        }
    }
    cleanup_handlers_.clear();
    state_ = SystemState::UNINITIALIZED;
}

} // namespace marshland
```

#### 使用示例

```cpp
// main.cpp
#include "Application.hpp"
#include <iostream>

int main() {
    marshland::Application app;
    
    std::string error;
    if (!app.start(error)) {
        std::cerr << "Failed to start: " << error << std::endl;
        for (const auto& err : app.errors()) {
            std::cerr << "  - " << err.step_name << ": " << err.message << std::endl;
        }
        return 1;
    }
    
    std::cout << "Application running. Press Ctrl+C to exit." << std::endl;
    // ... 主循环
    
    app.shutdown();
    return 0;
}
```

---

### 对比：精简效果

| 特性 | 当前设计 | 精简设计 | 减少 |
|------|---------|---------|------|
| 代码行数 | ~400 行 | ~150 行 | **-62%** |
| 抽象层次 | 3 层（接口/注册器/协调器） | 1 层（Application） | **-67%** |
| 配置复杂度 | RuntimeStep 枚举 + 注册 | 线性序列 | **-100%** |
| 适配器数量 | 19 个 | 0 个 | **-100%** |

**删除的概念**:
- ✂️ `ILifecycleModule` 接口
- ✂️ `RuntimeStep` 枚举
- ✂️ `ModuleRegistration` 结构
- ✂️ `RuntimeContext` 传递
- ✂️ `ModuleInitResult` 详细结果
- ✂️ 19 个生命周期适配器

**保留的核心功能**:
- ✅ 顺序启动
- ✅ 失败时反序关闭
- ✅ 错误信息记录
- ✅ 简单的状态查询

---

## 第二部分：ATOM-02 精简方案

### 当前问题

1. **状态爆炸**
   - 7 个系统模式
   - 12 个自动流程子状态
   - 60+ 条转换边
   - 17 条自动流程转换规则

2. **过度抽象的策略函数**
```cpp
// 当前：大量纯函数策略
bool IsModeTransitionAllowed(SystemMode from, SystemMode to, TransitionReason reason);
bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to);
bool IsAutoFireTransitionAllowed(AutoFireState from, AutoFireState to, ShotStage stage);
bool IsFaultBlocking(const ExternalInputs& inputs);
bool IsOperationalMode(SystemMode mode);
FireAuthorization EvaluateFireAuthorization(...);
IntentDecision EvaluateIntent(...);

// 问题：为简单逻辑引入过多函数
```

3. **控制权仲裁过度复杂**
```cpp
// 当前：复杂的仲裁结构
struct ArbitrationContext {
    SystemMode system_mode;
    AutoFireState auto_fire_state;
    ControlSource control_source;
    ExternalInputs inputs;
    // ... 更多字段
};

// 问题：上下文传递冗余信息
```

---

### 精简方案：精简状态机

#### 新设计：5 个核心状态

```cpp
// StateMachine.hpp
#pragma once
#include <cstdint>

namespace marshland {

// 精简到 5 个核心状态
enum class SystemState {
    INIT_FAILED,   // 初始化失败（禁止所有动作）
    STANDBY,       // 待机（可接受手动/自动命令）
    MANUAL,        // 手动控制中
    AUTO,          // 自动流程运行中
    ESTOP          // 急停（最高优先级）
};

// 精简到 8 个自动流程阶段
enum class AutoStage {
    IDLE,          // 空闲
    CHARGE,        // 蓄力
    AIM,           // 瞄准（含 YAW 调整）
    FIRE,          // 发射
    RELOAD,        // 换弹（含抬起/供弹/下降）
    NEXT_SHOT,     // 准备下一发
    COMPLETED,     // 全部完成
    ABORTED        // 中止
};

// 3 个控制源（优先级：SBUS > WEB > AUTO）
enum class ControlSource {
    AUTO,   // 自动流程（最低优先级）
    WEB,    // Web 界面
    SBUS    // 遥控器（最高优先级）
};

// 外部输入（简化为必要字段）
struct SystemInputs {
    bool sbus_online{false};
    bool sbus_auto_mode{false};  // SBUS 上的自动/手动开关
    bool web_online{false};
    bool estop_triggered{false};
    bool stop_fire{false};
    bool yaw_ready{false};
    bool has_severe_fault{false};  // 只关心阻塞性故障
};

class StateMachine {
public:
    StateMachine() = default;
    
    // ---- 状态查询 ----
    SystemState state() const { return state_; }
    AutoStage auto_stage() const { return auto_stage_; }
    ControlSource control_source() const { return control_source_; }
    int current_shot() const { return current_shot_; }
    bool is_estop() const { return state_ == SystemState::ESTOP; }
    
    // ---- 外部输入 ----
    void updateInputs(const SystemInputs& inputs);
    
    // ---- 状态切换 ----
    bool setInitFailed();                    // 初始化失败
    bool setStandby();                       // 初始化成功 → 待机
    bool enterManual();                      // 进入手动模式
    bool startAuto();                        // 开始自动流程
    bool stopAuto();                         // 停止自动流程
    bool triggerEstop();                     // 触发急停
    bool confirmEstop();                     // 确认急停（允许恢复动作）
    bool releaseEstop();                     // 解除急停
    
    // ---- 自动流程推进 ----
    bool advanceAutoStage(AutoStage next);   // 推进到下一阶段
    
    // ---- 发射授权检查（每次发射前调用）----
    bool canFire(ControlSource requester, std::string& deny_reason) const;
    
private:
    SystemState state_{SystemState::STANDBY};
    AutoStage auto_stage_{AutoStage::IDLE};
    ControlSource control_source_{ControlSource::AUTO};
    SystemInputs inputs_;
    int current_shot_{0};           // 0 = 未开始，1-4 = 发次
    bool estop_confirmed_{false};   // 急停是否已人工确认
    
    // 内部辅助
    void updateControlSource();     // 根据输入更新控制源
    bool isOperational() const;     // 是否可执行动作
};

} // namespace marshland
```

#### 实现：简化逻辑

```cpp
// StateMachine.cpp
#include "StateMachine.hpp"
#include <iostream>

namespace marshland {

void StateMachine::updateInputs(const SystemInputs& inputs) {
    inputs_ = inputs;
    
    // 急停输入锁存（一旦触发，必须人工解除）
    if (inputs_.estop_triggered && state_ != SystemState::ESTOP) {
        triggerEstop();
    }
    
    // 更新控制源选择
    updateControlSource();
}

void StateMachine::updateControlSource() {
    // 简化的控制源选择规则
    if (inputs_.sbus_online) {
        control_source_ = ControlSource::SBUS;
    } else if (inputs_.web_online) {
        control_source_ = ControlSource::WEB;
    } else {
        control_source_ = ControlSource::AUTO;
    }
}

bool StateMachine::isOperational() const {
    // 只有待机/手动/自动状态可执行动作
    return state_ == SystemState::STANDBY ||
           state_ == SystemState::MANUAL ||
           state_ == SystemState::AUTO;
}

bool StateMachine::setInitFailed() {
    state_ = SystemState::INIT_FAILED;
    std::cout << "[State] INIT_FAILED" << std::endl;
    return true;
}

bool StateMachine::setStandby() {
    if (state_ == SystemState::INIT_FAILED) {
        return false;  // 初始化失败后不可恢复
    }
    state_ = SystemState::STANDBY;
    auto_stage_ = AutoStage::IDLE;
    current_shot_ = 0;
    std::cout << "[State] STANDBY" << std::endl;
    return true;
}

bool StateMachine::enterManual() {
    if (!isOperational()) {
        return false;
    }
    state_ = SystemState::MANUAL;
    std::cout << "[State] MANUAL" << std::endl;
    return true;
}

bool StateMachine::startAuto() {
    if (state_ != SystemState::STANDBY && state_ != SystemState::MANUAL) {
        return false;
    }
    if (inputs_.has_severe_fault) {
        return false;  // 故障阻断
    }
    
    state_ = SystemState::AUTO;
    auto_stage_ = AutoStage::CHARGE;
    current_shot_ = 1;
    std::cout << "[State] AUTO started (shot 1)" << std::endl;
    return true;
}

bool StateMachine::stopAuto() {
    if (state_ != SystemState::AUTO) {
        return true;  // 幂等
    }
    state_ = SystemState::MANUAL;
    auto_stage_ = AutoStage::ABORTED;
    std::cout << "[State] AUTO aborted" << std::endl;
    return true;
}

bool StateMachine::triggerEstop() {
    state_ = SystemState::ESTOP;
    auto_stage_ = AutoStage::ABORTED;
    estop_confirmed_ = false;
    std::cout << "[State] ESTOP triggered" << std::endl;
    return true;
}

bool StateMachine::confirmEstop() {
    if (state_ != SystemState::ESTOP) {
        return false;
    }
    estop_confirmed_ = true;
    std::cout << "[State] ESTOP confirmed (recovery actions allowed)" << std::endl;
    return true;
}

bool StateMachine::releaseEstop() {
    if (state_ != SystemState::ESTOP || !estop_confirmed_) {
        return false;
    }
    state_ = SystemState::STANDBY;
    estop_confirmed_ = false;
    std::cout << "[State] ESTOP released → STANDBY" << std::endl;
    return true;
}

bool StateMachine::advanceAutoStage(AutoStage next) {
    if (state_ != SystemState::AUTO) {
        return false;
    }
    
    auto_stage_ = next;
    
    // 处理发次递增
    if (next == AutoStage::NEXT_SHOT && current_shot_ < 4) {
        current_shot_++;
        std::cout << "[Auto] Shot " << current_shot_ << std::endl;
    } else if (next == AutoStage::COMPLETED) {
        std::cout << "[Auto] COMPLETED (4 shots fired)" << std::endl;
        state_ = SystemState::STANDBY;
        auto_stage_ = AutoStage::IDLE;
        current_shot_ = 0;
    }
    
    return true;
}

bool StateMachine::canFire(ControlSource requester, std::string& deny_reason) const {
    // 1. 急停检查
    if (state_ == SystemState::ESTOP) {
        deny_reason = "Emergency stop active";
        return false;
    }
    
    // 2. 初始化失败
    if (state_ == SystemState::INIT_FAILED) {
        deny_reason = "System not initialized";
        return false;
    }
    
    // 3. 故障阻断
    if (inputs_.has_severe_fault) {
        deny_reason = "Severe fault detected";
        return false;
    }
    
    // 4. 停止发射标志
    if (inputs_.stop_fire) {
        deny_reason = "Stop fire flag set";
        return false;
    }
    
    // 5. YAW 未到位
    if (!inputs_.yaw_ready) {
        deny_reason = "YAW not ready";
        return false;
    }
    
    // 6. 控制权优先级检查（SBUS > WEB > AUTO）
    int requester_priority = (int)requester;
    int holder_priority = (int)control_source_;
    if (requester_priority < holder_priority) {
        deny_reason = "Control authority conflict (priority too low)";
        return false;
    }
    
    return true;
}

} // namespace marshland
```

---

### 对比：精简效果

| 特性 | 当前设计 | 精简设计 | 减少 |
|------|---------|---------|------|
| 代码行数 | ~1,063 行 | ~400 行 | **-62%** |
| 系统状态数 | 11 个 | 5 个 | **-55%** |
| 自动流程状态数 | 14 个 | 8 个 | **-43%** |
| 纯函数策略 | 7 个 | 0 个 | **-100%** |
| 转换规则表 | 60+ 条边 | 隐式逻辑 | **-100%** |

**删除的概念**:
- ✂️ `TransitionReason` 枚举（60+ 种原因）
- ✂️ `ArbitrationContext` 复杂上下文
- ✂️ `ModeEdge` / `AutoFireEdge` 转换表
- ✂️ 7 个纯函数策略
- ✂️ `IntentKind` / `IntentDecision` 仲裁结构
- ✂️ 事件缓冲区（64 个事件记录）

**保留的核心功能**:
- ✅ 状态切换（5 个核心状态）
- ✅ 控制权优先级（SBUS > WEB > AUTO）
- ✅ 发射授权检查
- ✅ 急停锁存与确认
- ✅ 自动流程推进（8 个阶段）

---

## 第三部分：集成方案

### 精简后的主程序结构

```cpp
// main.cpp
#include "Application.hpp"
#include "StateMachine.hpp"
#include "ControlLoop.hpp"
#include <iostream>
#include <csignal>

namespace {
    volatile bool g_running = true;
    void signal_handler(int) { g_running = false; }
}

int main() {
    // 捕获 Ctrl+C
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // 1. 启动应用（替代 ATOM-01）
    marshland::Application app;
    std::string error;
    if (!app.start(error)) {
        std::cerr << "Startup failed: " << error << std::endl;
        return 1;
    }
    
    // 2. 创建状态机（替代 ATOM-02）
    marshland::StateMachine state;
    state.setStandby();  // 启动成功 → 待机
    
    // 3. 主控制循环
    marshland::ControlLoop loop(state);
    
    while (g_running && app.state() == marshland::SystemState::RUNNING) {
        // 读取外部输入
        marshland::SystemInputs inputs;
        inputs.sbus_online = /* 从 SBUS 模块读取 */;
        inputs.web_online = /* 从 WebSocket 模块读取 */;
        // ...
        
        state.updateInputs(inputs);
        
        // 执行控制逻辑
        loop.tick();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // 4. 关闭
    app.shutdown();
    return 0;
}
```

---

## 第四部分：迁移路径

### 阶段 1: 准备（1 天）

1. **创建新文件**
   ```
   Atoms/Core/
   ├── Application.hpp          (替代 ATOM-01)
   ├── Application.cpp
   ├── StateMachine.hpp         (替代 ATOM-02)
   └── StateMachine.cpp
   ```

2. **保留旧代码**（暂时）
   ```
   Atoms/ATOM-01_Runtime/       (重命名为 _Runtime.old)
   Atoms/ATOM-02_State_Arbitration/  (重命名为 _StateArbitration.old)
   ```

---

### 阶段 2: 实现新代码（2-3 天）

1. **实现 Application 类**
   - 复制上面的代码模板
   - 调整模块启动顺序
   - 测试单独启动每个模块

2. **实现 StateMachine 类**
   - 复制上面的代码模板
   - 添加单元测试（状态转换）
   - 验证控制权仲裁逻辑

3. **编写单元测试**
   ```cpp
   // test_state_machine.cpp
   TEST(StateMachine, BasicTransitions) {
       StateMachine sm;
       ASSERT_TRUE(sm.setStandby());
       ASSERT_EQ(sm.state(), SystemState::STANDBY);
       
       ASSERT_TRUE(sm.startAuto());
       ASSERT_EQ(sm.state(), SystemState::AUTO);
   }
   
   TEST(StateMachine, FireAuthorization) {
       StateMachine sm;
       sm.setStandby();
       
       SystemInputs inputs;
       inputs.yaw_ready = true;
       sm.updateInputs(inputs);
       
       std::string reason;
       ASSERT_TRUE(sm.canFire(ControlSource::AUTO, reason));
   }
   ```

---

### 阶段 3: 迁移调用点（3-5 天）

1. **查找旧代码调用**
   ```bash
   grep -r "RuntimeCoordinator" Atoms/
   grep -r "StateArbitrator" Atoms/
   grep -r "ILifecycle" Atoms/
   ```

2. **逐个替换**
   
   **示例 1: 启动代码**
   ```cpp
   // 旧代码 (Entry.cpp)
   runtime::RuntimeCoordinator coordinator;
   coordinator.addModule({RuntimeStep::LoadConfig, true, std::move(config)});
   coordinator.addModule({RuntimeStep::InitializeCan, true, std::move(can)});
   // ... 19 次注册
   auto result = coordinator.start();
   if (!result.succeeded()) { /* 处理失败 */ }
   
   // 新代码
   Application app;
   std::string error;
   if (!app.start(error)) {
       std::cerr << "Failed: " << error << std::endl;
       return 1;
   }
   ```
   
   **示例 2: 状态查询**
   ```cpp
   // 旧代码
   state_arbitration::StateArbitrator arbitrator;
   auto ctx = arbitrator.context();
   if (ctx.system_mode == SystemMode::AUTO_FIRE) { /* ... */ }
   
   // 新代码
   StateMachine state;
   if (state.state() == SystemState::AUTO) { /* ... */ }
   ```
   
   **示例 3: 发射授权**
   ```cpp
   // 旧代码
   auto auth = EvaluateFireAuthorization(ctx, ControlSource::WEB, control_holder);
   if (auth.authorized) { fire(); }
   
   // 新代码
   std::string deny_reason;
   if (state.canFire(ControlSource::WEB, deny_reason)) {
       fire();
   } else {
       std::cerr << "Fire denied: " << deny_reason << std::endl;
   }
   ```

3. **更新依赖模块**
   - ATOM-03 (SBUS): 简化状态上报
   - ATOM-16 (HTTP): 简化状态查询 API
   - ATOM-17 (WebSocket): 简化状态推送
   - ATOM-11 (Fire): 简化发射授权检查

---

### 阶段 4: 测试与验证（2-3 天）

1. **单元测试**
   - 所有状态转换
   - 控制权仲裁
   - 发射授权检查

2. **集成测试**
   - 完整启动流程
   - SBUS 接管自动流程
   - 急停 → 确认 → 解除

3. **回归测试**
   - 运行原有测试套件
   - 验证业务逻辑不变

---

### 阶段 5: 删除旧代码（1 天）

1. **确认新代码稳定后删除**
   ```bash
   rm -rf Atoms/ATOM-01_Runtime.old
   rm -rf Atoms/ATOM-02_State_Arbitration.old
   rm -rf Atoms/Public/ILifecycle.hpp
   ```

2. **更新 CMakeLists.txt**
   ```cmake
   # 删除旧模块
   # add_subdirectory(Atoms/ATOM-01_Runtime)
   # add_subdirectory(Atoms/ATOM-02_State_Arbitration)
   
   # 添加新模块
   add_subdirectory(Atoms/Core)
   ```

3. **更新文档**
   - 删除 `Design/Atomic/ATOM-01_Runtime.md`
   - 删除 `Design/Atomic/ATOM-02_State_Arbitration.md`
   - 添加 `Design/Core-Architecture.md`（简化版）

---

## 第五部分：风险评估

### 高风险点

1. **状态语义变化**
   - **风险**: 合并状态可能丢失原有的细粒度控制
   - **缓解**: 在关键分支点保留断言，确保行为一致

2. **控制权仲裁简化**
   - **风险**: 简化后的优先级可能不够精细
   - **缓解**: 保留原有的 3 级优先级（SBUS > WEB > AUTO）

3. **事件记录丢失**
   - **风险**: 删除事件缓冲区后难以追溯状态变化
   - **缓解**: 用日志替代（spdlog），按需记录关键事件

### 中风险点

4. **模块启动顺序变化**
   - **风险**: 线性序列可能不满足复杂依赖
   - **缓解**: 保留原有顺序，只是简化注册机制

5. **测试覆盖不足**
   - **风险**: 删除大量代码后可能引入新 Bug
   - **缓解**: 编写充分的单元测试 + 集成测试

### 低风险点

6. **性能变化**
   - **风险**: 去除抽象层后性能可能略有波动
   - **评估**: 实际影响微乎其微（嵌入式系统主要瓶颈在硬件 I/O）

7. **可扩展性降低**
   - **风险**: 简化后难以扩展新状态
   - **评估**: 对竞赛项目而言，5 个状态已足够

---

## 第六部分：回退计划

### 如果新代码出现严重问题

1. **保留旧代码分支**
   ```bash
   git checkout -b feature/minimal-atom01-02
   # 在新分支开发，主分支保留旧代码
   ```

2. **并行运行一段时间**
   - 新代码先在测试环境验证 1-2 周
   - 确认稳定后再合并主分支

3. **快速回退机制**
   ```cmake
   # CMakeLists.txt 保留开关
   option(USE_LEGACY_RUNTIME "Use legacy ATOM-01/02" OFF)
   
   if(USE_LEGACY_RUNTIME)
       add_subdirectory(Atoms/ATOM-01_Runtime)
       add_subdirectory(Atoms/ATOM-02_State_Arbitration)
   else()
       add_subdirectory(Atoms/Core)
   endif()
   ```

---

## 总结

### 收益

| 维度 | 改进 |
|------|------|
| **代码量** | -1,000 行 (-62%) |
| **抽象层次** | 从 3 层降至 1 层 |
| **状态复杂度** | 从 11+14=25 个状态降至 5+8=13 个 |
| **维护成本** | 大幅降低（无需维护 19 个适配器） |
| **新手理解成本** | 从 2-3 天降至 1-2 小时 |

### 工作量

- **总时间**: 10-15 天（1 人全职）
- **高风险期**: 阶段 3（迁移调用点）
- **验证时间**: 2-3 天（不可压缩）

### 建议

1. **优先级**: ⭐⭐⭐⭐⭐ 强烈建议
   - ATOM-01/02 是架构基础，精简后整体复杂度大幅降低

2. **时机**: 在 **下一个大版本** 开始前重构
   - 不建议在临近比赛前大改

3. **团队规模**: 
   - 3-5 人团队：**强烈推荐**
   - 10+ 人团队：可保留当前复杂度

---

**下一步**: 如果你决定重构，我可以帮你：
1. 生成完整的可编译代码
2. 编写迁移脚本（自动替换调用点）
3. 设计单元测试用例
4. 审查风险点的缓解措施
