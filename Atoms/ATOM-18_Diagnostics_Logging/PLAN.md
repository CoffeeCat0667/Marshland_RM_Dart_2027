# ATOM-18 Diagnostics & Logging 实现计划

> 依据：`Design/Atomic/ATOM-18_Diagnostics_Logging.md`（只读基线，同步基线 PRD 0.10 / PEF 1.5 / Interface 1.5 / Config 0.8，2026-10-10）。
> 配置键名：`Rule/ConfigSchema.md` §2.18（log.*）、§2.16（fault.*）；数值权威：`Design/Config.md` §14、§16。
> 工作规则：仅遵守本目录 `AGENTS.md`；任何修改需用户明确许可。

## 0. 决策基线（2026-10-15，用户拍板）

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | 依赖注入 | 参考 ATOM-03：构造函数注入 `const config::IConfigView&` + `const app::IMonotonicClock&`，经 `LoadLogConfig(view, LogConfig&)` 加载为 POD 配置结构体 |
| 2 | UTC 时间戳 | 直接用 `std::chrono::system_clock`，按 `UTC_ISO8601_MS` 格式化；不新增 `IUtcClock` |
| 3 | 对外接口 | 在 `Atoms/Public/` 新增契约头，拆三个接口：`ILogSink`（写入）、`IFaultRegistry`（故障）、`IDiagnosticsSnapshot`（快照查询） |
| 4 | 配置键名 | 以 `Rule/ConfigSchema.md` §2.18 既有 `log.*` 键为准（见 §5），不新造键 |
| 5 | 队列满策略 | **丢弃最旧** + 丢弃计数（`dropped_count`，入诊断快照）；ERROR/安全事件走独立保底通道，不参与丢弃 |
| 6 | 迟到视觉状态 | 推模式：业务方（状态机/发射流程）显式调用记录接口，本模块不做 30 s 判定 |
| 7 | 换弹日志 | 换弹/机构模块调用本模块接口上报六类事件（见 §3.5）；"启动默认抬起=逻辑初值"由装配层在启动时显式写入一条 |
| 8 | dart_control.json | 见 §5 配置表 |
| 9 | 轮转检查时机 | 每次写入时检查（写入本就在后台线程） |
| 10 | JSON 序列化 | nlohmann/json 3.12.0（`Marshland::ThirdPartyNlohmannJson`，PRIVATE 链接，`#include "nlohmann/json.hpp"`） |
| 11 | 单元测试 | 本目录不建测试 |
| 12 | 实现形态 | `Inc/` + `Src/`，STATIC 库 `atom_18_diagnostics_logging` |

## 1. 目标与边界

实现 Marshland 的故障诊断与日志模块：为每次故障、状态变更和关键流程建立可查询记录，并按 Config 输出日志。此模块**不自行决定机构控制命令**，只负责记录、查询、状态维护、轮转与诊断快照。

约束：

- 不提供 `main` 入口函数。
- 头文件放 `Inc/`，源文件放 `Src/`。
- 编译标准（C++23）、目标平台（Ubuntu ARM）和编译选项由上层 CMake 工程统一决定；本目录只负责模块组织、依赖和配置。
- 日志格式 `JSON_LINES`（UTF-8，每行一个结构化事件）；时间戳 `UTC_ISO8601_MS`；时限测量用 `IMonotonicClock`。
- 日志等级、路径、轮转、大小上限、保留条数和刷新策略全部从 `IConfigView` 读取，禁止硬编码默认值掩盖配置错误。

## 2. 原子操作清单（来自设计文档）

1. 为单个日志事件写入时间戳、等级、模块、事件、来源、状态和 payload。
2. 对单个故障分配 Config 中的故障码和等级。
3. 保存一个故障的发生时间和来源。
4. 查询当前活动故障集合。
5. 查询一条历史故障记录。
6. 更新一条故障的恢复状态。
7. 清除一个满足策略的故障记录。
8. 将一次状态迁移写入诊断日志。
9. 将一次控制权变更写入诊断日志。
10. 将一次发射/动作结果写入运行日志。
11. 将一次急停或人工解除事件写入安全日志。
12. 对 ERROR / 安全事件立即刷新日志。
13. 在轮转阈值达到时执行一次日志文件轮转。
14. 超过总存储限额时删除最旧的单个日志文件。
15. 对外提供一个诊断快照。

## 3. 模块划分与架构

### 3.1 Public 契约头（写入 `Atoms/Public/`，已获授权）

- `DiagnosticsTypes.hpp`：`LogLevel`（枚举封闭集 `TRACE/DEBUG/INFO/WARN/ERROR`，与 schema `log.level` 一致；映射 `log.level` 阈值）、`LogEvent`（字段对齐 Config §16.3：timestamp/level/module/event/source/state/request_id/error_code/payload）、`FaultRecord`（故障码、`FaultSeverity` 复用 `MarshlandTypes.hpp`、发生时间、来源、活动/历史、恢复状态）。
- `ILogSink.hpp`：日志写入接口；纯虚、noexcept；含普通写入与 ERROR/安全事件保底写入两条路径。
- `IFaultRegistry.hpp`：活动故障查询、历史故障查询、恢复状态更新、单条清除。
- `IDiagnosticsSnapshot.hpp`：诊断快照（活动故障集 + 队列丢弃计数等健康指标；快照内容字段由实现定义， consumers 为 Web/装配层）。

依赖方向保持 `Atom → Public`；本模块对外只暴露上述接口，其他 ATOM 不得包含本模块私有头。

### 3.2 本模块内部结构（Inc/ + Src/，已实现 2026-10-15）

```text
ATOM-18_Diagnostics_Logging/
├─ CMakeLists.txt          # STATIC 库 atom_18_diagnostics_logging（见 §6）
├─ PLAN.md
├─ Inc/atom18/
│  ├─ diagnostics_service.hpp    # 实现 Public 三接口的主类
│  ├─ log_config.hpp             # LogConfig POD + LoadLogConfig(IConfigView&, LogConfig&)
│  ├─ lifecycle_adapter.hpp      # ILifecycleModule 适配（参考 ATOM-03 LifecycleAdapter）
│  ├─ stored_event.hpp           # 定长缓冲的队列内事件存储（深拷贝 string_view）
│  ├─ json_lines.hpp             # JSON Lines 序列化 + UTC ISO8601 毫秒时间戳
│  └─ log_writer.hpp             # 后台写线程、双队列、轮转与清理（轮转逻辑并入此类）
└─ Src/
   ├─ diagnostics_service.cpp    # 故障管理 + 三接口实现 + 缓冲/补登
   ├─ log_config.cpp             # log.* 键加载与校验
   ├─ lifecycle_adapter.cpp
   ├─ stored_event.cpp
   ├─ json_lines.cpp             # nlohmann ordered_json，按 log.fields 顺序输出
   └─ log_writer.cpp             # 丢最旧+计数、保底通道、每次写入时检查轮转
```

注：PLAN 初稿中独立的 `log_rotation.cpp` 已并入 `log_writer.cpp`（轮转与文件句柄强耦合，拆分只会增加状态同步复杂度）。

### 3.3 注入与初始化时序（关键）

`RuntimeStep::CreateDiagnostics` 排在 `LoadConfiguration` **之前**，`initialize()` 时 `IConfigView::isLoaded()` 可能为 false：

- 构造函数只存引用（`const IConfigView&`、`const IMonotonicClock&`），不查配置。
- `initialize()` 尝试 `LoadLogConfig`；若 `NOT_LOADED`，进入**缓冲模式**：事件写入内存环形缓冲（容量为编译期常量），不落盘、不报错为初始化失败（诊断模块自身就绪）。
- 每次写入前检查 `configVersion()`：变化时重新 `LoadLogConfig`；首次成功后把缓冲事件补落盘。
- 配置查询返回非 OK（MISSING/INVALID_TYPE/OUT_OF_RANGE）时，显式记录一条配置故障事件（`W-CONFIG-003`），禁止静默回填默认值。

### 3.4 异步写入与刷新

- 单一后台写线程 + 有界队列（容量为编译期常量；不在 ConfigSchema 中，避免修订 schema）。
- 队列满：**丢弃最旧**并累计 `dropped_count`；写入方永不阻塞。
- ERROR 与安全事件：独立小容量保底通道，写线程优先处理并立即 flush（`fflush`/`fsync` 按 `log.flush_policy`）。
- 普通日志：最多缓冲 1 秒（Config §16.1），由写线程按单调时钟周期 flush。
- `log.stdout_echo = CRITICAL_ONLY`：仅关键告警/错误同步写 stdout。

### 3.5 换弹日志（推模式，由换弹/机构模块上报）

须记录的六类事件（Config §16.2、§14.2）：

- 左右速度目标（`reload.speed_rpm` 幅值 + 方向约定：抬起左正右负、放下左负右正）；
- 计时开始（`timing.reload_lift_run_s` / `reload_lower_run_s`，3 s 运行计时）；
- 零转速完成（两台 0 rpm 目标提交成功）；
- 手动 / 急停零力矩中止；
- 单侧故障联动（另一侧零力矩并取消动作）；
- 恢复失败 FAULT 锁定（`F-RELOAD-002`）。

另：启动默认抬起状态由装配层在启动时写入一条日志，**标明为逻辑初值**，不伪装为物理位置检测。

### 3.6 故障管理

- 故障码/等级来自 Config §14.2 故障表（经 `fault.table[]` 查询，禁止新增表外故障码）。
- 活动/历史故障分集存储；历史保留 `log.fault_history_limit`（1000）条，策略 `RING_LATEST_FIRST`。
- 查询：按故障码、来源、发生时间。
- 清除：按策略清除单条（如已恢复且人工确认），不影响其他故障。

## 4. 诊断规则

记录 30 秒回退后的迟到视觉状态（推模式，业务方调用，本模块只记录）：

- 当前发次尚未实际发射：记录"重新执行 YAW 到位检查"事件；
- 当前发次已经完成发射：记录"迟到视觉状态"事件，**不回滚已完成动作**（`W-VISION-002` 属正常回退路径，不触发一般故障恢复）。

## 5. dart_control.json 需要写入的 `log` 节

键名全部来自 `Rule/ConfigSchema.md` §2.18（既有键，不新增），数值来自 `Design/Config.md` §16：

| 键 | 类型 | 值 | 来源 |
|---|---|---|---|
| `log.level` | enum | `"INFO"` | §16.1（故障/安全事件至少 WARN，严重故障用 ERROR，为代码逻辑非配置） |
| `log.output_directory` | str | `"./Logs/"` | §16.1，相对程序工作目录 |
| `log.stdout_echo` | str | `"CRITICAL_ONLY"` | §16.1 |
| `log.format` | str | `"JSON_LINES"` | §16.1 |
| `log.timestamp_format` | str | `"UTC_ISO8601_MS"` | §16.1 |
| `log.rotation_enabled` | bool | `true` | §16.1 |
| `log.max_files` | int | `10` | §16.1 |
| `log.max_file_size_bytes` | int | `10485760`（10 MiB） | §16.1 |
| `log.max_total_size_bytes` | int | `104857600`（100 MiB） | §16.1 |
| `log.fault_history_limit` | int | `1000` | §16.1 |
| `log.fault_history_policy` | str | `"RING_LATEST_FIRST"` | §16.1 |
| `log.flush_policy` | str | `"ERROR_SAFETY_IMMEDIATE_OTHERS_BUFFER_1S"` | §16.1"ERROR/安全事件立即刷新，其他最多缓冲 1 秒"；schema 该键为自由 string（无枚举约束），字面量已由本模块定义并核对 schema.json 通过 |
| `log.required_events` | array\<str\> | 见下 | §16.2 十类必记事件；schema 该键为 `array<string>`（无枚举约束），字面量已核对 schema.json 通过 |
| `log.fields` | array\<str\> | `["timestamp","level","module","event","source","state","request_id","error_code","payload"]` | §16.3，字段顺序即输出顺序 |

`log.required_events` 建议取值（snake_case 标识，对应 §16.2 十类事件 + 换弹子事件由 payload 区分）：

```json
["startup", "shutdown", "module_init_result", "control_source_change",
 "auto_flow_start", "auto_flow_stop", "auto_flow_terminate", "current_shot",
 "yaw_target", "yaw_arrived", "yaw_clear",
 "reload_action", "feed_action", "fire_action",
 "fault", "auto_recovery", "software_estop", "estop_manual_release",
 "pid_modify", "pid_persist_result"]
```

核对结论（2026-10-15，已授权读 `Rule/ConfigSchema.schema.json`）：`log.flush_policy` 与 `log.required_events` 在 schema 中均为无枚举约束的自由形式（string / array\<string\>），上表字面量可直接通过校验；`log.level` 合法集为 `TRACE/DEBUG/INFO/WARN/ERROR`。`fault.table[]`（§2.16，故障码表）已在 schema 中定义，属 ATOM-15 范围，本模块只消费不新增。队列容量为编译期常量，**不**入 JSON（避免修订 ConfigSchema）。

## 6. CMake 集成方案

- STATIC 库 `atom_18_diagnostics_logging`，别名 `Marshland::atom_18_diagnostics_logging`。
- 由上层 `add_subdirectory()` 组装；不含 `project()`/`main()`/工具链设置。
- 依赖（已获授权，2026-10-15 已加入本目录 CMakeLists.txt）：
  - `Marshland::PublicHeaders`（契约头，按 `if(TARGET ...)` 模式链接，与上层注释约定一致）；
  - `Marshland::ThirdPartyNlohmannJson`（仅在有 Src/ 源码、STATIC 形态时 PRIVATE 链接，避免第三方头泄漏到对外接口）。

## 7. 实施顺序

1. 在 `Atoms/Public/` 新增 `DiagnosticsTypes.hpp` / `ILogSink.hpp` / `IFaultRegistry.hpp` / `IDiagnosticsSnapshot.hpp` 契约头。
2. 本目录建 `Inc/atom18/` + `Src/`：`LogConfig` POD 与 `LoadLogConfig`（对齐 §5 键表）。
3. JSON Lines 序列化（nlohmann）+ UTC ISO8601 毫秒时间戳（`system_clock`）。
4. 故障管理：记录、活动/历史查询、恢复更新、单条清除、1000 条环形历史。
5. 后台写线程：有界队列（丢最旧+计数）、保底通道、刷新策略（立即/1 秒缓冲）。
6. 文件轮转与总容量清理（每次写入时检查；命名 `{event?}-{UTC日期}-{序号}.jsonl`，实现时定稿）。
7. 状态迁移、控制权变更、发射结果、急停/人工解除专用记录接口。
8. 诊断快照（活动故障集 + dropped_count 等健康指标）。
9. 换弹六类事件 + "启动默认抬起=逻辑初值"记录接口（推模式，供业务方调用）。
10. `LifecycleAdapter`（参考 ATOM-03）接入 `CreateDiagnostics` 步骤，含 §3.3 缓冲模式。
11. 由上层 CMake 在 ARM 环境（WSL 交叉编译或目标主机）增量编译集成验证；不建独立测试。

## 8. 验收映射（对齐设计文档验收条件）

- 每个故障事件可按故障码、来源和发生时间查询。
- 活动故障与历史故障区分清楚；清除一个故障不误清其他故障。
- 状态迁移、控制权变更、发射结果、急停和恢复均产生结构化日志。
- 达到单文件或总量限制时按 Config 轮转/清理，不阻塞控制线程。
- 安全 / ERROR 事件立即刷新，普通日志按配置策略刷新。
- 诊断规则能区分"尚未实际发射"和"已完成发射"两种情况。
- 换弹日志完整记录 §3.5 所列事件；启动默认抬起明确标注为逻辑初值。
