# ATOM10ConnectProject — ATOM-10 换弹与供弹接入规则

| 项目 | 内容 |
|---|---|
| 模块 | ATOM-10 换弹与供弹机构控制（Marshland） |
| 模块目录 | `Atoms/ATOM-10_Reload_Feed/` |
| 本文档版本 | 1.0 |
| 日期 | 2026-10-15 |
| 设计基线 | PRD 0.10、PEF 1.5、Interface 1.8、Config 1.4、Atoms 0.12、ConfigSchema **1.9** |
| 关联文档 | `Rule/AtomIntegration.md`、`Rule/ConfigSchema.md`、`Atoms/ATOM-10_Reload_Feed/PLAN.md`、`Atoms/Public/GeneralBugs/20261015-Entry.md` |

## 1. 模块目标与 CMake

1. 模块目标名 `atom_10_reload_feed`，别名 `Marshland::atom_10_reload_feed`；链接一律使用别名。
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

包含约定：消费方以 `atom10_reload_feed/<头文件>` 形式包含；公共契约（`ILifecycle.hpp`、
`IConfigView.hpp`、`IMonotonicClock.hpp`、`AppInterfaces.hpp`、`MarshlandTypes.hpp`、
`StateArbitrationTypes.hpp`）经 `Marshland::PublicHeaders` 传递。
禁止包含任何其他 ATOM 的私有头或 `AppCoordinator.hpp`。

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

ATOM-10 只声明能力，实现全部由装配层注入：

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

## 5. AppCoordinator 晚绑定（方案 C）

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

## 6. 生命周期注册

1. 适配器注册步骤为 `RuntimeStep::InitializeServoOutputs`（与 ATOM-11 共用同一步骤，
   同步步骤内按注册先后 FIFO）；
2. `required` 取值由装配层/启动策略显式决定（ATOM-10 自身不持有硬件资源）；
3. 注册只能在装配层完成：`runtime.addModule(...)` 返回 `false` 时必须读取
   `lastRegistrationError()`；ATOM-10 不自行注册，也不注册其他模块；
4. 顺序依赖：ATOM-04 → ATOM-05 → ATOM-10；ATOM-07 → ATOM-10（`AtomIntegration.md` §6.3）。

## 7. 命令与事件接线

| 调用方 | 接口 | 说明 |
|---|---|---|
| ATOM-13（自动流程） | `requestLift(source, IntentKind::CONTROL, request_id)` / `requestFeedOnce(...)` / `requestLower(...)` | 供弹 2 s 等待由 ATOM-13/PEFExecutor 自 `FeedStatus::last_trigger_micros` 起独立计时 |
| ATOM-14（安全恢复） | `requestLift(source, IntentKind::SAFETY_RECOVERY, request_id)` | 恢复抬起再次失败由 ATOM-14 进入 `FAULT` 锁定；ATOM-10 不自动重试 |
| 手动/调试动作 | `requestLift/requestLower/requestFeedOnce`（`CONTROL`） | 对应 WEB action `reload_lift`/`reload_lower`/`feed_once` |
| 手动停止 | `requestManualStop(source)` | 取消动作与计时，两侧零力矩；不等待动作组完成边界 |
| 仲裁/安全事件 | `notifySoftwareEstopLocked()` / `notifySoftwareEstopReleased()` | 急停锁定：取消一切、两侧零力矩、自闭锁普通命令 |
| ATOM-05 故障联动 | `notifySideFault(side)` | 单侧离线/反馈/PID 异常 → 两侧零力矩、取消动作与计时 |
| 周期驱动 | `poll()` | 由装配层按控制周期（≤ 100 ms）调用，推进运行计时收尾与供弹时序 |
| 状态查询 | `reloadStatus()` / `feedStatus()` | `action_in_progress()` 为控制源接管边界依据；完成次数单调递增 |

线程约定：内核非线程安全；命令、事件通知与 `poll()` 须由同一线程调用。

## 8. 安全语义（验收核对用）

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

## 9. 接入自检清单

- [ ] `Atoms/CMakeLists.txt` 已 `add_subdirectory(ATOM-10_Reload_Feed)`；
- [ ] `ConfigSchema.schema.json` 与初始 `dart_control.json` 已迁移（含 `reload.speed_rpm`，
      移除旧 `servo_mechanisms.reload[]` 与供弹角度字段）——否则配置装载会如实失败；
- [ ] 装配层已提供 `IReloadMotorPair` / `IFeedActuator` 实现；
- [ ] `bindControlAuthority()` 已在 `AppCoordinator` 构造后、`Start()` 前调用；
- [ ] `poll()` 已接入控制周期调度（≤ 100 ms）；
- [ ] ARM 增量构建（WSL aarch64 或目标机）通过，`-Wall -Wextra -Wpedantic` 零警告。

## 10. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-15 | 首次编制。固化 ATOM-10 接入规则：模块目标、目录结构、配置键（ConfigSchema v1.9）、窄接口契约、AppCoordinator 晚绑定（方案 C）、生命周期注册、命令与事件接线、安全语义与接入自检清单。 |
