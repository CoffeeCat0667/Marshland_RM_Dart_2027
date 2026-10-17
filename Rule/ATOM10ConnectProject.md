# ATOM10ConnectProject — ATOM-10 换弹与供弹接入规则

| 项目 | 内容 |
|---|---|
| 模块 | ATOM-10 换弹与供弹机构控制（Marshland） |
| 模块目录 | `../Atoms/ATOM-08_Control` |
| 本文档版本 | 1.1 |
| 日期 | 2026-10-16 |
| 设计基线 | PRD 0.10、PEF 1.5、Interface 1.8、Config 1.4、Atoms 0.12、ConfigSchema **1.9** |
| 关联文档 | `Rule/AtomIntegration.md`、`Rule/ConfigSchema.md`、`../Atoms/ATOM-08_Control`、`Atoms/Public/GeneralBugs/20261015-Entry.md` |

## 1. 模块目标与 CMake

1. 模块目标名 `atom_10_reload_feed`，别名 `Marshland::atom_10_reload_feed`；仅装配层可链接该别名，
   **其他 ATOM 禁止链接本模块目标**。
2. `Src/` 非空，目标为 **STATIC** 库；不定义 `project()`、不产生可执行文件、无 `main()`。
3. 依赖：文件尾部按 `if(TARGET Marshland::PublicHeaders)` 声明公共契约依赖；模块无线程，
   不需要 `Threads::Threads`；不设置任何编译器/架构/C++ 标准/编译选项。
4. 上层 `Atoms/CMakeLists.txt` 需 `add_subdirectory(ATOM-10_Reload_Feed)`（聚合状态见
   PLAN.md P-06，接入前核实）。

## 2. 目录结构与产物

```
ATOM-10_Reload_Feed/
├─ Inc/atom10_reload_feed/
│   ├─ reload_feed_types.hpp        # ReloadSide/ReloadAction/ReloadState/FeedState、命令结果与状态结构
│   ├─ reload_feed_interfaces.hpp   # IReloadMotorPair、IFeedActuator 窄接口（能力声明，无实现）
│   ├─ reload_feed_config.hpp       # ReloadFeedConfig、LoadReloadFeedConfig(IConfigView)
│   ├─ reload_feed_core.hpp         # ReloadFeedCore 纯逻辑内核
│   └─ lifecycle_adapter.hpp        # ReloadFeedLifecycleAdapter（唯一 ILifecycleModule 实现）
└─ Src/
    ├─ reload_feed_config.cpp
    ├─ reload_feed_core.cpp
    └─ lifecycle_adapter.cpp        # 薄壳（≤ 50 行，无业务逻辑）
```

包含约定：本模块头文件为**装配面**（assembly-facing），仅装配层（Entry 等）以
`atom10_reload_feed/<头文件>` 形式包含；**其他 ATOM 禁止包含本模块任何头文件**。
公共契约（`ILifecycle.hpp`、`IConfigView.hpp`、`IMonotonicClock.hpp`、`AppInterfaces.hpp`、
`MarshlandTypes.hpp`、`StateArbitrationTypes.hpp`）经 `Marshland::PublicHeaders` 传递。
本模块禁止包含任何其他 ATOM 的私有头或 `AppCoordinator.hpp`。

## 3. 配置键（经 `IConfigView` 逐项读取并检查 `ConfigStatus`）

| 键 | 类型/单位 | 设计值 | 用途 |
|---|---|---|---|
| `reload.speed_rpm` | int / rpm | 2000（>0） | 换弹速度目标**幅值**；方向为模块内约定（抬起左正右负、放下左负右正） |
| `timing.reload_lift_run_s` | int / s | 3 | 抬起运行时间 |
| `timing.reload_lower_run_s` | int / s | 3 | 放下运行时间 |
| `feed.hold_ms` | int / ms | 100 | 供弹换弹位保持时间 |
| `timing.feed_max_action_ms` | int / ms | 1000 | 供弹最大动作时间（超时判失败） |

约束：任一键非 `ConfigStatus::OK` 或越界即初始化失败，如实返回键名与状态；
禁止臆造默认值、禁止为 `[待定]` 项填值；`configVersion()` 变化后由装配层重新装载并调
`ReloadFeedCore::updateConfig()`。ATOM-10 不读取 `motor[]` 映射与 PID（归 ATOM-05）、
不读取 `timing.feed_to_reload_lower_delay_s`（2 s 等待归 ATOM-13/PEFExecutor）。

## 4. 窄接口与装配职责

ATOM-10 只声明**消费的**能力（方向：ATOM-10 → 他模块资源），实现全部由装配层注入：

| 窄接口 | 语义 | 装配层实现方 |
|---|---|---|
| `IReloadMotorPair` | `submitVelocityRpm(side, rpm)` / `requestZeroTorque(side)` / `isSideUsable(side)` | 适配 ATOM-05 |
| `IFeedActuator` | `moveToReloadPosition()` / `moveToStandbyPosition()`（位置模型，不用角度） | 适配供弹执行器驱动；**供弹硬件已变更（2026-10-15 用户裁决），具体实现方待 Design 修订后确定**（原路径为 ATOM-07） |

`IReloadMotorPair` 契约（与 `ATOM5ConnectProject.md` §9 对齐）：

1. 分侧提交速度目标；任一侧被拒绝或失败，由 ATOM-10 撤销整对目标并两侧零力矩；
2. 两个调用不是事务，不承诺跨电机物理原子写入或回滚；
3. 接口实现内**没有**换弹动作计时器，计时由 ATOM-10 负责；
4. 正常到位必须提交 `VELOCITY = 0`（有效闭环目标），**不得**以 `DISABLED`/零力矩替代；
5. 零转速闭环 ≠ 零力矩：正常到位为前者，手动停止/急停/故障为后者。

## 5. 线程模型（2026-10-16 用户裁决）

1. 本模块为**纯逻辑 ATOM**：只暴露装配面，**不向任何其他 ATOM 提供接口**；
   跨模块请求全部表现为"本模块经窄接口向他模块资源发出请求"。
2. 本模块内部**不创建任何线程**；装配层在装配时为本模块创建**工作线程**，
   关闭时由装配层结束该线程。
3. 内核的全部公开方法（动作命令、事件通知、`poll()`、状态查询）都由装配层的
   本模块工作线程调用；经窄接口发出的请求（电机、供弹、仲裁、故障上报）均从该线程发出。
4. 内核非线程安全；装配层须保证对同一内核实例的单线程调用。
5. 因此内核方法不使用互斥锁；若装配层需要跨线程观察状态，由装配层自行同步。

## 6. AppCoordinator 晚绑定（方案 C）

依据 `Atoms/Public/GeneralBugs/20261015-Entry.md` 的架构裁决：`IControlAuthority` /
`IInputReporter` 仅由 `AppCoordinator` 实现，而 `AppCoordinator` 晚于模块注册才能构造，
因此 ATOM-10 采用**晚绑定**：

1. `ReloadFeedCore::Dependencies` 只含早期可用依赖（`IReloadMotorPair&`、`IFeedActuator&`、
   `app::IMonotonicClock&`）；
2. 装配层在 `AppCoordinator` 构造后、`Start()` 前调用：
   - `core.bindControlAuthority(app)`（必需）；
   - `core.bindFaultReporter(&app)`（可选，故障上报；允许不绑定）；
3. 未绑定期间，所有需仲裁的命令按 `IntentRejected`（`RejectReason::NOT_READY`）拒绝
   （fail-safe）；零力矩等安全输出不受绑定状态影响，始终允许；
4. `initialize()` 路径（配置装载 + 两侧 0 rpm 初始目标）不依赖 `AppCoordinator`，
   可在绑定前或后执行，均安全。

该约定同样适用于面临同一循环依赖的 ATOM-08/09/11（及后续 ATOM-12/13/14/18），
建议在 `Rule/AtomIntegration.md` 中固化为统一接入规范。

## 7. 生命周期注册

1. 适配器注册步骤为 `RuntimeStep::InitializeServoOutputs`（与 ATOM-11 共用同一步骤，
   同步步骤内按注册先后 FIFO）；
2. `required` 取值由装配层/启动策略显式决定（ATOM-10 自身不持有硬件资源）；
3. 注册只能在装配层完成：`runtime.addModule(...)` 返回 `false` 时必须读取
   `lastRegistrationError()`；ATOM-10 不自行注册，也不注册其他模块；
4. 顺序依赖：ATOM-04 → ATOM-05 → ATOM-10；ATOM-07 → ATOM-10（`AtomIntegration.md` §6.3）。

## 8. 命令与事件驱动（全部由装配层工作线程发起）

**其他 ATOM 不调用本模块的任何方法。** 装配层的 ATOM-10 工作线程按下表观察仲裁状态
与事件，并驱动内核：

| 驱动来源（装配层观察） | 内核接口 | 说明 |
|---|---|---|
| `AutoFireState` 进入 `RELOAD_LIFT` | `requestLift(ControlSource::AUTO, IntentKind::CONTROL, request_id)` | 自动流程换弹抬起 |
| `AutoFireState` 进入 `FEED` | `requestFeedOnce(...)` | 自动流程供弹 |
| `AutoFireState` 进入 `RELOAD_LOWER` | `requestLower(...)` | 自动流程换弹放下 |
| 供弹 2 s 等待 | —（无接口） | **触发时刻即仲裁状态 `FEED → FEED_WAIT` 的转换时刻**（2026-10-16 裁决），ATOM-13 从状态转换事件自取计时起点；ATOM-10 不提供跨模块数据通道 |
| 安全恢复阶段（ATOM-14 编排） | `requestLift(source, IntentKind::SAFETY_RECOVERY, request_id)` | 恢复抬起；再次失败由 ATOM-14 进入 `FAULT` 锁定（装配层经 `reloadStatus()` 观察 `last_failed_action`/`completed_actions`）；ATOM-10 不自动重试 |
| WEB/SBUS 手动动作意图 | `requestLift` / `requestLower` / `requestFeedOnce`（`CONTROL`） | 对应动作 `reload_lift`/`reload_lower`/`feed_once` |
| 手动停止 | `requestManualStop(source)` | 取消动作与计时，两侧零力矩；不等待动作组完成边界 |
| 软件急停锁定/解除 | `notifySoftwareEstopLocked()` / `notifySoftwareEstopReleased()` | 锁定：取消一切、两侧零力矩、自闭锁普通命令 |
| ATOM-05 换弹侧故障上报 | `notifySideFault(side)` | 单侧离线/反馈/PID 异常 → 两侧零力矩、取消动作与计时 |
| 控制周期调度（≤ 100 ms） | `poll()` | 推进运行计时收尾与供弹时序 |
| 机构状态上报 | `reloadStatus()` / `feedStatus()` | 由装配层汇入 `mechanism_state`；`action_in_progress()` 为控制源接管边界依据 |

## 9. 安全语义（验收核对用）

1. 需裁决的动作先 `SubmitIntent` 并检查 `accepted`；`accepted == false` 不得当作成功；
2. 意图分类：零力矩 `SAFETY`（记录性质，命令无条件发出，fail-safe）、恢复抬起
   `SAFETY_RECOVERY`、普通动作 `CONTROL`；
3. 急停锁定期间模块**自身**拒绝普通出力命令（`EstopLocked`）；机构故障态拒绝普通命令
   （`FaultLatched`）；不把安全寄托于调用方查询 `RunGate()`；
4. 两台运行目标均成功提交后才开始单调时钟计时，不以反馈到目标为起点；
5. 计时到期提交两侧 0 rpm，均成功且动作未取消/失败才判定到位；迟到完成以动作代数拒绝；
6. 不设置换弹动作超时；不执行换弹校准/回零；本版本不接入换弹限位；启动逻辑默认抬起，
   初始化后两侧 0 rpm，不主动抬起；
7. 供弹写入失败（如 IIC 失败）不得报告供弹完成；超过 `feed_max_action_ms` 判供弹失败；
8. 动作失败/供弹超时经 `IInputReporter::ReportFault(true, SEVERE)` 上报（对应
   F-RELOAD-001 条件）；换弹故障不冒充舵机 IIC 故障。

## 10. 接入自检清单

- [ ] `Atoms/CMakeLists.txt` 已 `add_subdirectory(ATOM-10_Reload_Feed)`；
- [ ] **其他 ATOM 未包含本模块头文件、未链接本模块目标**（纯逻辑 ATOM 只暴露装配面）；
- [ ] **装配层已创建本模块工作线程并承担其销毁**；命令/事件/`poll()` 全部来自该同一线程；
- [ ] `ConfigSchema.schema.json` 与初始 `dart_control.json` 已迁移（含 `reload.speed_rpm`，
      移除旧 `servo_mechanisms.reload[]` 与供弹角度字段）——否则配置装载会如实失败；
- [ ] 装配层已提供 `IReloadMotorPair` / `IFeedActuator` 实现；
- [ ] `bindControlAuthority()` 已在 `AppCoordinator` 构造后、`Start()` 前调用；
- [ ] `poll()` 已接入控制周期调度（≤ 100 ms）；
- [ ] ARM 增量构建（WSL aarch64 或目标机）通过，`-Wall -Wextra -Wpedantic` 零警告。

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-15 | 首次编制。固化 ATOM-10 接入规则：模块目标、目录结构、配置键（ConfigSchema v1.9）、窄接口契约、AppCoordinator 晚绑定（方案 C）、生命周期注册、命令与事件接线、安全语义与接入自检清单。 |
| 1.1 | 2026-10-16 | 按用户裁决新增 §5 线程模型：纯逻辑 ATOM 只暴露装配面、不向其他 ATOM 提供接口，工作线程由装配层创建与销毁；§8 命令与事件接线全部改为装配层工作线程驱动（其他 ATOM 不再作为调用方）；供弹 2 s 计时起点改为仲裁状态 `FEED → FEED_WAIT` 转换时刻（无跨模块数据通道）；自检清单增加禁止他模块包含/链接、线程归属与单线程调用三项。 |
