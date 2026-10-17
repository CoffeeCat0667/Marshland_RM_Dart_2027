# ATOM-09_Force 工程接入说明

> 版本：1.0
> 日期：2026-10-16
> 适用模块：`../Atoms/ATOM-08_Control`
> 取代：2026-10-15 的「接入结论 —— 对 GeneralBugs/20261015-Entry.md 的答复」版（技术结论保留，结构重写为正式接入说明）
> 对照模板：`Rule/ATOM3ConnectProject.md`、`Rule/ATOM4ConnectProject.md`、`Rule/ATOM5ConnectProject.md`

## 1. 接入目标与结论

ATOM-09 负责变力机构控制：接受一个变力目标，交给 Config 映射的变力电机（当前 `motor[].role == "force"`，即电机 6 / node `0x206` / `0x1FF` 帧槽位 1）闭环执行，并提供目标、当前反馈与动作状态。本模块**不安排发射流程顺序、不计发次**。

**结论：ATOM-09 侧无需任何代码修改即可接入。**

1. **不注册生命周期**。按 `PLAN.md` §2.1 与裁决 Q-07，ATOM-09 是纯逻辑内核：不实现 `ILifecycleModule`、不提供 `lifecycle_adapter`、不注册任何 `RuntimeStep`。因此「模块注册需要先构造内核对象」这一环**不存在**，装配层可以在 `AppCoordinator` 构造完成**之后**再构造 `ForceController`。
2. **不需要 `IControlAuthority`**。`ForceController` 构造函数只注入 `IStateReader`；授权决策归 ATOM-02，控制意图由调用方（ATOM-13 / ATOM-16）先 `SubmitIntent(CONTROL)`，ATOM-09 只做 fail-closed 复核（Q-05 / Q-16）。
3. 因此 `Atoms/Public/GeneralBugs/20261015-Entry.md` 所述「ATOM-08/09/10/11 循环依赖」**对 ATOM-09 不成立**；`Entry.cpp:417` 的编译错误属**装配顺序与实参类型错误**，不是架构死锁。

## 2. CMake 接入方向

```text
Marshland 可执行
  -> Atoms/Public/AppCoordinator（顶层装配）
  -> Marshland::atom_09_force
  -> Marshland::PublicHeaders
```

要求：

1. 上层工程先建立 `Atoms/Public` 的 `Marshland::PublicHeaders`（由 `Atoms/Public/Headers/CMakeLists.txt` 定义）。
2. 上层通过 `add_subdirectory(Atoms/ATOM-09_Force)` 加入本模块；**该行已在 `Atoms/CMakeLists.txt:25` 就位**，聚合链接见同文件 `:50`。
3. ATOM-09 提供目标 `atom_09_force` 与别名 `Marshland::atom_09_force`。
4. ATOM-09 依赖 `Marshland::PublicHeaders` 与 C++ 标准库；**不依赖 `Threads::Threads`**（本模块无线程）。
5. 有 `Src/*.cpp` 时模块目标为 `STATIC`（参与增量编译）。
6. 本模块不定义 `project()`，不创建可执行文件，不设置架构、C++ 标准或编译选项；ARM 工具链、警告选项与最终可执行文件全部由上层决定。

## 3. 公共契约

`Atoms/Public` 中属于本模块的两个头已登记进 `Atoms/Public/Headers/CMakeLists.txt`。公共头只依赖 `MarshlandTypes.hpp` 与 C++ 标准库，**禁止**包含 `AppCoordinator.hpp` 或任何 ATOM 私有头。

### 3.1 `ForceTypes.hpp` —— 跨 ATOM 共享数据

| 类型 | 内容 |
|---|---|
| `enum class ForceMode` | `TIMED`（自动流程，X 秒完成） / `MANUAL`（SBUS・WEB，无时限） |
| `enum class ForceActionState` | `IDLE` / `RUNNING` / `COMPLETED` / `REJECTED` / `FAULT` |
| `enum class ForceSubmitResult` | `ACCEPTED` / `REJECTED_INVALID` / `REJECTED_RANGE` / `REJECTED_CONFIG` / `REJECTED_BUSY` / `REJECTED_SAFETY` / `MOTOR_COMMIT_FAILED` |
| `struct ForceTarget` | `target_rpm`、`source`（`ControlSource`）、`request_id` |
| `struct ForceMotorFeedback` | `valid`、`has_received`、`rpm`、`timestamp_us` |
| `struct ForceStatus` | `state`、`mode`、`target_rpm`、`source`、`feedback`、`deadline_us`、`generation`、`error_code` |

`ForceStatus::error_code` 是**本地诊断标识**（如 `CONFIG` / `RANGE` / `SAFETY` / `MOTOR` / `CLOCK` / `SBUS_RANGE` / `CONFIG_RELOAD`），**不是** `Design/Config.md` §14.2 故障码：故障码由 ATOM-05 / ATOM-04 / ATOM-14 各自登记，本模块不重复登记（Q-17）。

### 3.2 `ForceInterfaces.hpp` —— 对外调用端口

```cpp
class IForceController {
    ForceSubmitResult submitTarget(const ForceTarget& target, ForceMode mode) noexcept;
    void update(std::int64_t now_us) noexcept;
    ForceStatus status() const noexcept;
    void stop() noexcept;
    bool ready() const noexcept;
    const char* initializationError() const noexcept;
};
```

全部方法 `noexcept`，不得阻塞在 I/O 上。调用方**必须**区分 `ACCEPTED` 与其余取值，禁止把拒绝或 `MOTOR_COMMIT_FAILED` 当作成功。

## 4. 装配对象图与构造时机

### 4.1 对象图（装配层创建并持有，顺序保证被引用对象寿命更长）

```text
marshland::atom15::ConfigStore      config_store;   // IConfigView
marshland::app::SystemMonotonicClock clock;        // IMonotonicClock
AppCoordinator                       app{runtime, arbitrator, clock};
  -> 装配层的 IMotorCommandSink 适配器（适配 ATOM-05）
  -> 装配层的 IForceFaultSink 适配器（适配 ATOM-14，可为 nullptr）
  -> marshland::atom09::ForceController force_controller(
         config_store, clock, sbus_core, app, force_motor_sink, &force_fault_sink);
```

### 4.2 构造时机：**必须在 `AppCoordinator` 之后**

ATOM-09 无生命周期注册，因此构造时机自由；但 `IStateReader` 由 `AppCoordinator` 实现，故顺序固定为：

```cpp
marshland::app::AppCoordinator app{runtime, arbitrator, clock};

marshland::atom09::ForceController force_controller(
    config_store,        // config::IConfigView        —— ATOM-15
    clock,               // app::IMonotonicClock       —— 装配层
    sbus_core,           // sbus::ISbusSnapshotReader  —— ATOM-03
    app,                 // app::IStateReader          —— AppCoordinator 实现
    force_motor_sink,    // atom09::IMotorCommandSink  —— 装配层适配 ATOM-05
    &force_fault_sink);  // atom09::IForceFaultSink*   —— 装配层适配（可空）
```

装配层**只应向模块暴露接口引用**（`IStateReader&` 等），不要注入具体 `AppCoordinator&`（`Rule/AtomIntegration.md` §4.2）。

### 4.3 就绪检查（构造后立刻做）

构造函数会立即读取并校验配置。配置不可用时**不抛异常、不崩溃**，而是 `ready() == false` 且 `initializationError()` 返回可定位的原因：

```cpp
if (!force_controller.ready()) {
    std::fprintf(stderr, "[marshland] ATOM-09 config unusable: %s\n",
                 force_controller.initializationError());
    // 装配层决定：记录诊断 / 阻止进入可动作状态。不得静默忽略。
}
```

另：若构造时 ATOM-15 尚未加载配置（例如在 `runtime.start()` 之前构造），ATOM-09 会在首次 `update()` 时依 `IConfigView::configVersion()` 变化**自动重试加载**；但推荐直接把构造放在 `app.Start()` 成功之后，行为最确定。

## 5. 生命周期与周期驱动（P0）

### 5.1 不注册任何 `RuntimeStep`

`Atoms/Public/ILifecycle.hpp` 的 11 个步骤均为**硬件与服务**初始化步骤，其中无变力对应项；`Rule/AtomIntegration.md` §3.1 亦规定纯逻辑模块不应强行注册。因此 ATOM-09 **不实现 `ILifecycleModule`**，装配层**不要**调用 `runtime.addModule(...)`。

### 5.2 由装配层周期线程驱动（Q-04）

`update()` 由装配层普通周期线程调用，周期上限取 `timing.max_control_cycle_ms`（100 ms；实际可按 `timing.motor_loop_period_ms` 的 20 ms 量级）：

```cpp
force_controller.update(clock.nowMicros());
```

### 5.3 启停时序（**顺序错误会导致悬垂线程与 UAF**）

```text
① 构造对象图（config_store / sbus / can / motor / force ...）      <- 不启线程
② AppCoordinator app{runtime, arbitrator, clock}
③ app.Start(&detail)
      |- 失败 -> return（线程从未启动，无需清理）
      |- 成功 v
④ scheduler.start()                                              <- 此时才创建线程
⑤ exit_signals.Wait(...)                                         <- 运行
⑥ scheduler.stop() + join                                        <- 先停调用方
⑦ app.Shutdown()                                                 <- 再拆模块
⑧ 对象析构
```

规则：

1. **必须在 `app.Start()` 成功之后**才启动周期线程；否则会对未初始化模块调用 `update()`。
2. **必须在 `app.Shutdown()` 之前**停止并 `join` 周期线程；否则线程仍在回调已析构的模块（UAF）。
3. 建议把节拍器做成 **RAII 小类**（析构即 `stop()`），避免任何提前 `return` 造成线程泄漏。
4. `stop()` 必须**幂等**；重复关闭不得再次提交 CAN 输出。

### 5.4 节拍器骨架

```cpp
class PeriodicScheduler {
public:
    void add(std::function<void(std::int64_t)> task, std::uint32_t period_ms);
    bool start() noexcept;   // 创建线程；失败如实返回 false
    void stop()  noexcept;   // 幂等：置停止标志 -> join
    ~PeriodicScheduler() { stop(); }
};

PeriodicScheduler scheduler;
// ... 构造对象图与 app.Start() ...
scheduler.add([&force_controller](std::int64_t now_us) { force_controller.update(now_us); },
              cycle_ms);
if (!scheduler.start()) { /* 如实报错并退出，不得静默继续 */ }

exit_signals.Wait(exit_signal);
scheduler.stop();   // ★ 先于 app.Shutdown()
app.Shutdown();
```

调度要求（`Design/Config.md` §15、`Rule/AtomIntegration.md` §8.4）：

- 使用**单调时钟绝对期限**推进，Linux `SCHED_OTHER`；
- 监测超期并记录，但**不得追赶**多次计算；
- **禁止**把日志、网络、文件 I/O 放进 `update()` 路径；
- 线程创建失败必须如实上报。

## 6. 并发与线程安全边界（P0）

ATOM-09 内部有 `std::mutex`，`submitTarget` / `update` / `status` / `stop` 全部加锁，**允许**被不同线程并发调用。装配层仍需注意：

| 并发方 | 调用 | 说明 |
|---|---|---|
| 装配层节拍线程 | `update(now_us)` | 唯一的时间推进者 |
| HTTP/WebSocket 线程 | `submitTarget(...)`（`force_set`） | 与节拍线程并发 |
| 流程线程（ATOM-13） | `submitTarget(...)`、`status()` | 与节拍线程并发 |

三条硬要求：

1. **ATOM-09 只自我保护**。装配层注入的 `IMotorCommandSink` 适配器**自身必须线程安全**（会同时被节拍线程与 HTTP 线程调用）。
2. **装配层仍需负责统一同步**。`AppCoordinator` / ATOM-02 侧的外部输入唯一写入点与统一互斥门面（根 `PLAN.md` §5）**尚未落地**；在它落地前，`ReportXxx` / `SubmitIntent` 的跨线程调用没有全局保护，属装配层遗留风险。
3. **不得在持有 `StateReadView` 租约时回调其他端口**（`AppInterfaces.hpp` 契约）。ATOM-09 内部遵守此约束（读状态后立即释放租约）。

## 7. 配置接入

一律经注入的 `config::IConfigView` 查询；**禁止**自解析 JSON、**禁止**臆造默认值、**禁止**消费标记为 `[待定]` 或未列出的键。所有键必须存在且类型正确，否则 `ready() == false`。

| 键 | 要求 |
|---|---|
| `force.control_mode` | 必须为 `CLOSED_LOOP_PID` |
| `force.target_unit` | 必须为 `rpm` |
| `force.completion_criterion` | 必须为 `TIMED` |
| `force.center_action` | 必须为 `ZERO_TORQUE`（Q-13） |
| `force.target_default_source` | 必须为 `SBUS_CH12` |
| `force.has_dedicated_fault_handling` | 必须为 `false` |
| `force.target_range` | 整数 1..15000（对称 ±，WEB `force_set` 用） |
| `force.center_deadband_rpm` | 整数 0..2000（CH12 中位判定死区） |
| `timing.force_time_s` | 整数 0..3600（TIMED 完成判据 X 秒） |
| `timing.max_control_cycle_ms` | 整数 1..100 |
| `motor[].role` / `motor[].id` | 恰好一条 `role == "force"`，其 `id` 即变力电机（**禁止硬编码 6**） |

说明：ATOM-09 **不读取任何 `sbus.*` 键**（直接消费 `ISbusSnapshotReader` 的快照）；`timing.force_max_action_ms` 按 Q-14 方案 B **不再消费**。

热加载：`update()` 会比较 `IConfigView::configVersion()`；版本变化时重新加载。**重新加载失败**时 ATOM-09 会停止电机、置 `FAULT` 并上报 `onForceActionFailed`，**不会**静默沿用旧配置。

## 8. 数据流与线程流

```text
控制权查询  IStateReader::ReadState().context().control_source
   |- SBUS  -> sbus::ISbusSnapshotReader::readSbusSnapshot().scaled_channels[11]   (CH12 专用 rpm)
   |             |- |rpm| <= force.center_deadband_rpm -> stopMotor()（零力矩）+ IDLE
   |             |- 否则                                 -> submitVelocityTarget(rpm)
   |- WEB   -> IForceController::submitTarget(..., ForceMode::MANUAL)   （force_set）
   |- AUTO  -> IForceController::submitTarget(..., ForceMode::TIMED)    （ATOM-13）
   v
IMotorCommandSink -> ATOM-05（PID）-> ATOM-04（CAN 0x1FF 槽位 1）-> 电机 6 / 0x206

反馈：ATOM-05 -> IMotorCommandSink::readFeedback() -> ForceStatus::feedback
故障：IMotorCommandSink::faultLatched() -> IForceFaultSink::onForceActionFailed(detail)

停止：装配层 scheduler.stop() -> app.Shutdown()；
      ATOM-09 自身 stop() 撤销目标并请求零力矩（幂等）。
```

注意两条定时语义（Q-10）：`TIMED` 恰好执行 `timing.force_time_s` 后置 `COMPLETED`；`MANUAL` **无时限**，持续到新目标、控制权切换或安全事件。TIMED 动作进行中**不消费 CH12**（控制权接管由 ATOM-02 / ATOM-13 在当前硬件动作组完成后处理）。

## 9. 依赖适配清单（装配层负责）

| ATOM-09 端口 | 适配目标 | 说明 |
|---|---|---|
| `IMotorCommandSink::submitVelocityTarget(id, rpm)` | ATOM-05 电机内核 | 提交速度闭环目标 |
| `IMotorCommandSink::stopMotor(id)` | ATOM-05 | 撤销目标并走**零力矩**锁存（**不是** 0 rpm 闭环目标，Q-13） |
| `IMotorCommandSink::readFeedback(id, out)` | ATOM-05 | 读取该电机最近一次有效反馈 |
| `IMotorCommandSink::faultLatched(id)` | ATOM-05 | 电机/编码器/PID/CAN 故障锁存查询 |
| `IForceFaultSink::onForceActionFailed(detail)` | 装配层 -> ATOM-14 | 只上报「变力动作失败」事实，**不携带故障码**（Q-17） |

适配器约束：

1. **禁止回调 ATOM-09**（避免形成依赖环）；
2. 全部方法 `noexcept`，必须在控制周期预算内返回，不得阻塞；
3. 适配器自身线程安全（见 §6）；
4. `stopMotor()` 必须真实落到零力矩路径，且不得与 ATOM-04 的全零安全帧互相覆盖（`Rule/ATOM5ConnectProject.md` §5、§9）。

## 10. 调用方契约与开放问题

| 调用方 | 调用方式 | 前置条件 |
|---|---|---|
| ATOM-13（自动流程） | 每发一次 `submitTarget({rpm, ControlSource::AUTO, request_id}, ForceMode::TIMED)` | 与 `AutoFireState::FORCE` 对齐；先 `SubmitIntent(CONTROL)` |
| ATOM-16（WEB `force_set`） | `submitTarget({rpm, ControlSource::WEB, request_id}, ForceMode::MANUAL)` | 允许模式「手动・调试」；受 `force.target_range` 限制；先仲裁 |
| ATOM-17/18（状态推送） | 轮询 `status()` | 只读，不加锁外依赖 |

调用方**不得**包含 ATOM-09 的私有头（`Inc/atom09_force/`），只允许包含 `Atoms/Public/ForceTypes.hpp` 与 `ForceInterfaces.hpp`。

**已知开放问题（阻塞 ATOM-13）**：`BUG-20261014-09-13` —— 全库缺少「自动变力目标 rpm」配置键，`Design/Config.md` §5.6 只写「自动流程按 PEF 执行定时变力动作」。两种候选：(a) 自动流程沿用当前手动目标（先读 `status().target_rpm`）；(b) 新增 `force.auto_target_rpm` 键。**裁决前 ATOM-13 无法确定 TIMED 的 `target_rpm`**；ATOM-09 侧接口两种情况均无需改动。

其他尚未就绪的外部项（安全门、周期线程、CAN 端口、ATOM-14/16、ATOM-05 适配面）见 `Atoms/Public/GeneralBugs/2026-10-14-ATOM-09.md`，本文件不重复登记。

## 11. 就绪检查与关闭语义

| 项目 | 语义 |
|---|---|
| `ready()` | 构造期配置是否可用（加锁读取） |
| `initializationError()` | 配置不可用时的可定位原因；成功时为空串 |
| 构造失败行为 | **不抛异常**；模块可构造但不接受任何目标（返回 `REJECTED_CONFIG`） |
| `stop()` | 幂等；撤销目标并请求零力矩；清除本地 `FAULT` 锁存以便上层恢复 |
| 析构 | ATOM-09 不持资源（无设备句柄、无线程、无文件），析构无需特殊处理；装配层只需保证注入对象的寿命更长 |

## 12. 验证责任

ATOM-09 侧可复查的静态项：公共头依赖方向（不得包含 ATOM 私有头 / `AppCoordinator.hpp`）、无 `main()`、无独立可执行目标、CMake 目标与别名命名、`update()` 无阻塞 I/O、全部方法 `noexcept`。行为判定表见 `../Atoms/ATOM-08_Control` §7。

ARM 目标编译（WSL aarch64 交叉编译或目标机编译，增量、`-Wall -Wextra -Wpedantic` 零警告）由装配层/集成负责人执行，本模块不单独编译。

## 13. 对 GeneralBugs/20261015-Entry.md 建议条款的立场

| 报告条款 | ATOM-09 立场 |
|---|---|
| §7.1 回滚 `Entry.cpp` 到 ATOM-07 后稳定状态 | ATOM-08/10/11 部分回滚合理；**ATOM-09 部分无需回滚等待架构决策**，按本文 §4 调整构造顺序即可接入，建议与另三个模块分开处理 |
| §5 方案 A（延迟构造） | 赞同用于 ATOM-08/10/11；对 ATOM-09 **不必要**（无生命周期适配器，天然支持在 `AppCoordinator` 之后构造） |
| §5 方案 B（提前创建 `AppCoordinator`） | 反对，架构冲击过大 |
| §5 方案 C（Setter 注入） | 反对，违反「不修改 ATOM 代码」约束并引入半初始化窗口 |
| §7.2 更新 `Rule/AtomIntegration.md` | 赞同，建议补充 §14 的分类说明 |

## 14. 对 `Rule/AtomIntegration.md` 的补充建议

建议在接入规范中明确区分两类模块，避免 ATOM-12/13/14/18 重复同类困惑：

1. **生命周期模块**：注册 `RuntimeStep`，须在 `AppCoordinator` 构造前完成注册；若其内核需要 `AppCoordinator` 接口，应采用工厂函数延迟构造（方案 A）。
2. **纯逻辑内核**（不注册生命周期，如 ATOM-09）：由装配层在 `AppCoordinator` 之后构造并注入依赖，由装配层周期线程驱动 `update()`，并遵守本文 §5.3 的启停时序与 §6 的并发边界。接入规范应写明「构造时机 = `AppCoordinator` 之后、周期线程启动之前」。

## 15. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-10-15 | 「接入结论 —— 对 GeneralBugs/20261015-Entry.md 的答复」。给出无循环依赖结论、构造时机、周期驱动与适配清单；无 CMake / 公共契约 / 启停时序 / 并发 / 配置键 / 数据流 / 验证责任 / 版本记录等章节 |
| 1.0 | 2026-10-16 | 重写为正式工程接入说明，对齐 `ATOM3/4/5ConnectProject.md` 模板：新增 CMake 接入、公共契约清单、就绪检查、**启停时序（P0）**、**并发与线程安全边界（P0）**、配置键全表、数据流与线程流、调用方契约与开放问题、关闭语义、验证责任、版本记录；保留原答复的技术结论与对报告的立场。**未修改 ATOM-09 任何代码。** |

