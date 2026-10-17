# ATOM-11_Fire 工程接入说明

> 版本：1.0
> 日期：2026-10-15
> 适用模块：`../Atoms/ATOM-08_Control`
> 关联文档：`../Atoms/ATOM-08_Control`（v0.5）、`Atoms/Public/GeneralBugs/20261015-Entry.md`

## 1. 接入目标

ATOM-11 负责对发射机构舵机（`mechanism == "FIRE"`，PCA9685A 通道 5）执行一次发射动作：校验急停/故障/授权 → 写发射角 → 保持计时（100 ms）→ 写回待机角 → 待机角写回成功才判 `COMPLETED` 并恰好发布一次完成事件。

模块不决定发射时机（调用编排归 ATOM-13/ATOM-14），不做授权判定（判定归 ATOM-02，本模块只查询结论），不直接访问 IIC/PCA9685A 寄存器（归 ATOM-07），不在模块内部实现 `main()`，不实现 CTest/测试宿主（属上层）。

## 2. 核心结论：ATOM-11 零修改即可接入

针对 `Atoms/Public/GeneralBugs/20261015-Entry.md` 提出的「ATOM-08/09/10/11 构造依赖 AppCoordinator ↔ AppCoordinator 依赖注册完成」循环依赖报告，ATOM-11 侧的结论是：

**该循环对 ATOM-11 不成立，本模块无需任何代码修改即可接入。** 依据：

1. 报告所依赖的前提「`AppCoordinator` 必须在所有模块注册完成后才能构造」未被证实。`AppCoordinator` 构造函数（`Atoms/Public/AppCoordinator/Src/AppCoordinator.cpp:29-31`）只做成员引用初始化，不读取 `RuntimeCoordinator` 内部状态；真正的约束仅是 `Start()` 必须在注册完成后调用。
2. 因此装配层只需把 `AppCoordinator` 的构造**提前到模块注册之前**（Start 时序不变），ATOM-11 的构造函数依赖（`IStateReader&` / `IControlAuthority&`）即可在注册点正常获得。对 ATOM-11 是**零修改**，对 Entry.cpp 是构造顺序调整，不涉及任何架构重构。
3. 报告的「尝试 2」失败是因为把 `AppCoordinator` 放在注册**之后**构造；提前构造的路径未被尝试即被排除。

**前置验证项**（装配层责任，一行即可确认）：`AppCoordinator` 构造函数除引用初始化外无任何对 `RuntimeCoordinator` 的状态读取。若该前提被证伪（构造函数未来改为读取注册状态），启用 §10 备选方案。

## 3. CMake 接入方向

```text
Marshland executable
  -> Atoms/Public/AppCoordinator（顶层装配）
  -> Marshland::atom_11_fire
  -> Marshland::atom_07_servo_driver（用户裁决：直连，按 ServoMechanism::Fire 寻址）
  -> Marshland::PublicHeaders
```

要求：

1. 上层工程先建立 `Marshland::PublicHeaders` 与 `Marshland::atom_07_servo_driver`。
2. 上层工程通过 `add_subdirectory(Atoms/ATOM-11_Fire)` 加入本模块（已在 `Atoms/CMakeLists.txt` 聚合中）。
3. ATOM-11 提供目标 `atom_11_fire` 和别名 `Marshland::atom_11_fire`，并链接上述两个依赖。
4. 上层工程负责决定 ARM 工具链、C++ 标准、警告选项和最终可执行文件。
5. 本模块不定义 `project()`，不创建可执行文件，不设置全局编译选项。

## 4. 公共契约

ATOM-11 **不新增** `Atoms/Public` 任何头文件（用户裁决）。复用既有契约：

- `IConfigView.hpp`：配置查询；
- `AppInterfaces.hpp`：`IStateReader`（读控制权与安全事实）、`IControlAuthority`（查询发射授权结论）；
- `StateArbitrationTypes.hpp`：`FireAuthorization` 授权结论；
- `MarshlandTypes.hpp`：`ControlSource` 等基础类型；
- `atom07_servo_driver/servo_core.hpp`（ATOM-07 公开头）：`ServoCore::writeTarget(ServoMechanism::Fire, ...)`。

本模块自持两个窄端口（`Inc/atom11_fire/` 内，不上提 Public）：

- `IServoAngleWriter`：内核 → 适配层，由适配层直接转发到 `ServoCore::writeTarget(ServoMechanism::Fire, ...)`；
- `IFireEventOutlet`：适配层 → 装配层，完成事件与故障事实（`F-SERVO-001` / `W-CMD-001`）出口，装配层实现并最终委托 `IInputReporter::ReportFault(...)` 与相应事件消费方（ATOM-13/17/18）。

## 5. 装配对象图

由 `Entry.cpp` 或等价顶层装配代码创建并持有以下对象，创建顺序必须保证依赖对象寿命更长：

```text
StateArbitrator
IMonotonicClock 实现
IConfigView 实现
RuntimeCoordinator
AppCoordinator(RuntimeCoordinator&, StateArbitrator&, IMonotonicClock&)   // ★ 提前到模块注册之前构造（§2）
ServoCore（ATOM-07，装配层持有实例）
atom11::LifecycleAdapter(
    IConfigView&,                  // 配置查询
    const IStateReader&,           // = AppCoordinator
    IControlAuthority&,            // = AppCoordinator
    ServoCore&,                    // 与 ATOM-07 适配器共享同一实例
    IFireEventOutlet*              // 装配层实现；可空，为空时事件与故障事实不上报
)
```

装配层必须保证 `AppCoordinator`、`ServoCore`、配置视图与事件出口的寿命长于适配器。`ServoCore` 实例与 ATOM-07 的适配器共享，无需改动 ATOM-07。

## 6. 生命周期注册

上层装配层将 `atom11::LifecycleAdapter` 注册到：

```text
RuntimeStep::InitializeServoOutputs
```

注册时必须：

- 显式填写 `required`（ATOM-11 属硬件/安全相关模块，**应**为 `true`；fire 角度配置已落地，取 `true` 不会因配置缺失导致启动失败）；
- 检查 `addModule()` 返回值；
- 失败时读取 `lastRegistrationError()`；
- 不在 ATOM-11 内自行注册；
- 顺序依赖：ATOM-07（`InitializeIicPca9685`）必须先于本步骤完成。

## 7. 配置接入

配置通过装配层注入的 `marshland::config::IConfigView` 查询，不由 ATOM-11 打开或解析 JSON。发射舵机为 PWM 舵机、无 id（用户裁决），槽位按 `mechanism == "FIRE"` 遍历定位，禁止硬编码下标。

本模块读取：

| 键 | 类型/范围 | 用途 | 当前值 |
|---|---|---|---|
| `servo.channels[FIRE].standby_deg` | int，0～180 | 待机角 | 90（已落地） |
| `servo.channels[FIRE].action_deg` | int，0～180 | 发射角 | 180（已落地） |
| `timing.fire_angle_hold_ms` | 正整数 | 发射角保持时间（唯一权威键；`fire.hold_ms` 不读取） | 100 |
| `timing.fire_max_action_ms` | 正整数 | 最大动作时间，超出判 `F-SERVO-001` | 300 |

缺失、类型错误或越界配置必须使生命周期初始化失败（如实返回 `success = false`），禁止臆造默认值。槽位 `id` 为 `null` 时 `getIntAt` 返回 `INVALID_TYPE`，本模块不读取该键，不视为失败。

## 8. 数据和调用流

```text
调用方（ATOM-13 四发流程 / ATOM-14 三次恢复 / 手动路径）
  -> LifecycleAdapter::beginOnce(now_us) / tick(now_us)   // 显式单调时间，外部 tick 驱动
  -> 适配层 readFacts()：
       IStateReader::ReadState()（租约内只拷贝 control_source / estop / fault，释放后再调端口）
       -> 按 control_source：SBUS/WEB -> IControlAuthority::AuthorizeManualFire(source)
                             AUTO    -> IControlAuthority::AuthorizeFire(AUTO)
       -> 组装 FireFacts
  -> FireActionCore 状态机（校验 -> 写发射角 -> HOLDING -> 写回待机角 -> COMPLETED）
  -> IServoAngleWriter::writeFireTarget -> ServoCore::writeTarget(ServoMechanism::Fire, ...)
  -> 完成事件 / 故障事实 -> IFireEventOutlet -> 装配层出口
```

约束：

- 一次动作只能产生一次完成事件；`FAILED` 不产生完成事件；
- 动作进行中再次 `beginOnce` 返回 `REJECTED_NOT_READY`（不自行重复发射；三次恢复由 ATOM-14 逐次调用并确认）；
- 动作进行中每个 `tick` 复检急停/故障，出现时不写任何新目标并判 `FAILED`；
- 手动、自动与 PEF 故障恢复复用同一动作行为，无调试专用发射路径。

## 9. 控制和安全边界

- 不决定是否允许发射，不提交 `ControlIntent`，不包含 `AppCoordinator.hpp` / `StateArbitrator` 具体头，不调用 `UpdateInputs()` 或字段 setter；
- 授权判定由 ATOM-02 做出，本模块仅经 Public 契约查询结论；急停事实传入时内核自身拒绝写入舵机目标，不把安全寄托于调用方先查 `RunGate()`；
- 不改变其他电机/舵机目标；仅为 `ServoMechanism::Fire` 单通道产生目标；
- 不直接访问 IIC/PCA9685A 寄存器，不发送 CAN 报文；
- 事件出口为空（`outlet == nullptr`）时动作行为不受影响，仅不上报——装配层应尽快接合出口，避免故障事实滞留。

## 10. 备选方案（仅当 §2 前提被证伪时启用）

若未来 `AppCoordinator` 构造函数改为读取 `RuntimeCoordinator` 注册状态，导致提前构造不可行，ATOM-11 支持晚期绑定小改（约 20 行，不动内核、不动安全语义）：

1. 构造函数只保留早期可得依赖（`IConfigView&` + `ServoCore&`）；
2. 增加 `bindRuntime(const IStateReader*, IControlAuthority*, IFireEventOutlet*)`，在 `AppCoordinator` 构造后、`Start()` 前调用；
3. 未绑定时 `beginOnce` / `tick` 返回 `REJECTED_NOT_READY`（fail-closed，急停/授权语义不变）。

该方案与 ATOM-07 `ServoCore` 既有的可空窄端口注入先例一致。**当前不实施**，以 §2 零修改路径为准。

## 11. 验证责任

本模块静态验证至少包括：

- Public 依赖方向和头文件边界（仅 `Atoms/Public` + `atom07_servo_driver` 公开头）；
- 状态机逐步结果与判定表（`PLAN.md` §7 共 15 项场景）；
- 恰好一次完成事件防护；
- 配置键读取、`ConfigStatus` 检查与越界失败；
- 幂等 `shutdown`、无 `main()`、无独立可执行目标；
- CMake 目标与别名命名、两个依赖链接块；
- aarch64 交叉编译零警告（已完成，仅增量编译）。

运行时验证（14 项判定场景、端到端发射流程）由上层测试宿主与目标机联调承担，不属本模块职责。

## 12. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-15 | 初版：记录 ATOM-11 接入结论——针对 `GeneralBugs/20261015-Entry.md` 的循环依赖报告，本模块零修改即可接入（`AppCoordinator` 提前到注册前构造，§2）；补齐 CMake、装配对象图、注册、配置键、调用流、安全边界与备选方案（§10，当前不实施）。 |
