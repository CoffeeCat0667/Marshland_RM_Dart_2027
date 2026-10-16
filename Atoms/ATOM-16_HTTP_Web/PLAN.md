# ATOM-16 HTTP/Web 实现方案（v3.2，依赖选型全部确定）

> 依据：`Design/Atomic/ATOM-16_HTTP_Web.md`、`Design/{PRD,Interface,Config,PEF,Atoms,LifecycleEvents}.md`、`Atoms/Public/` 全部公共头文件与 AppCoordinator 实现（经用户授权只读一次）。
> 变更记录：v2 完成设计对齐；v3 确定 HTTP/JSON 库选型方向，记录 auto/stop 文档冲突报告，下游接口与前端设为 TODO/调研项；v3.1 JSON 改 header-only 方向；v3.2 确定 JSON 库为 nlohmann/json，第三方库存放于上层共享目录 `NewVersion/ThirdParty`；v3.3 读入 `Rule/ConfigSchema.md`（用户授权），按规范键名落地首批源码（PLAN §6 第 1~5 步），新增 §7.5 缺口清单；v3.4 全量重读（2026-10-15），记录四项裁决（§8），前置阻塞登记至 `Public/GeneralBugs/2026-10-15-ATOM16.md`。

## 1. 目标与边界

实现 ATOM-16 的 HTTP 服务与 Web 前端交互模块（Interface §5.1 中的 `WebAdapter`）：监听 Config 指定地址/端口，提供静态资源与 JSON API，把每个请求分派为领域命令或查询，统一生成 Config §11.3 约定的成功/错误 JSON 响应。

边界红线：

- 不直接操作硬件，不做安全判断；所有控制请求经状态机、ControlArbiter、SafetyManager 三层（ATOM-16 文档安全条件）。
- 无 `main` 入口；自身是挂接在 `RuntimeStep::StartHttpAndWebSocket` 上的生命周期模块（`ILifecycle.hpp`）。
- 写/控制路径全部归本模块；实时状态推送（WebSocket，只读）归 ATOM-17，本模块不实现 WS。控制语义一律走 HTTP。
- 本模块只做 HTTP；前端静态资源采用多文件目录形态（已决策），但本期不实现，继续调研（见 §7.4）。

## 2. 与 Public 公共接口的对接（已确认的真实契约）

命名空间与头文件均来自 `Atoms/Public/`（`#include "Xxx.hpp"` 直接可用，链接 `Marshland::PublicHeaders` 即可；**不链接 `marshland_app`**，接口由装配层注入）。

### 2.1 本模块需实现

- `marshland::runtime::ILifecycleModule`（`ILifecycle.hpp`）：
  - `ModuleInitResult initialize(const RuntimeContext&)` — 读配置、绑定监听、启动 HTTP 服务线程；
  - `void shutdown() noexcept` — 停服务、释放监听；
  - `const char* name() const noexcept`。
  - 初始化失败时按 `required` 语义返回失败，由 RuntimeCoordinator 决定是否 `ROLLBACK_BEGIN`；不支持运行中热重启（LifecycleEvents §3 不变量 4）。

### 2.2 本模块需注入（构造函数注入的抽象接口）

| 接口（头文件） | 用途 |
|---|---|
| `marshland::config::IConfigView`（IConfigView.hpp） | 读监听地址/端口、请求体上限、超时、静态资源目录、action 表等全部 Config §11 配置；`configVersion()` 判断热加载 |
| `marshland::app::IStateReader`（AppInterfaces.hpp） | `/api/state`、`/api/diagnostics` 数据源：`ReadState()`（`StateReadView` 持锁读租约，含 `ArbitrationContext`、`run_gate()`、`current_shot()` 等）、`CopyEvents()` |
| `marshland::app::IControlAuthority`（AppInterfaces.hpp） | 全部控制写路径：`SubmitIntent` / `SubmitStopFireIntent` / `RequestModeTransition` / `RequestAutoFireState` / `BeginNewAutoFireRun` / `AuthorizeFire` / `AuthorizeManualFire` |
| `marshland::app::IInputReporter`（AppInterfaces.hpp） | `ReportWebOnline(bool)` 上报 WEB 客户端连接状态（控制源选择依赖它：有 WEB 连接 → 控制源 WEB）；`ReportStopFire` |
| `marshland::app::IMonotonicClock`（IMonotonicClock.hpp） | 构造 `ControlIntent::timestamp_us`；禁止墙上时钟（Interface §3.3） |
| `marshland::can::ICanNodeStatusReader`（CanInterfaces.hpp，可选） | 诊断 API 中 CAN 总线/节点状态 |
| `marshland::sbus::ISbusSnapshotReader`（SbusInterfaces.hpp，可选） | 诊断/状态 API 中 SBUS 在线与通道快照 |

### 2.3 序列化可直接复用的类型

- `MarshlandTypes.hpp`：`SystemMode`（11 态）/`ControlSource`（4 项）/`AutoFireState`（15 态）/`FaultSeverity` 及其 `ToString()`；`ControlSourceRank()`（SBUS=3 > WEB=2 > AUTO=1 > NONE=0）。
- `StateArbitrationTypes.hpp`：`ControlIntent`（含 `request_id`、`timestamp_us`）、`IntentDecision`、`ModeTransitionResult`、`FireAuthorization`、`RejectReason`、`StateEvent`——控制响应与错误码映射的直接来源。
- `AppInterfaces.hpp`：`RunGateReason`（`ToString` 可用）。

## 3. HTTP 服务规格（Config §11，已确认数值）

### 3.1 服务配置（§11.1）

- 监听 `0.0.0.0:10000`；静态资源由主程序提供；UTF-8。
- 请求体上限 **64 KiB** → 超限 HTTP 413；响应体上限 **1 MiB** → 超限 HTTP 500 并记录错误。
- 请求处理超时 **200 ms** → HTTP 408；长耗时控制立即返回 `accepted` + `request_id`，不同步等待。
- 无登录/权限等级/普通操作二次确认；**软件急停恢复必须人工确认**。
- 网络任务优先级为低，不得阻塞控制和安全任务（Config §15）→ HTTP 线程独立、低优先级，锁内禁止 I/O（`StateReadView` 约束）。

### 3.2 路由表（§11.2，9 条，全部实现）

| 方法 | 路径 | 下游 | 说明 |
|---|---|---|---|
| GET | `/api/state` | `IStateReader::ReadState()` | 始终可读；字段含 `system_mode`/`control_source`/`auto_fire_state`/`current_shot`/`total_shots`/`stop_fire`/`estop_active`/`active_faults` |
| GET | `/api/diagnostics` | `IStateReader` + ATOM-18 诊断快照（+可选 CAN/SBUS reader） | 始终可读；`active_faults`/`fault_history`/`recovery_state`/`interfaces` |
| POST | `/api/control/auto/start` | ATOM-13 自动流程（经 `IControlAuthority`） | 应急链路；仅待机且无急停/故障/高优先级接管；立即返回 accepted + request_id |
| POST | `/api/control/auto/stop` | `ReportStopFire` / 控制服务 | 应急链路；**可用性条件两文档冲突，待裁决（见 §7.2）** |
| POST | `/api/control/manual` | 按 action 分派（见 §3.4） | 仅接受 Config 11.4 action 白名单 |
| POST | `/api/control/estop` | ATOM-14 软件急停 | **始终可用**，不受普通控制优先级阻挡 |
| POST | `/api/control/estop/confirm` | ATOM-14 人工确认（`confirm` 必须为 true） | 仅急停锁定状态；确认后进入安全恢复，不重新初始化 |
| GET/POST | `/api/parameters` | ATOM-15 参数查询/更新 | PID 修改仅 WEB 控制源 + 手动/调试模式；响应必须分别携带 `effective` 与 `persisted` 结果 |
| POST | `/api/calibration` | ATOM-19 校准（`action`+`target`） | 无模式前提；无急停/活动故障时可用，调用即进入 CALIBRATION 模式 |

禁止项：不新增故障清除 HTTP 路由（Config 14.2/ATOM-14）；不新增独立模式切换路由（Config §11.2，未注册模式路由返回错误）；不暴露换弹校准接口（ATOM-19）；静态资源请求不触发任何机构动作。

### 3.3 统一响应结构（§11.3）

- 成功：`{"request_id":"...","ok":true,...}`；错误：`{"request_id":"...","ok":false,"error":{"code":"...","message":"...","details":{}}}`。
- 错误码集合（映射 400/408/409/413/422/500/503，未知→500）：`INVALID_JSON`、`INVALID_ARGUMENT`、`INVALID_STATE`、`CONTROL_PREEMPTED`、`STOP_FIRE_ACTIVE`、`ESTOP_ACTIVE`、`FAULT_ACTIVE`、`NOT_INITIALIZED`、`NOT_FOUND`、`VALUE_OUT_OF_RANGE`、`PAYLOAD_TOO_LARGE`、`CONFIGURATION_ERROR`、`DEPENDENCY_UNAVAILABLE`、`REQUEST_TIMEOUT`、`INTERNAL_ERROR`。
- 所有请求携带 `request_id` 与控制源标识（WEB），贯通 `ControlIntent` 与日志（Interface §9.2/§19）。
- `RejectReason`/`RunGateReason` → 错误码的映射表在本模块集中实现（如 `SOFTWARE_ESTOP`→`ESTOP_ACTIVE`/`409`，`LOWER_PRIORITY_CONFLICT`/`NO_CONTROL_AUTHORITY`→`CONTROL_PREEMPTED` 等）。

### 3.4 手动控制 action 白名单（§11.4，14 个，唯一来源）

| action | target | 允许模式 | 分派与约束 |
|---|---|---|---|
| `charge_start` / `charge_stop` | 无 | 手动、调试 | 蓄力机构；受 SBUS 优先级/急停/故障限制 |
| `charge_home` | 无 | 手动、调试 | 蓄力双电机回零 |
| `reload_lift` / `reload_lower` | 无 | 手动、调试 | 双 M3508 定时运行后保持 0 rpm（Config §5.7；11.4 表中"5.6"为笔误） |
| `feed_once` | 无 | 手动、调试 | 供弹舵机 `FeedOnce()` |
| `force_set` | 数值 rpm | 手动、调试 | 变力机构；范围 ±15000（Config §5.6） |
| `yaw_set` | 数值 rpm | 手动、调试 | 手动 YAW 目标为 rpm；服从 `SBUS > WEB > 自动流程` 仲裁 |
| `servo_set` | 角度+舵机编号 | **仅调试** | 仅供弹/发射舵机，不能控制换弹双电机；急停时发射舵机一律拒绝 |
| `fire_once` | 无 | 手动、调试 | 经 `AuthorizeManualFire` 复核全部发射前置条件 |
| `stop_fire_set` | 无 | 手动、调试 | 置位后不发射、不开下一发 |
| `stop_fire_clear` | 无 | 手动、调试 | 执行前须退出自动模式转手动；清除标志并**终止**当前自动流程（不暂停、不恢复） |
| `auto_start` | 无 | 待机 | 同 `/api/control/auto/start`，返回 accepted + request_id |
| `auto_stop` | 无 | 任意普通模式 | 终止自动流程，不暂停不恢复 |

不得接受白名单之外的隐含 action；每个 action 分别校验 target、允许模式、控制权、安全状态后再分派。

## 4. 技术选型（v3.2：全部确定）

两个第三方库均为 header-only：vendor 后无第三方编译单元，CMake 只需 include 路径，交叉编译零风险。

### 4.1 cpp-httplib（HTTP 服务）—— 已定

- 单头文件 header-only（`httplib.h`），MIT 许可证，C++11 起，跨平台（Linux/aarch64 无碍）。
- 功能覆盖本模块全部需求：`set_payload_max_length`（对应 64 KiB → 413）、`set_read_timeout`/`set_write_timeout`、`set_mount_point` 静态资源目录、路由注册、keep-alive。
- 线程模型为阻塞 I/O + 每连接线程：与"网络任务低优先级、不得阻塞控制/安全任务"（Config §15）兼容——HTTP 处理全部发生在自有线程池内，handler 内只做非阻塞接口调用；handler 超时（200 ms → 408）需在分派层自行实现（httplib 的超时是 socket 读写超时，不是 handler 执行超时）。
- 已知 CVE（如 CVE-2026-28435，0.35.0 前 streaming+gzip 绕过 payload 上限）：锁定时选用 ≥0.35.0 的版本，且不启用 gzip 请求体解压路径。

### 4.2 nlohmann/json（JSON 编解码）—— 已定（用户确认）

- 单头 `json.hpp`，MIT 许可证，C++11+，生态最大最稳。
- 用非异常解析 API（`json::parse(text, nullptr, false)` + `is_discarded()`）映射 `INVALID_JSON`，避免异常进入控制流；响应序列化用其 `dump()`。
- 规模前提：请求体 ≤ 64 KiB、响应 ≤ 1 MiB、200 ms 预算，性能完全富余；这是放弃 simdjson（非 header-only）的原因。
- 备选记录：glaze（header-only、反射映射、性能强，但项目年轻、编译器要求激进）；RapidJSON 已排除（维护停滞）。

### 4.3 第三方代码放置 —— 已定并落地（用户指定）

- 位置：**上层共享目录 `D:\Project\C++\RM2027\Cocoon-Dart2027\NewVersion\ThirdParty`**（用户明确指定，作为对"禁止写入上层目录"规则的专项授权，仅限该目录）。
  - 实际布局：`ThirdParty/httplib/httplib.h`（**0.61.0**，两个 CVE 均已修复）、`ThirdParty/nlohmann/json.hpp`（**3.12.0**）。
  - ⚠ LICENSE 文件尚未随库放入，已提醒用户补充（MIT×2）。
- 集成方式（不改动上层 `CMakeLists.txt`，其对本模块只读）：
  - `ThirdParty/CMakeLists.txt`（已建）：定义 `marshland_thirdparty_httplib` / `marshland_thirdparty_nlohmann_json` 两个 INTERFACE 目标（别名 `Marshland::ThirdPartyHttplib` / `Marshland::ThirdPartyNlohmannJson`），以 SYSTEM 方式导出 ThirdParty 根为 include 根（消费方写 `#include "httplib/httplib.h"`、`#include "nlohmann/json.hpp"`），含缺文件 FATAL_ERROR 检查；
  - 本模块 `CMakeLists.txt`（已接线）：目标不存在时兜底 `add_subdirectory(../../ThirdParty)`；STATIC 时 PRIVATE 链接（INTERFACE 占位期用 INTERFACE），第三方头只在 `Src/` 内包含，不随对外接口泄漏；
  - 若上层将来统一 `add_subdirectory(ThirdParty)`，本模块的 `if(NOT TARGET ...)` 守卫自动跳过，无需改动。
- 风险记录：上层 `ThirdParty` 由谁维护、多模块共用时的版本一致性，属上层工程治理问题，已提示用户。

## 5. 计划的模块结构

- `Inc/`（对外头文件）：
  - `HttpWebModule.hpp` — `ILifecycleModule` 实现类声明，构造函数接收 §2.2 注入接口；
  - `Router.hpp` / `Routes*.hpp` — 路由表与 9 条路由 handler；
  - `JsonCodec.hpp` — 基于 nlohmann/json 的请求解析与响应序列化（含枚举 `ToString` 桥接；对内隔离第三方头，对外头文件不暴露 JSON 库类型）；
  - `ErrorMapping.hpp` — `RejectReason`/`RunGateReason`/`ConfigStatus` → 错误码/HTTP 状态映射；
  - `ManualControlActions.hpp` — action 白名单表、参数与模式校验、分派；
  - `StaticFiles.hpp` — 静态资源服务（httplib mount point 封装；不触发机构动作）。
- `Src/`：对应实现；handler 分派超时（200 ms）、64 KiB/1 MiB 限制、accepted+request_id 异步受理。第三方头只在 `Src/` 内包含，配合 PRIVATE include。
- `Web/`：前端多文件目录（本期不实现，见 §7.4）。
- `CMakeLists.txt`：已存在，INTERFACE 目标 `atom16_http_web`；后续按 Public 模板追加 `if(TARGET Marshland::PublicHeaders) target_link_libraries(... Marshland::PublicHeaders)`、源文件收集与 §4.3 的第三方 include。

## 6. 功能实现顺序

1. 第三方库接入（向上层 `ThirdParty` 放置 httplib.h 与 json.hpp，需下载对应版本）+ 请求/响应/路由/错误码映射等内部抽象 + JSON 编解码封装。
2. `HttpWebModule` 生命周期骨架：`initialize` 读 `IConfigView` 绑定 `0.0.0.0:10000`，`shutdown` 停服务；接入 `StartHttpAndWebSocket` 步骤。
3. HTTP 监听与连接处理、请求体 64 KiB 上限（413）、handler 200 ms 超时（408）。
4. JSON 解析失败统一错误、统一成功/错误序列化、`request_id` 生成与贯通。
5. `GET /api/state`、`GET /api/diagnostics`（只读路径，先打通 `IStateReader`）。
6. `POST /api/control/manual`：14 个 action 白名单、target/模式/控制权校验、经 `IControlAuthority`/`IInputReporter` 分派。
7. 急停两路由（始终可用 + 人工确认）、`auto/start` 应急链路与 accepted+request_id；`auto/stop` 待 §7.2 裁决后实现。
8. `/api/parameters`（含 PID 的 WEB+手动/调试授权与 `effective`/`persisted` 双结果）、`/api/calibration`。
9. `ReportWebOnline` 连接状态上报与断线行为（断线不启动/恢复/继续已终止自动流程）。
10. （暂缓）静态前端：多文件目录、状态展示（数据源以 ATOM-17 推送为主，HTTP 轮询兜底）、控制提交、结果反馈、受限 PID 编辑（仅 WEB+手动/调试显示可编辑）。

## 7. 待定项状态（v3.2 更新）

### 7.1 HTTP/JSON 库选型 —— 已全部决策并落地

- HTTP：cpp-httplib **0.61.0**（实际入库版本，两个 CVE 均已修复）。
- JSON：nlohmann/json **3.12.0**（实际入库版本）。
- 放置：上层共享目录 `NewVersion/ThirdParty`，`ThirdParty/CMakeLists.txt` 已建（两个 INTERFACE 目标 + 别名），本模块 `CMakeLists.txt` 已兜底接线（§4.3）。

### 7.2 `/api/control/auto/stop` 可用性冲突 —— 已裁决（2026-10-15）：始终可用

**裁决结果**：`POST /api/control/auto/stop`（置停止发射标志）**始终可用**，不受急停/故障/优先级阻挡。佐证：`StateArbitrationTypes.hpp` 中 `IntentKind::STOP_FIRE_SET` 注释"安全单调：只收紧，不受优先级阻断"。**文档待同步**：`Design/Config.md:476`（及 :485、:502）需修订为"始终可用"（记录于 `Public/GeneralBugs/2026-10-15-ATOM16.md` BUG-08）。

<details><summary>冲突原文存档（裁决依据）</summary>

- Interface.md:680（§14.2 路由表）：`| POST | /api/control/auto/stop | 设置停止发射 | WEB | 始终可用 |`
- Config.md:476（§11.2 路由表）：`应急链路；无急停、活动故障或更高优先级控制接管时可用`
- Config.md:485：`链路分工：…… 专用路由是应急链路（无急停、故障或更高优先级接管）；两条链路同时存在。`

</details>

### 7.3 下游接口缺口 —— TODO（本期不做）

ATOM-13/14/15/18/19 的 C++ 接口尚未在 Public 定义。当前所有分派只能经 `IControlAuthority`/`IInputReporter`/`IStateReader`；诊断快照、参数写入、校准命令等缺具体接口。

- TODO：待对应 Atom 或装配层桥接落地后，补齐 `/api/diagnostics` 完整字段、`/api/parameters` 写路径、`/api/calibration` 分派。
- 实现策略：分派层做成薄适配（每路由一个 handler + 一个下游接口），下游接口出现后以最小改动替换桩实现。
- 桩实现期间，未接通的下游返回 `DEPENDENCY_UNAVAILABLE`（503），不得谎报成功。

### 7.4 前端静态资源 —— 演示骨架已落地，接口未接

- 已定：**多文件目录**形态（非单文件内嵌），静态资源由本模块经 httplib mount point 提供。
- **演示骨架 v3 已完成**（浅色简约风重做 + 纵向摇杆，已经浏览器实际渲染与拖拽验证）：
  - `Web/index.html` + `Web/assets/css/style.css` + `Web/assets/js/main.js`；
  - 浅色简约风：白底卡片、细边框、轻阴影、青灰主色，动效收敛为轻量过渡；
  - **摇杆速度控制面板**：蓄力/变力/YAW/换弹四个**仅纵向**虚拟摇杆（竖向胶囊轨道，水平方向锁定），纵向偏移量→速度**线性映射** `value = offset / radius × max`，范围统一 **±5000 rpm**（上推为正）；松手弹簧回中归零、实时读数（正绿/负红）+ 中线线性条；Pointer Events 实现，触屏可用；
  - 仍为演示模式：状态区 mock 驱动，摇杆未接真实接口（口后接 `/api/control/manual` 的 `force_set`/`yaw_set` 等 action）；
- 仍未做（调研清单）：
  1. 真实接口接入：`/api/state` 轮询兜底 + ATOM-17 WebSocket 推送的状态管理；
  2. Config 中静态资源根目录的 key 名与目录约定（schema 中 `http.static_resource_provider=MAIN_PROGRAM`，无目录键，待补）；
  3. 缓存策略与 `Cache-Control` 头、404/回退行为约定；
  4. PID 编辑区权限联动 UI（仅 WEB + 手动/调试可编辑）、校准界面、急停确认交互；
  5. 手动控制按钮与 `/api/control/manual` action 集的完整映射（含 `force_set`/`yaw_set`/`servo_set` 数值输入）。

### 7.5 配置 schema 落地后新发现的缺口（v3.3）

读入 `Rule/ConfigSchema.md`（用户授权单文件）后确认：

1. **HTTP 键名已定**（§2.12.1）：`http.listen_address`/`http.port`/`http.request_body_limit_bytes`/`http.response_body_limit_bytes`/`http.request_timeout_ms` 等，已在 `Inc/ConfigKeys.hpp` 逐字落地。
2. **路由表与错误码表必须配置驱动**：`http.routes[]`（含 `request_fields`/`response_fields`/`status_restriction`/`is_emergency_path`）、`http.error_response_template`、`http.error_codes`、`http.status_mapping`、`http.manual_actions[]`（§2.12.2/§2.12.3）"必须入 JSON、禁止代码另行硬编码"。**但 `IConfigView` 只能查询标量与数组元素的标量字段，无法查询 object**（`error_response_template`/`status_mapping` 均为 object），`manual_actions[].allowed_modes` 是元素内的嵌套数组同样无法查询。**待裁决**：扩展 IConfigView、约定扁平化键（如 `http.status_mapping.INVALID_JSON`）、或由 ATOM-15 提供专用查询。过渡实现：`ApiError.hpp` 的错误码表按 Config.md:492 硬编码并注明待定。
3. **NOT_FOUND 状态码**：Config.md:492 列出的映射集合（400/408/409/413/422/500/503）不含 404；`ApiError.hpp` 中 NOT_FOUND→404 为提议值（未知路径遵循 HTTP 语义），待澄清。
4. **急停确认通道缺口**：`ISafetyStateControl`（ConfirmSoftwareEstop/ReleaseSoftwareEstopLatch）注释明确"仅注入安全管理器"，则 `/api/control/estop/confirm` 的下游通道不存在于 Public 接口中，待装配层或 ATOM-14 补充。
5. **`total_shots`**：`/api/state` 响应的 total_shots 当前硬编码 4（Interface §6.6 固定四发）；如 baseline 域有对应键（ConfigSchema `baseline.*`），应改为配置读取。
6. **日志上报**：响应体超限"记录错误"（Config.md:462）需要 ATOM-18 日志接口，当前无注入通道，代码中已留 TODO。

## 8. 裁决记录（2026-10-15 用户裁决，全量重读后）

1. **桥接方式**：ATOM-16 在自有 `Inc/` 定义窄接口（蓄力/换弹供弹/发射/舵机/急停确认/参数写入各一），装配层在 Entry.cpp 写适配器桥接到机构模块；**ATOM-16 不包含任何模块私有头**。
2. **auto/stop**：始终可用（见 §7.2）。
3. **http 配置节**：由用户按 `Public/GeneralBugs/2026-10-15-ATOM16.md` §3 草稿补入 `dart_control.json`；补齐前 ATOM-16 保持 fail-closed（缺配置即初始化失败）。
4. **web_online**：ATOM-16 不上报；由 ATOM-17（WS 长连接+心跳）唯一上报。

全量重读发现的 11 项前置阻塞/缺陷（归属其他方）已登记：`Atoms/Public/GeneralBugs/2026-10-15-ATOM16.md`（含汇总表、归属索引、逐条细节、配置草稿）。

## 9. 必须落实的安全约束

- 所有控制请求经状态机、ControlArbiter、SafetyManager；本模块自身不做安全判断、不直接操作硬件。
- 发射授权严格 `SBUS > WEB > 自动流程`；`fire_once` 前经 `AuthorizeManualFire` 复核急停/故障/stop_fire/yaw/优先级。
- 急停、故障、模式冲突或高优先级抢占时拒绝普通 WEB 请求；`CONTROL_PREEMPTED`/`ESTOP_ACTIVE`/`FAULT_ACTIVE` 等错误按 PEF 规则返回。
- 软件急停（`/api/control/estop`）始终可用；急停锁定期拒绝一切发射舵机普通命令与全部普通执行器命令；解除必须人工确认（`confirm:true`），不自动解除。
- WEB 断线不得启动、恢复或继续已终止的自动流程；`stop_fire_clear` 仅手动/调试模式、先退自动模式、清除即终止当前自动流程。
- PID 更新仅 WEB 来源且手动/调试模式；越界严格按 Config 报错（`VALUE_OUT_OF_RANGE`），不自动钳位；`effective`/`persisted` 不一致时如实分别上报。
- 请求处理不阻塞控制/安全任务；`StateReadView` 持锁期间禁止 I/O。
- 校准/调试执行命令在急停、故障锁定、未初始化状态下拒绝；调试查询只读，不获得控制权。
- 未接通下游的桩路由返回 `DEPENDENCY_UNAVAILABLE`（503），不得谎报成功（§7.3）。

## 10. 验证方案

- 编译验证：由上层 CMake 在允许的 Ubuntu ARM/aarch64 环境（WSL 交叉编译或目标主机编译）完成；本模块不配置独立架构和入口。第三方头文件库（httplib + nlohmann/json）需在该环境验证编译。
- 单元测试：请求体上限（413）、handler 超时（408）、JSON 解析错误、action 白名单拒绝、target/模式/权限校验、`RejectReason`→错误码映射、响应序列化、`request_id` 贯通、`effective`/`persisted` 双结果、桩路由 503。
- 集成测试：监听地址/端口、静态资源不触发机构动作、状态/诊断查询、手动控制分派、auto_start 的 accepted+request_id、断线与急停行为、SBUS 优先级抢占返回 `CONTROL_PREEMPTED`。
- 安全回归：WEB 不越过仲裁器直触硬件；急停期间发射舵机命令全拒；急停解除必须人工确认；WEB 断线不恢复自动流程。

## 11. 当前状态

- 模块目标 `atom16_http_web`（STATIC）+ 首批源码（生命周期骨架、配置读取、限界、错误体系、`/api/state`）已落地；前端演示骨架 v3（浅色简约风 + 纵向摇杆 ±5000 rpm）已在浏览器验证。
- **2026-10-15 全量重读完成**：P0 循环依赖确认已解决（Entry.cpp 构造顺序重排）；四项裁决已记录（§8）；11 项前置阻塞登记于 `Atoms/Public/GeneralBugs/2026-10-15-ATOM16.md`。
- **仍阻塞（等外部）**：http 配置节（用户补）、`allowed_modes` nested 查询（ATOM-15）、装配层注册与窄接口桥接（Entry.cpp）、ATOM-12/13/19 未实现（`yaw_set`/自动流程执行侧/校准只能 503 桩）。
- **下一步（可立即开工）**：定义窄接口头文件；实现 `/api/diagnostics`（ATOM-18 三件套）、`/api/control/estop`、`/api/control/auto/stop`（始终可用）、`stop_fire_set/clear`、`force_set`（IForceController）、`GET /api/parameters`（只读）；`ConfigKeys.hpp` 键名修正（`http.http_status_mapping`）；静态资源 mount。
- **未经编译验证**：首次编译须在 Ubuntu ARM/aarch64 环境进行。
