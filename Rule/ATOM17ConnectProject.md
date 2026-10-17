# ATOM-17 WebSocket 接入主系统工程文档

> 版本：1.0　编制日期：2026-10-16
> 模块：`Atoms/ATOM-17_WebSocket`（目标 `Marshland::atom_17_websocket`）
> 功能权威来源：`Design/Atomic/ATOM-17_WebSocket.md`；数值唯一来源 `Design/Config.md` §12；
> 键名契约 `Rule/ConfigSchema.md` §2.14 与 `Rule/ConfigSchema.schema.json`。
> 本文档面向主系统（上层装配层）负责人，描述把 ATOM-17 接入 `marshland`
> 主程序所需的前置条件、装配步骤、运行验证与已知边界。

---

## 1. 模块现状

ATOM-17 内部实现已完成并通过 WSL aarch64 增量编译（`atom_17_websocket`
编译零警告，全工程链接通过）。已实现：

- 独立监听 `websocket.listen_address:websocket.port`（`0.0.0.0:10002`，
  独立于 HTTP 10000 与视觉 10001），任意路径握手即创建只读订阅；
- 应用层 JSON 心跳：校验客户端 `ping`，回复关联相同 `request_id` 的 `pong`；
  连续 3 个心跳周期（`connection_timeout_ms`）无有效数据判定断开并清理会话；
- 统一信封周期推送（`push_period_ms`），字段名与顺序来自配置
  （`envelope_fields` / `message_types[].payload_fields`）；
- 只读隔离：一切非 ping 客户端消息返回 `error` 信封并提示改用 HTTP，
  不执行任何动作；
- `ReportWebOnline` 唯一写入方（≥1 有效会话 → true，0 会话 → false）；
- `LifecycleAdapter`（`marshland::runtime::ILifecycleModule`），可注册到
  `RuntimeStep::StartHttpAndWebSocket`。

对外接口（`Inc/atom17_websocket/`，无任何第三方库类型）：

- `WebSocketServer`（`websocket_server.hpp`）：核心，构造注入依赖；
- `LifecycleAdapter`（`lifecycle_adapter.hpp`）：生命周期适配器；
- `IWsTransport`（`ws_transport.hpp`）：传输抽象，测试可注入替身；
- `LoadWsConfig`（`ws_config.hpp`）：配置读取与校验（装配层无需直接调用）。

---

## 2. 接入前置条件（必须逐项完成）

### P1（必需）：`Config/dart_control.json` 补齐 websocket 配置

`LoadWsConfig` 对下列键全部按必需处理，任何键缺失/类型错误/越界都会使
ATOM-17 初始化失败（不回填默认值）。顶层 `websocket` 对象参考：

```jsonc
"websocket": {
  "listen_address": "0.0.0.0",
  "port": 10002,
  "message_version": "1.0",
  "heartbeat_period_ms": 1000,
  "connection_timeout_ms": 3000,
  "reconnect_backoff_s": [1, 2, 4, 8, 16],
  "reconnect_backoff_max_s": 30,
  "reconnect_jitter": true,
  "control_over_ws_allowed": false,
  "multi_client_mode": "READ_ONLY_SUBSCRIBE",
  "push_period_ms": 20,
  "envelope_fields": ["version", "type", "timestamp", "request_id", "source", "payload"],
  "heartbeat_message": { "version": "1.0", "type": "ping", "request_id": "", "payload": {} },
  "message_types": [
    { "type": "system_state",
      "payload_fields": ["system_mode", "initialized", "control_source",
                          "auto_fire_state", "current_shot", "total_shots",
                          "stop_fire", "estop_active"] },
    { "type": "mechanism_state",
      "payload_fields": ["charge", "reload", "feed", "force", "fire", "yaw"] },
    { "type": "motor_state", "payload_fields": ["motors"] },
    { "type": "sensor_state", "payload_fields": ["gpio_inputs"] },
    { "type": "yaw_state",
      "payload_fields": ["target_angle", "arrived", "cleared_by_main"] },
    { "type": "fire_progress",
      "payload_fields": ["current_shot", "total_shots", "state", "stage", "stop_fire"] },
    { "type": "fault", "payload_fields": ["active_faults"] },
    { "type": "estop_state",
      "payload_fields": ["active", "source", "confirm_required", "recovery_state"] },
    { "type": "control_arbiter",
      "payload_fields": ["active_source", "priority_order", "preempt_reason"] },
    { "type": "parameter_changed",
      "payload_fields": ["key", "accepted", "persisted", "effective", "error_code"] },
    { "type": "error", "error_message_payload": true,
      "payload_fields": ["code", "message", "details", "request_id"] }
  ]
}
```

同时确认以下既有键存在（ATOM-17 直接消费）：

- `timing.websocket_io_timeout_ms`（设计值 200，Config.md §4）；
- `baseline.auto_total_shots`（固定 4）；
- `baseline.fire_priority_order`（`["SBUS","WEB","AUTO"]`）。

`payload_fields` 逐类型字段名以 `Design/Config.md` §12.3 为准；上表为对齐样例，
修改字段集合必须同步 `Config.md` 与两处 Schema。

### P2（必需）：`Rule/ConfigSchema.schema.json` 同步监听键

形式化 Schema 的 `websocket` 段为 `additionalProperties: false` 且尚无
`listen_address`/`port`：**不同步会使含 P1 新键的配置被 ATOM-15 校验拒绝，
导致整机启动失败**。需在 `websocket.properties` 中补：

```json
"listen_address": { "type": "string", "minLength": 1 },
"port": { "type": "integer", "minimum": 1, "maximum": 65535 }
```

（`Rule/ConfigSchema.md` §2.14 与 `Design/Config.md` §12.1 已于 2026-10-15
同步登记；仅差本 Schema 文件。）

### P3（可选）：ATOM-15 覆盖嵌套数组查询

`Atoms/Public/IConfigView.hpp` 已新增带默认实现的
`getNestedArraySize` / `getNestedStringAt`（默认返回 MISSING，非破坏）。
ATOM-15 覆盖这两个方法之前，各消息类型的 `payload_fields` 读取结果为
"配置未提供"，ATOM-17 将**跳过全部周期状态推送**（不输出空 payload 冒充），
仅保留握手、心跳、pong 与错误响应能力；跳过次数计入
`WebSocketServer::skipped_push_count()`。接入后可据此诊断 P3 是否生效。

### P4（确认项）：`ReportWebOnline` 唯一写入方

经用户确认，ATOM-17 是 `IInputReporter::ReportWebOnline` 的唯一写入方。
接入时必须确认 ATOM-16（HTTP）或其他模块不再调用该方法，否则 WEB 在线
状态将出现多写入方竞争。

---

## 3. 装配步骤（Entry.cpp）

### 3.1 依赖关系与声明顺序

`WebSocketServer::Dependencies` 引用 `AppCoordinator`（实现 `IStateReader`
与 `IInputReporter`），因此声明顺序必须为：

```text
AppCoordinator app            // 先声明
WebSocketServer  server       // 依赖 app，后声明（先于 app 析构）
LifecycleAdapter adapter      // 引用 server，最后声明
```

对象生命期满足"被引用方晚于引用方析构"，与 Entry.cpp 既有约定一致。

### 3.2 参考代码

```cpp
#include "atom17_websocket/websocket_server.hpp"
#include "atom17_websocket/lifecycle_adapter.hpp"

// ... AppCoordinator app{runtime, arbitrator, clock}; 之后、app.Start() 之前：

// ATOM-17: WebSocket 只读状态服务
// Dependencies: config_view / clock / state_reader / input_reporter(可空) / transport(空=civetweb)
marshland::atom17::WebSocketServer websocket_server{
    marshland::atom17::WebSocketServer::Dependencies{
        config_store,   // const config::IConfigView&
        clock,          // const app::IMonotonicClock&
        app,            // const app::IStateReader&
        &app,           // app::IInputReporter*（ReportWebOnline 唯一写入方）
        nullptr}};      // transport：默认 CivetWeb

auto websocket_adapter =
    std::make_unique<marshland::atom17::LifecycleAdapter>(websocket_server);

const bool websocket_registered = runtime.addModule({
    marshland::runtime::RuntimeStep::StartHttpAndWebSocket,
    false,  // 可选：WS 状态推送失败不阻断硬件启动（与 ATOM-03 同原则；
            // WEB 控制通道由 HTTP/ATOM-16 承担，WS 只承载状态推送）
    std::move(websocket_adapter)});

if (!websocket_registered) {
    const auto error = runtime.lastRegistrationError();
    std::fprintf(stderr, "[marshland] ATOM-17 registration failed: %s\n",
                 marshland::runtime::toString(error));
    return 1;
}
```

链接侧无需新增内容：`marshland_public`/`marshland_atoms` 聚合已包含
`Marshland::atom_17_websocket`（本模块 CMake 已链接 `Marshland::PublicHeaders`，
civetweb/nlohmann 为 PRIVATE 不泄漏）。

### 3.3 生命周期语义

- `initialize()`：加载并校验全部 WS 配置 → 开始监听 → 启动推送线程；
  任一失败返回 `ModuleInitResult{success=false, error_message=首个失败原因}`。
- `shutdown()`：停推送线程 → 停监听（civetweb 工作线程汇合）→ 清理全部会话；
  幂等。
- 运行期客户端重连不恢复自动流程、不改变执行器状态（模块内无任何控制路径）。

---

## 4. 接入后验证

### 4.1 构建验证（WSL，增量）

```bash
cmake --build cmake-build-ubuntu-arm -j$(nproc)   # 不得 clean，不得全量重编译
```

预期：`atom_17_websocket` 编译零警告，`marshland` 链接通过。

### 4.2 运行验证（ARM 主机或 qemu-aarch64）

| 场景 | 操作 | 预期 |
|---|---|---|
| 握手与订阅 | WS 客户端连接 `ws://<host>:10002/` | 握手成功，进入只读订阅 |
| 心跳 | 每 1000 ms 发 `{"version":"1.0","type":"ping","request_id":"a1","payload":{}}` | 收到同 `request_id` 的 `pong` |
| 心跳超时 | 停止发送任何数据 ≥ 3000 ms | 连接被关闭，`timed_out_session_count` 增加 |
| 状态推送 | 保持连接 | 每 20 ms 收到各类 `*_state` 信封（P3 完成后） |
| 只读拒绝 | 发送任意控制类消息 | 收到 `error`（`code=read_only_channel`），无任何执行器动作 |
| 版本不符 | ping 携带错误 `version` | 收到 `error`（`code=version_mismatch`） |
| 断开隔离 | 多客户端连接，强制断开其一 | 其余客户端推送不受影响 |
| 在线上报 | 首个客户端连入 / 全部断开 | `ReportWebOnline(true/false)` 各一次 |

### 4.3 诊断入口

`WebSocketServer` 提供单调计数：`pushed_message_count()` /
`skipped_push_count()` / `rejected_message_count()` /
`timed_out_session_count()`，以及 `session_count()` / `last_error()`。

---

## 5. 已知边界（接入方须知）

1. **运行行为未实测**：截至本文件编制，仅完成 ARM 交叉编译验证，未在
   ARM 主机或 qemu 上执行；§4.2 为接入后必须执行的验证清单。
2. **无数据源的消息类型**：`mechanism_state` / `motor_state` / `sensor_state`
   的机构、电机、GPIO 数据在 `Atoms/Public` 尚无契约来源；
   `parameter_changed` 依赖 ATOM-15 热加载事件端口（未落地）。这些类型
   即使配置了 `payload_fields` 也会被跳过推送，不输出全 null 占位消息。
   已可推送的 6 类中，无来源字段输出 null（"未接入"语义，
   禁止回填猜测值）：`estop_state.source/confirm_required/recovery_state`、
   `yaw_state.target_angle/cleared_by_main`、`fault` 详细节点的
   `code/message/occurred_at/recovery_state`、`control_arbiter.preempt_reason`
   （无控制源变更事件时）。
3. **`timing.websocket_io_timeout_ms`（200 ms）** 作为单次读写/消息处理的
   设计上限加载并校验，但不映射为 civetweb 会话空闲超时（空闲断开只由
   心跳超时判定，Config.md §12.1）。
4. **错误码取值**（`bad_message`/`version_mismatch`/`read_only_channel`）
   为本模块定义：Config.md §12.1 只规定 error 信封格式，未枚举 code 取值。
   若后续设计补充错误码表，以设计为准修改。
5. **推送线程栈 1 MiB** 为 Config 线程设计值；`thread.*` 中 WebSocket 专用
   键名未在 ConfigSchema 确认，当前为模块内常量，确认键名后可改由
   `IConfigView` 注入。
6. **客户端重连退避**（1/2/4/8/16 s，上限 30 s + 抖动）是客户端策略；
   服务端只负责释放断开连接的资源，模块不实现退避逻辑。
