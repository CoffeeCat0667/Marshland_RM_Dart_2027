# ATOM-17 WebSocket 实现方案

> 更新：2026-10-15。依据用户授权读取的上层文件：
> `NewVersion/CMakeLists.txt`、`CMakePresets.json`、`PLAN.md`、`Entry.cpp`、`README.md`，
> `Design/Config.md`（§4/§10/§12）、`Rule/ConfigSchema.md`（§2.14 等）、`ThirdParty/` 盘点，
> 以及 `Atoms/Public/` 公共契约头（`ILifecycle.hpp`、`IConfigView.hpp`、
> `IMonotonicClock.hpp`、`AppInterfaces.hpp`）。
> 功能权威来源仍为 `Design/Atomic/ATOM-17_WebSocket.md`（只读）；
> 数值唯一来源为 `Design/Config.md`，键名契约为 `Rule/ConfigSchema.md`。

## 1. 目标与边界

ATOM-17 负责建立只读 WebSocket 状态订阅、连接心跳、统一事件封装和实时广播；不负责执行控制命令。任何控制类 WS 消息必须拒绝执行，并提示调用方改用 HTTP（`websocket.control_over_ws_allowed = false`）。

本模块不提供 `main` 入口，不独立决定平台、编译器或编译选项；这些内容由上层 `NewVersion/CMakeLists.txt` 统一决定。目标平台为 Ubuntu ARM64，C++23 严格模式（`-std=c++23`，`CMAKE_CXX_EXTENSIONS OFF`），构建只能经 WSL aarch64 交叉编译或在 ARM 主机上直接编译，且必须增量编译。

## 2. 已确认的上层工程事实

### 2.1 构建契约

- 模块目标名固定为 `Marshland::atom_17_websocket`；有 `Src/` 源码时建 STATIC 库，否则建 INTERFACE 库，上层用同一目标名链接。
- 本目录 `CMakeLists.txt` 不定义 `project()`、不设置编译器/平台/C++ 标准/编译选项。
- 上层要求各模块在 `if(TARGET Marshland::PublicHeaders)` 条件下链接 `Marshland::PublicHeaders`（契约目标在 `add_subdirectory(Atoms)` 之前建立）。**当前本目录 CMakeLists.txt 尚无此链接，落地实现时需补上**（修改前需获得许可）。
- 测试由上层统一组织：`MARSHLAND_BUILD_TESTS=ON` 时，上层在 `Tests/` 下为各 ATOM 建 `marshland_atomNN_tests` 并按用例名注册 CTest（交叉编译时经 qemu-aarch64 运行）。ATOM-17 的测试接入点属于上层 CMake 修改，本模块只保证自身可被这样链接。

### 2.2 生命周期接入

- 公共契约 `marshland::runtime::ILifecycleModule`（`Atoms/Public/ILifecycle.hpp`）是唯一接入点：`initialize(const RuntimeContext&)` / `shutdown() noexcept` / `name()`。
- 启动步骤枚举中已存在 `RuntimeStep::StartHttpAndWebSocket`，ATOM-17 的生命周期适配器注册到该步骤。
- 上层组装模式（见 `Entry.cpp`）：构造核心对象与适配器 → `runtime.addModule({step, required, adapter})` → 检查结果，失败读 `lastRegistrationError()`。本模块只需提供可这样组装的核心类与适配器。
- `RuntimeContext.values` 是无类型字符串字典；配置的类型化查询必须走 `IConfigView`，不从 `values` 自行解析数值。

### 2.3 可用的公共契约（Atoms/Public）

- `marshland::config::IConfigView`：只读配置查询（getInt/getDouble/getBool/getString/数组查询、`configVersion()` 热加载版本、`isLoaded()`）。全部 noexcept，非 OK 状态必须显式处理，禁止"返回默认值+无提示"。键名遵循 `Rule/ConfigSchema.md`。
- `marshland::app::IMonotonicClock`：`nowMicros()`，用于心跳计时、超时判定与推送周期（**不用于**信封 `timestamp`，见 §2.4）。
- `marshland::app::IStateReader`：`ReadState()` 返回持锁借用的 `StateReadView`（不可复制、可移动；持有期间禁止 I/O、禁止调用其他端口、引用不得在视图释放后保留）；`CopyEvents()` 复制状态事件缓冲；`dropped_event_count()`。
- `marshland::app::IInputReporter::ReportWebOnline(bool)`：经用户确认，**ATOM-17 是该输入的唯一写入方**（见 §7）。

### 2.4 WebSocket 配置事实（Config.md §12 / ConfigSchema §2.14，已确认）

- **监听**：`websocket.listen_address = 0.0.0.0`，`websocket.port = 10002`（独立监听，独立于 HTTP 10000 与视觉 10001；已写入 Config.md §12.1 并登记 ConfigSchema §2.14）。
- **消息版本** `websocket.message_version = "1.0"`；主版本变化时必须更新协议版本。
- **心跳是应用层 JSON**，不是协议层 ping/pong 控制帧：客户端每 `websocket.heartbeat_period_ms`（1000 ms）发送 `{"version":"1.0","type":"ping","request_id":"...","payload":{}}`；服务端回复 `pong` 并关联相同 `request_id`。
- **连接超时** `websocket.connection_timeout_ms`（3000 ms）：连续 3 个心跳周期无有效数据判定断开。
- **推送周期** `websocket.push_period_ms`（20 ms）；另有 `timing.websocket_push_period_ms` 与 `timing.websocket_io_timeout_ms`（200 ms，单次读写/消息处理上限）需按 schema 语义取用。
- **断线重连退避**（1/2/4/8/16 s、上限 30 s、随机抖动）是**客户端策略**；服务端只负责释放断开连接的资源。
- **信封**：`version` / `type` / `timestamp` / `request_id` / `source` / `payload`。
  - `timestamp` 为 **UTC ISO 8601 毫秒时间戳**；周期与超时计算才使用单调时钟。
  - `source` 服务端推送统一为 `WEB`，不随事件原始来源变化。
  - **无关联请求的服务端事件 `request_id` 为 `null`**；`error` 消息保留原 `request_id`（Config.md §12.1/§12.2 已定，修正了此前"置空字符串"的设想）。
- **错误消息**：`type=error`，`payload` 含 `code`、`message`、`details`。
- **消息类型表与 payload 字段禁止在代码中硬编码**（ConfigSchema §2.14 禁令）：`websocket.message_types[]` 逐类型给出 `payload_fields`，`websocket.heartbeat_message` 给出心跳信封结构，`websocket.envelope_fields` 给出字段顺序——实现必须从 `IConfigView` 读取这些定义。
- 线程模型：独立线程，`SCHED_OTHER`（非硬实时），线程栈 1 MiB。

### 2.5 消息类型清单（Config.md §12.3）

`system_state`、`mechanism_state`（`charge`/`reload`/`feed`/`force`/`fire`/`yaw`，每项含 `state`/`target`/`feedback`/`error_code`）、`motor_state`（`motors` 数组，纳入两台换弹电机）、`sensor_state`（`gpio_inputs` 数组 + 两个蓄力限位）、`yaw_state`（`target_angle`/`arrived`/`cleared_by_main`）、`fire_progress`、`fault`、`estop_state`、`control_arbiter`、`parameter_changed`、`error`。

换弹 `reload` 的 `target`/`feedback` 分别包含左右电机目标与速度反馈，并上报动作、到位状态及错误；定时到位不得冒充物理位置反馈。

## 3. 目录结构（已实现，2026-10-15）

```text
ATOM-17_WebSocket/
├─ CMakeLists.txt            # 统一模板 + PublicHeaders/Civetweb/nlohmann 链接
├─ PLAN.md                   # 本文件
├─ Inc/
│  └─ atom17_websocket/
│     ├─ ws_config.hpp        # WsConfig / MessageTypeDefinition / LoadWsConfig
│     ├─ ws_snapshot.hpp      # StateSnapshot（StateReadView 租约内按值拷出）
│     ├─ ws_transport.hpp     # IWsTransport 传输抽象 + CreateCivetwebTransport
│     ├─ websocket_server.hpp # WebSocketServer 核心（pimpl，无第三方类型）
│     └─ lifecycle_adapter.hpp# ILifecycleModule 适配器
└─ Src/
   ├─ ws_config.cpp           # IConfigView 全量读取与语义校验
   ├─ message_builder.hpp/.cpp# 信封/payload 构造（nlohmann 仅在此）
   ├─ civetweb_transport.cpp  # civetweb 实现（civetweb.h 仅在此）
   ├─ websocket_server.cpp    # 会话表、心跳、超时、推送线程、只读拒绝
   └─ lifecycle_adapter.cpp
```

第三方库类型不进入 `Inc/`；`WsTransport` 抽象使测试可注入替身而不依赖真实网络。

## 4. 组件设计

### 4.1 订阅与连接管理

- `WebSocketServer`：在 `websocket.listen_address:websocket.port` 独立监听、接受握手、创建只读订阅、维护客户端集合。
- `WebSocketSession`：保存连接标识、最近有效数据时间、订阅资源和发送接口。
- `SubscriptionRegistry`：广播事件到所有仍有效的只读客户端。
- 会话数变化驱动 `IInputReporter::ReportWebOnline`：首个有效会话建立 → `true`，最后一个会话断开/超时 → `false`。本模块只搬运事实，不做仲裁。
- 断开或心跳超时后移除会话并释放对应订阅资源；单个客户端故障不得影响其他客户端或硬件控制流程。
- 提供 `LifecycleAdapter`（实现 `ILifecycleModule`），注册到 `RuntimeStep::StartHttpAndWebSocket`；`shutdown()` 幂等，不得持锁 join 自身工作线程。

### 4.2 心跳

- 应用层 JSON ping/pong：校验客户端 `ping` 信封（版本、类型、request_id），回复相同 `request_id` 的 `pong`；心跳信封结构以配置 `websocket.heartbeat_message` 为准。
- 任何有效入站数据都刷新会话的最近活动时间；连续 3 个心跳周期（`connection_timeout_ms`）无有效数据判定断开并清理。
- 心跳只维持连接，不改变执行器状态、不授予控制权限。
- 计时用 `IMonotonicClock`；周期、超时等参数全部经 `IConfigView` 读取，读取失败按 `ConfigStatus` 显式处理，不硬编码兜底值。

### 4.3 消息模型与状态采集

- 信封字段顺序以 `websocket.envelope_fields` 为准；`timestamp` 由**墙钟**生成 UTC ISO 8601 毫秒字符串（需一个系统时间源，与 `IMonotonicClock` 分工：前者给信封时间戳，后者管周期/超时）。
- 消息类型集合与各类型 `payload_fields` 从配置读取，不在代码中硬编码消息类型表或 payload 字段（ConfigSchema §2.14 禁令）。
- 状态采集遵守 `StateReadView` 借用约束：读取租约内只把所需字段**按值拷出**，JSON 序列化与网络发送一律在锁外完成；禁止把视图内引用交给推送线程保存。
- 推送线程独立，周期 20 ms（依 Config），`SCHED_OTHER`，栈 1 MiB；单次读写/消息处理不超过 `timing.websocket_io_timeout_ms`（200 ms）。
- 序列化使用 `ThirdParty/nlohmann/json.hpp`（已在库）。

### 4.4 控制隔离

收到控制类 WebSocket 消息时，返回 `type=error` 信封（`payload` 含 `code`/`message`/`details`，保留原 `request_id`），明确提示改用 HTTP；不调用任何执行器或控制流程。客户端重连不恢复自动流程、不改变执行器状态。

## 5. 依赖与接口策略

1. 公共依赖仅 `Marshland::PublicHeaders`（`IConfigView` / `IMonotonicClock` / `IStateReader` / `IInputReporter` / `ILifecycleModule` 契约），不反向包含任何其他 ATOM 的私有头。
2. 网络传输经适配接口隔离，第三方库类型不得泄漏到 `Inc/` 公共头文件；序列化用 nlohmann/json（已在 ThirdParty）。
3. Config、时钟、状态读取、在线上报通过构造函数注入接口指针/引用。
4. 本模块只声明自身依赖，不修改上层 CMake、设计文件或其他上层目录文件（Config.md/ConfigSchema.md 的本次端口登记为已授权例外）。
5. CMake 当前为 INTERFACE 占位；`Src/` 落地后按模板自动切换为 STATIC，上层链接名不变。

## 6. 实现状态（2026-10-15）

- [x] CivetWeb 接入 ThirdParty（见 §7.2）。
- [x] 创建 `Inc/`、`Src/`；`Marshland::PublicHeaders`（PUBLIC）与 Civetweb/nlohmann（PRIVATE）链接已补入本目录 CMakeLists。
- [x] 会话生命周期、应用层 JSON ping/pong、心跳超时清理、只读权限隔离。
- [x] 配置驱动的消息类型/payload 定义读取、统一信封、"拷出→锁外序列化→广播"推送管线。
- [x] `ReportWebOnline` 上报（唯一写入方，≥1 会话 true / 0 会话 false）。
- [x] WSL aarch64 增量编译验证：`atom_17_websocket` 编译零警告，全工程（含 `marshland` 与全部测试目标）链接通过。
- [ ] ATOM-15 覆盖 `getNestedArraySize/getNestedStringAt`（否则各类型 `payload_fields` 读取为 MISSING，payload 无字段定义，按"未提供"处理）。
- [ ] `Rule/ConfigSchema.schema.json` 同步 `listen_address`/`port`（其 websocket 段 `additionalProperties:false`，未同步会使含新键的配置校验失败；Rule/ 文件，需用户授权）。
- [ ] 上层装配：Entry.cpp 注册 ATOM-17 到 `RuntimeStep::StartHttpAndWebSocket`（属上层改动，需授权）。
- [ ] 上层测试接入（Tests/ 归上层负责）。

### 6.1 已实现行为要点

- 监听 `websocket.listen_address:websocket.port`（`0.0.0.0:10002`），任意路径握手即创建只读订阅；协议层 ping 直接回 pong（与应用层 JSON 心跳互不替代）。
- 客户端消息：合法 `ping`（版本匹配）→ 回相同 `request_id` 的 `pong`；版本不符 → `version_mismatch`；非 JSON/缺字段 → `bad_message`；其余一切消息 → `read_only_channel`（提示改用 HTTP），均不执行任何动作。
- 推送：每 `push_period_ms` 拷出快照，对配置中有数据源的 6 类（`system_state`/`fire_progress`/`control_arbiter`/`estop_state`/`fault`/`yaw_state`）序列化广播；无数据源的 4 类（`mechanism_state`/`motor_state`/`sensor_state`/`parameter_changed`）跳过并计入 `skipped_push_count()`，不输出全 null 占位消息。无数据源字段输出 null（"未接入"语义）。
- 诊断计数：`pushed_message_count` / `skipped_push_count` / `rejected_message_count` / `timed_out_session_count`。

### 6.2 Public 契约变更（2026-10-15，非破坏）

`Atoms/Public/IConfigView.hpp` 新增两个**带默认实现**的虚方法（`getNestedArraySize` / `getNestedStringAt`），用于读取对象数组元素内的嵌套数组（如 `websocket.message_types[].payload_fields`）；默认返回 MISSING，ATOM-15 无需立即修改即可继续编译运行，覆盖后 ATOM-17 自动获得 payload 字段定义。已验证全工程编译链接不受影响。

## 7. 裁决记录（2026-10-15，原"待确认事项"已全部关闭）

1. **端口与监听器**：独立监听 `0.0.0.0:10002`。用户最初提议 10001，因与视觉（`vision.port = 10001`）冲突改定 10002。已写入 `Design/Config.md` §12.1，并登记 `Rule/ConfigSchema.md` §2.14（`websocket.listen_address` / `websocket.port`）及示例 JSON。
2. **网络库**：`ThirdParty/` 盘点结果仅有 httplib（无 WS 服务端能力）、nlohmann/json、pboettch（JSON Schema）。选定 **CivetWeb v1.16**（MIT，C 实现，含 WS 服务端 API）。已完成接入（2026-10-15，经用户授权）：目录裁剪至 `LICENSE.md`、`README.md`、`include/civetweb.h`、`src/civetweb.c` + 15 个 `.inl`；为本工程重写 `ThirdParty/civetweb/CMakeLists.txt`（替代上游构建文件，源码未改），产出静态库 `Marshland::ThirdPartyCivetweb`，编译定义为 `USE_WEBSOCKET`（v1.16 的 WS 开关，默认关闭）、`NO_SSL`（明文 ws://，去除 OpenSSL 依赖）、`NO_CGI`；因上层 `project()` 仅声明 CXX，文件内从 CXX 编译器推导同前缀 C 交叉编译器后 `enable_language(C)`。已在 `ThirdParty/CMakeLists.txt` 末尾 `add_subdirectory(civetweb)`。WSL aarch64 增量编译验证通过：`civetweb.c.o` 为 ARM aarch64 ELF，全工程（含 `marshland` 与各测试目标）链接正常。
3. **配置键名**：已读取 `Rule/ConfigSchema.md` §2.14 与 `Design/Config.md` §12，键名与语义以两处文档为准（见 §2.4）。
4. **在线状态上报**：确认由 ATOM-17 作为 `IInputReporter::ReportWebOnline` 唯一写入方（≥1 有效会话为 true，0 会话为 false）。
5. **request_id**：采纳"客户端给、服务端回显"；并依 Config.md §12.1/§12.2 定案：无关联请求的服务端事件 `request_id` 为 `null`，`error` 保留原 `request_id`。

## 8. 验收对照

- 所有推送消息包含统一信封和版本字段（"1.0"），字段顺序与消息类型/payload 定义来自配置。
- 应用层 JSON ping/pong 关联相同 `request_id`；连续 3 个心跳周期无有效数据判定断开并清理会话。
- 信封 `timestamp` 为 UTC ISO 8601 毫秒；服务端主动推送 `request_id` 为 `null`；`error` 保留原 `request_id` 且 `payload` 含 `code`/`message`/`details`。
- WebSocket 始终是只读通道，控制命令不会执行，只返回改用 HTTP 的 `error` 提示。
- 一个客户端断开不影响其他客户端和硬件控制流程；会话数变化正确驱动 `ReportWebOnline`。
- 配置项全部经 `IConfigView` 读取；客户端重连不恢复自动流程、不改变执行器状态。
- 推送线程遵守 20 ms 周期（依 Config），序列化与发送在状态锁外完成。
- 适配器可经 `RuntimeStep::StartHttpAndWebSocket` 注册并被上层 Entry 模式组装。
