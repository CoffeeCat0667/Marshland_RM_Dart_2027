# ATOM-18_Diagnostics_Logging 顶层网关接入说明

> 版本：1.0
> 日期：2026-10-15
> 适用对象：`NewVersion/Entry.cpp`、`Atoms/Public/AppCoordinator` 及顶层对象装配代码

## 1. 接入目标

ATOM-18 是唯一负责故障登记与结构化日志输出的模块（程序代号 Marshland 的诊断与日志原子）。它对外提供三个 Public 契约接口：

```cpp
marshland::diagnostics::ILogSink                    // 日志写入（诊断/运行/安全）
marshland::diagnostics::IFaultRegistry              // 故障登记、查询、恢复、清除
marshland::diagnostics::IDiagnosticsSnapshotProvider// 诊断快照
```

顶层网关必须完成两件事：

1. 创建并长期持有 `marshland::atom18::DiagnosticsService`（它同时实现上述三接口）；
2. 创建 `marshland::atom18::LifecycleAdapter`，把它注册到 ATOM-01 的 `RuntimeStep::CreateDiagnostics`。

其他 ATOM 不得自行打开/写入日志文件，不得自行定义日志格式，不得包含 `Inc/atom18/` 下的任何私有头。

## 2. CMake 依赖

上层工程已通过 `add_subdirectory(Atoms)` 创建：

```text
atom_18_diagnostics_logging
Marshland::atom_18_diagnostics_logging
```

`Entry.cpp` 所属的最终可执行目标链接 `Marshland::Atoms` 聚合目标即可，不需要在顶层重复加入 ATOM-18 的源码文件。

ATOM-18 的装配头通过其 `Inc` 包含目录提供：

```cpp
#include "atom18/diagnostics_service.hpp"
#include "atom18/lifecycle_adapter.hpp"
```

其他 ATOM 只包含 Public 契约头：

```cpp
#include "ILogSink.hpp"
#include "IFaultRegistry.hpp"
#include "IDiagnosticsSnapshot.hpp"
#include "DiagnosticsTypes.hpp"
```

## 3. 对象图与生命周期

**构造顺序敏感**：`CreateDiagnostics` 是 `RuntimeStep` 的第一步，先于 `LoadConfiguration`。因此 ATOM-18 构造时配置尚未加载——它只保存 `IConfigView` 引用，配置加载后经 `configVersion()` 变化自动感知。装配层只要求 ATOM-15 的 `ConfigStore` **对象**先于 ATOM-18 存在（不要求配置已加载）。

```text
ConfigStore（ATOM-15，未加载状态即可）
  ├─ SystemMonotonicClock（IMonotonicClock）
  ├─ DiagnosticsService(config_store, clock)
  │     ├─ 以 ILogSink / IFaultRegistry / IDiagnosticsSnapshotProvider 分发给各 ATOM
  │     └─ LifecycleAdapter(DiagnosticsService&)
  │           └─ RuntimeCoordinator::addModule(CreateDiagnostics, ...)
  └─ ... 其余 ATOM 按既有顺序
```

推荐的顶层声明顺序：

```cpp
marshland::atom15::ConfigStore config_store;
marshland::app::SystemMonotonicClock clock;
marshland::atom18::DiagnosticsService diagnostics{config_store, clock};
auto diagnostics_adapter =
    std::make_unique<marshland::atom18::LifecycleAdapter>(diagnostics);
// ... RuntimeCoordinator 与其余模块
```

C++ 局部对象逆序析构：以上顺序保证 `diagnostics` 在所有使用它的模块之后销毁。`LifecycleAdapter` 只保存 `DiagnosticsService&`，禁止让 `diagnostics` 先于 `RuntimeCoordinator` 销毁。

## 4. 注册 ATOM-18

必须在首次 `runtime.start()` 前完成注册，且检查返回值：

```cpp
const bool registered = runtime.addModule({
    marshland::runtime::RuntimeStep::CreateDiagnostics,
    true,
    std::move(diagnostics_adapter)
});
if (!registered) {
    std::fprintf(stderr,
                 "[marshland] ATOM-18 registration failed: %s\n",
                 marshland::runtime::toString(runtime.lastRegistrationError()));
    return 1;
}
```

注册要求：

- `step` 必须为 `RuntimeStep::CreateDiagnostics`（启动序列第一步，后续步骤的初始化结果才能被记录）；
- `required` 为 `true`；
- `addModule()` 返回 `false` 时必须读取 `lastRegistrationError()`；
- 不要创建占位模块掩盖注册失败。

`initialize()` 不依赖配置就绪（缓冲模式），恒返回成功；诊断不可用（如日志目录不可写）不会阻断启动，事件进入内存缓冲并通过快照暴露。

## 5. 启动时序与缓冲模式

```text
RuntimeCoordinator::start()
  -> [CreateDiagnostics] ATOM-18 initialize()
       -> 配置未加载：进入缓冲模式（事件入 256 条环形缓冲，满时丢最旧并计数）
  -> [LoadConfiguration] ATOM-15 加载配置，configVersion() 递增
  -> 此后第一个日志事件到达时，ATOM-18 感知版本变化：
       -> LoadLogConfig() 校验 log.* 键
       -> 启动后台写线程，打开首个日志文件
       -> 补登缓冲期故障报告（此时才从 fault.table[] 取等级、分配故障 id）
       -> 回放缓冲事件落盘
```

对装配层的要求：**无**。不需要"配置就绪后通知 ATOM-18"的额外逻辑。

热加载同理：`configVersion()` 每次变化都会触发 `log.*` 重读并下发到写线程；非法键显式拒绝（stderr + 保留旧配置），不会静默兜底。

## 6. 向其他 ATOM 注入接口

顶层装配层把同一个 `diagnostics` 对象按接口分发给消费者：

```cpp
// 写日志的模块（状态机、机构控制、通信模块等）
some_module_adapter{static_cast<marshland::diagnostics::ILogSink&>(diagnostics), ...};

// 上报故障的模块
motor_adapter{static_cast<marshland::diagnostics::IFaultRegistry&>(diagnostics), ...};

// Web/HTTP 模块（Interface 18/19 查询入口）
http_adapter{static_cast<marshland::diagnostics::IDiagnosticsSnapshotProvider&>(diagnostics),
             static_cast<marshland::diagnostics::IFaultRegistry&>(diagnostics), ...};
```

### 6.1 写入调用约定

```cpp
const marshland::diagnostics::PayloadField fields[] = {
    {"from", std::string_view{"STANDBY"}},
    {"to",   std::string_view{"MANUAL"}},
};
marshland::diagnostics::LogEvent ev;
ev.level  = marshland::diagnostics::LogLevel::INFO;
ev.module = "state";
ev.event  = "mode_transition";
ev.source = "WEB";              // 控制源或事件来源
ev.state  = "MANUAL";           // 当前系统/流程状态
ev.request_id = "req-123";      // 可空
ev.payload = fields;
ev.payload_count = 2;
log_sink.WriteDiagnostic(ev);   // noexcept、不阻塞（异步队列）
```

- 三个写入方法对应三条逻辑流：`WriteDiagnostic`（状态迁移、控制权变更、迟到视觉状态）、`WriteRun`（发射/动作结果、换弹/供弹/发射动作）、`WriteSafety`（急停、人工解除、安全事件）。单文件统一输出，`stream` 以保留键注入 payload 区分；
- `string_view` 参数只需在调用期间有效（ATOM-18 入队前深拷贝）；payload 最多 16 个字段，超长截断并在 payload 标记 `"_truncated": true`；
- 低于 `log.level` 的事件被直接丢弃；ERROR 与安全事件走保底通道并立即刷新；
- 换弹六类事件（左右速度目标、计时开始、零转速完成、手动/急停零力矩中止、单侧故障联动、恢复失败 FAULT 锁定）由换弹/机构模块埋点调用 `WriteRun`/`WriteSafety`；"启动默认抬起"由装配层在启动时写一条日志，必须标注为逻辑初值。

### 6.2 故障上报约定

```cpp
const std::uint64_t id = fault_registry.ReportFault("F-CAN-001", "can0");
// id == 0：故障码不在 Config §14.2 fault.table[] 内（拒绝），
//          或配置未就绪（已入待登记缓冲，补登后才分配 id）
```

- 故障等级由 ATOM-18 从 `fault.table[]` 查表，**调用方不得自定等级**；
- `MarkFaultRecovered(id)` 在根因清除后调用；`ClearFault(id)` 仅在已恢复后生效，且只影响该 id 这一条；`fault.table[]` 中 `requires_confirm=true` 的故障，`ClearFault` 调用本身即代表人工确认已由上游（Web 路径）完成；
- 故障登记/恢复/清除各自生成一条结构化日志（`fault` / `fault_recovered` / `fault_cleared`）。

### 6.3 快照查询

```cpp
const marshland::diagnostics::DiagnosticsSnapshot snap = snapshot_provider.GetSnapshot();
// config_loaded / writer_active / active_fault_count / history_fault_count /
// dropped_event_count / buffered_event_count / written_event_count / rotation_count
```

活动故障明细经 `IFaultRegistry::QueryActiveFaults()` 获取。

## 7. 配置文件要求

`Config/dart_control.json` 必须包含 `log` 节（键名权威：`Rule/ConfigSchema.md` §2.18；数值权威：`Design/Config.md` §16）：

| 键 | 值 |
|---|---|
| `log.level` | `"INFO"` |
| `log.output_directory` | `"./Logs/"` |
| `log.stdout_echo` | `"CRITICAL_ONLY"` |
| `log.format` | `"JSON_LINES"` |
| `log.timestamp_format` | `"UTC_ISO8601_MS"` |
| `log.rotation_enabled` | `true` |
| `log.max_files` | `10` |
| `log.max_file_size_bytes` | `10485760` |
| `log.max_total_size_bytes` | `104857600` |
| `log.fault_history_limit` | `1000` |
| `log.fault_history_policy` | `"RING_LATEST_FIRST"` |
| `log.flush_policy` | `"ERROR_SAFETY_IMMEDIATE_OTHERS_BUFFER_1S"` |
| `log.required_events` | 21 个事件标识数组，见 ATOM-18 `PLAN.md` §5 |
| `log.fields` | `["timestamp","level","module","event","source","state","request_id","error_code","payload"]` |

缺失的键使用 `Config.md` §16.1 内置默认值（计入快照语义）；类型/范围非法的键使整份 log 配置被拒绝（保留旧配置）。故障码表 `fault.table[]` 属 ATOM-15 接入范围，ATOM-18 只消费。

日志文件命名：`marshland-<UTC毫秒>-<序号>.jsonl`，写入 `log.output_directory`；单文件达上限轮转，超 `max_files` 或 `max_total_size_bytes` 删除最旧文件。

## 8. 禁止事项

- 任何 ATOM 不得自行创建日志文件或第二份 JSON Lines 输出；
- 不得用 `RuntimeContext::values` 传递日志配置；
- 不得包含 `Inc/atom18/` 私有头；
- 不得上报 `fault.table[]` 之外的故障码；
- 不得在持有 `DiagnosticsService` 引用的情况下先于它销毁；
- 不得把 `IDiagnosticsSnapshotProvider::GetSnapshot()` 当作故障明细来源（明细走 `IFaultRegistry`）。

## 9. 接入自检清单

- [ ] `ConfigStore` 对象在 `DiagnosticsService` 之前创建（不要求配置已加载）。
- [ ] `DiagnosticsService` 寿命长于所有使用其接口的模块。
- [ ] ATOM-18 只注册一次，步骤为 `RuntimeStep::CreateDiagnostics`，`required == true`。
- [ ] `addModule()` 返回值已检查，失败时读取 `lastRegistrationError()`。
- [ ] 各 ATOM 经 Public 契约头注入，无人包含 `Inc/atom18/`。
- [ ] `dart_control.json` 含完整 `log` 节与 `fault.table[]`。
- [ ] 换弹六类事件与"启动默认抬起=逻辑初值"埋点已在对应模块落实。
- [ ] Web 查询路径使用 `IFaultRegistry` + `IDiagnosticsSnapshotProvider`。
- [ ] 关机经 `RuntimeCoordinator` 正常驱动 `shutdown()`（冲刷并停止写线程），非直接 `exit()`。

## 10. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-15 | 新增 ATOM-18 顶层网关注册、对象生命周期（配置后加载缓冲模式）、三接口注入分发、写入/故障/快照调用约定、log 配置节要求与接入自检清单。 |
