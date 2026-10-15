# ATOM-11_Fire 实现计划（Coding Plan）

> 状态：**代码已实施**（`Inc/atom11_fire/` 5 个头文件 + `Src/` 3 个实现文件）；aarch64 增量编译通过，`-Wall -Wextra -Wpedantic` 零警告（2026-10-15，WSL `aarch64-linux-gnu-g++` 13.3）。
> 文档版本：0.5
> 更新日期：2026-10-15
> 同步基线：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8；`Rule/AtomIntegration.md` 1.3；`Rule/ConfigSchema.md` 1.4；`Design/Atomic/ATOM-11_Fire.md`（2026-10-10 基线，已复核无变更）
> 验证方式：静态检查与判定表；本模块不建立独立可执行测试入口（`Rule/AtomIntegration.md` §9.5）；CTest 属上层内容，本模块不实现（用户裁决 #7）
> 授权：本轮（2026-10-15）用户授权实施代码并更新本 `PLAN.md`。

## 1. 本次更新说明

本轮（0.2）按用户授权只读读取以下内容，未修改其中任何文件：

| 范围 | 用途 |
|---|---|
| `Design/Atomic/ATOM-11_Fire.md` | 本模块单一职责、原子步骤、安全约束、验收条件 |
| `Design/Atomic/ATOM-02/07/13/14_*.md` | 授权边界、舵机底层、四发流程调用方、三次发射恢复调用方 |
| `Design/Atoms.md`、`Design/LifecycleEvents.md` | 19 个 Atom 索引与公共事件契约 |
| `Design/Interface.md` §6.5、§12.3 | `FireAction` 数据与舵机动作接口（`FireOnce()`） |
| `Design/Config.md` §4、§5.6、§9.2、§9.3、§14.1、§14.2 | 时序、舵机参数、安全规则与故障码 |
| `Rule/AtomIntegration.md` 1.3 | 依赖方向、内核+适配器两层、注册、三个契约面、配置、时间与线程、构建验证、自检清单 |
| `Rule/ConfigSchema.md` 1.4 | 键名唯一来源、`fire.*` / `timing.*` / `servo.strategy.*` 键表 |
| `Rule/ATOM3ConnectProject.md` 与 `Atoms/ATOM-03_SBUS`（只读） | ATOM 通用接入方式与既有实现范式 |
| `Atoms/Public/`（本工作目录允许范围） | `ILifecycle.hpp`、`IConfigView.hpp`、`AppInterfaces.hpp`、`StateArbitrationTypes.hpp`、`MarshlandTypes.hpp`、`IMonotonicClock.hpp` |
| `NewVersion/CMakeLists.txt`、`Atoms/CMakeLists.txt`、`Atoms/Public/*/CMakeLists.txt`（只读） | 实际构建接入点与目标名 |

### 1.1 复核说明（2026-10-15，v0.3）

本轮按用户授权只读复核了例外允许项 `Design/Atomic/ATOM-11_Fire.md`（单一职责、6 个原子步骤、安全约束、验收条件），结论：职责、步骤、安全约束与本计划一致，无冲突、无超范围；核实本目录 `CMakeLists.txt` 与 §4.7 描述一致。

### 1.2 用户裁决记录（2026-10-15，v0.4 / v0.5）

用户逐项裁决了 §9（v0.3）的全部外部决策项，并授权只读核查 `Config/dart_control.json`、`Rule/ConfigSchema.schema.json`、`Atoms/ATOM-07_Servo_Driver/Inc/`、`Atoms/Public/AppInterfaces.hpp`、`Atoms/Public/StateArbitrationTypes.hpp`（只读，未修改）：

| # | 裁决 | 内容与落地方式 |
|---|---|---|
| 1 | A 类事项维持原计划 | `timing.fire_angle_hold_ms` 为保持时间唯一权威键；内核接收显式 `now_us`；外部 tick 驱动，不阻塞主循环 |
| 2 | `fire.servo_id` 取消 | PWM 舵机无 id，本模块不读取该键；发射舵机按 `mechanism == "FIRE"` 寻址（`dart_control.json` `servo.channels` 中通道 5，`id: null` 是 ATOM-07 的既定设计场景） |
| 3 | 发射角度已裁决 | 待机角 90、发射角 180；落地方式经裁决 #8 调整为 `servo.channels` FIRE 槽位 |
| 4 | 舵机写入直连 ATOM-07 | 直接链接 `Marshland::atom_07_servo_driver`，调用 `ServoCore::writeTarget(ServoMechanism::Fire, angle_deg)`；`ServoCore` 实例由装配层注入；原「自持窄接口待适配」方案作废 |
| 5 | 安全事实与授权自查 | 经 Public `IStateReader::ReadState()` 读当前控制权（`control_source`）与急停/故障事实；按持有控制权的来源经 `IControlAuthority::AuthorizeManualFire(ControlSource)`（SBUS/WEB）或 `AuthorizeFire(ControlSource)`（AUTO）查询 `FireAuthorization`；原「调用方注入授权」方案作废 |
| 6 | 不新增 Public 接口 | `IStateReader` / `IControlAuthority` / `FireAuthorization` 均已在 `Atoms/Public`，无需任何新增公共类型 |
| 7 | CTest 不实现 | 测试宿主与 CTest 集成属上层内容，本模块只提供可注入替身的内核 |
| 8 | 不新增 `servo_mechanisms` 段（v0.5） | 发射角度经 `servo.channels` 中 `mechanism == "FIRE"` 槽位的 `standby_deg`（90）/ `action_deg`（180）读取，**配置已落地**（`dart_control.json` `servo.channels[1]`）；`fire.standby_deg` / `fire.fire_deg` 逻辑键需求作废；槽位按 mechanism 遍历定位，禁止硬编码下标；`id` 为 `null`（`getIntAt` 返回 `INVALID_TYPE`），本模块不读取 |

本次实际修改/新增：本 `PLAN.md`、`CMakeLists.txt`（补链接块）、`Inc/atom11_fire/` 5 个头文件、`Src/` 3 个实现文件。

## 2. 与架构设计的一致性检查（`NewVersion/AGENTS.md` 第 3 节）

1. 需求来源：`Design/Atomic/ATOM-11_Fire.md` 的单一职责是对发射机构配置的舵机提供一次发射动作及结果状态，与本计划 §3 冻结范围一致。
2. 调用边界：授权判定不属于本模块，仍由 `ATOM-02` 的 `StateArbitrator` 做出；本模块仅经 Public 契约 `IControlAuthority` **查询**授权结论（用户裁决 #5），不自行裁决发射时机；发射动作的调用编排（何时发起、四发流程、三次恢复）仍属 `ATOM-13` / `ATOM-14`。
3. 硬件边界：`ATOM-07` 是 PCA9685A/IIC 唯一底层；本模块不直接访问 IIC/PCA9685A 寄存器，仅调用其公开接口 `ServoCore::writeTarget()` 请求单通道目标（用户裁决 #4）。
4. 执行器边界：本模块不修改其他电机或其他舵机目标；三次发射由 `ATOM-14` 编排，本模块不自行重复调用。
5. 用户批准的偏差登记：「直接链接 `Marshland::atom_07_servo_driver`」偏离 `Rule/AtomIntegration.md` §6.2 方案 B（窄端口注入）的默认取向，系用户明确裁决；`ATOM-07` 的 `Inc/` 是该模块的公开头目录，本模块仍不得包含任何其他 Atom 的私有头。
6. 结论：**无冲突、未超出设计范围**，已按本计划实施。若后续需求与 `Design/Atomic/ATOM-11_Fire.md` 的安全约束冲突（例如新增绕过安全校验的调试发射入口、或在急停期间允许写入发射目标），必须先停止并报告，不得实施。

## 3. 已冻结的功能范围

### 3.1 主要职责

对发射机构舵机（`mechanism == "FIRE"`，通道 5）执行一次 `FireAction`：

1. 校验急停、故障事实与发射授权（经 Public `IStateReader` / `IControlAuthority` 查询）及配置就绪状态；
2. 写入发射角目标（`servo.channels[FIRE].action_deg`，当前 180）；
3. 启动并完成发射角保持计时（`timing.fire_angle_hold_ms` = 100 ms）；
4. 保持到期后向同一舵机写回待机角目标（`servo.channels[FIRE].standby_deg`，当前 90）；
5. 仅当待机目标写回成功时判定 `COMPLETED`；
6. 满足完成判据后发布恰好一次完成事件。

每一步独立返回结果；任何普通阶段失败都不得虚报完成（`Design/Atomic/ATOM-11_Fire.md`）。

### 3.2 明确不做

- 不决定是否允许发射（授权判定归 `ATOM-02`，本模块只查询结论），不提交 `ControlIntent`，不直接调用仲裁器；
- 不实现四发流程、发次计数、停止发射标志扫描（属 `ATOM-13`）；
- 不实现急停锁存/解除、故障恢复编排、零力矩帧（属 `ATOM-14`）；
- 不直接访问 IIC/PCA9685A 寄存器（属 `ATOM-07`）、不发送 CAN 报文（属 `ATOM-04`）；
- 不改变其他电机/舵机目标，不为其他机构设置急停例外；
- 不定义 `main()`、不创建可执行文件、不实现 `Design/Atomic` 中其他 Atom 的功能；
- 不实现 CTest / 测试宿主（用户裁决 #7）。

## 4. 接入规范符合性（`Rule/AtomIntegration.md` 1.3）

### 4.1 依赖方向

本模块依赖：`Atoms/Public` 契约头、`Marshland::atom_07_servo_driver` 公开头（用户裁决 #5/#4）、C++ 标准库。不包含 `AppCoordinator.hpp`、`ATOM-02` 私有头或任何其他 Atom 私有头（§1、§6.1）。

### 4.2 结构：内核 + 生命周期适配器（§2）

目录（已实施）：

```text
Atoms/ATOM-11_Fire/
├── CMakeLists.txt                      （统一模板 + 两个链接块，见 §4.7）
├── PLAN.md                             （本文件）
├── Inc/atom11_fire/
│   ├── fire_types.hpp                  FireActionState（Interface §6.5 字面量）与 FireStepOutcome
│   ├── fire_facts.hpp                  FireFacts：急停/故障/授权结论快照（适配层拉取后传入内核）
│   ├── fire_config.hpp                 FireConfig 与 LoadFireConfig(IConfigView&, ...)
│   ├── fire_action.hpp                 FireActionCore（纯逻辑内核）+ IServoAngleWriter / IFireEventSink 窄端口
│   └── lifecycle_adapter.hpp           唯一的 ILifecycleModule 实现声明 + IFireEventOutlet 出口
└── Src/
    ├── fire_config.cpp
    ├── fire_action.cpp
    └── lifecycle_adapter.cpp           独立翻译单元；除「Public 契约查询 → 内核入参」翻译外无业务逻辑
```

- 内核（`fire_types` / `fire_facts` / `fire_config` / `fire_action`）不含任何 Atom 私有头；内核只消费 `FireFacts` 快照与 `IServoAngleWriter` 窄端口，不直接触碰 `IStateReader` / `IControlAuthority` / `ServoCore`，可用替身注入测试。
- `lifecycle_adapter` 职责：配置读取 → 内核就绪 → 结果翻译；另负责每个调用周期经 `IStateReader::ReadState()`（租约内只拷贝字段，释放后再调授权端口）与 `IControlAuthority::AuthorizeManualFire/AuthorizeFire` 组装 `FireFacts` 传入内核（属 §2.3 的「翻译」职责，不含发射时机或授权的业务判断）。
- 业务安全校验发生在内核动作阶段，**不**依赖调用方一定先查 `RunGate()`；急停事实传入时内核自身拒绝写入舵机目标；动作进行中每个 tick 复检急停/故障，出现时不再写任何新目标。

### 4.3 生命周期注册（§3）

按 §3 映射表，`ATOM-11` 对应 `RuntimeStep::InitializeServoOutputs`，与 `ATOM-10` 同步骤；顺序依赖 `ATOM-07`（IIC/PCA9685A 就绪）先完成（§6.3）。

- 注册只能由装配层（`Entry.cpp`）完成；本模块不自行注册、不互相注册（§7.4）。
- 装配层必须检查 `addModule()` 返回值与 `lastRegistrationError()`（§7.2）。
- `required` 必须显式书写（§7.3）；ATOM-11 属硬件/安全相关模块，**应**为 `true`。配置已落地（裁决 #8），取 `true` 不再因配置缺失导致启动失败。
- 本模块新增 `RuntimeStep` 是不允许的；不得在 `InitializeServoOutputs` 之外的语义处注册。

### 4.4 三个契约面（§4）

| 面 | 本模块角色 |
|---|---|
| 生命周期面 | 实现 `marshland::runtime::ILifecycleModule`，仅 `lifecycle_adapter` 一个实现 |
| 输入上报面 | 完成事件与故障事实经自持窄端口 `IFireEventOutlet` 上报，由装配层实现并最终委托 `IInputReporter::ReportFault(...)`。不注入或包含具体 `AppCoordinator` / `StateArbitrator` 引用，不调用字段 setter 与 `UpdateInputs()` |
| 仲裁面 | 本模块**不提交** `ControlIntent`；经 Public `IStateReader::ReadState()` 读 `control_source` 与安全事实，按来源经 `IControlAuthority::AuthorizeManualFire`（SBUS/WEB）/ `AuthorizeFire`（AUTO）查询 `FireAuthorization` 结论（用户裁决 #5）；授权判定本身仍由 `ATOM-02` 做出 |

舵机写入采用用户裁决 #4：`ServoCore` 实例由装配层注入适配层；适配层把内核窄端口 `IServoAngleWriter` 直接转发到 `ServoCore::writeTarget(ServoMechanism::Fire, ...)`。

### 4.5 配置接入（§5）

- 一律通过 `marshland::config::IConfigView` 查询；不自解析 JSON、不自造键名、不臆造默认值（§5.1～§5.5）。
- 必须检查 `ConfigStatus`；返回非 `OK` 时禁止使用该值并如实失败。

本模块读取的键（键名以 `Rule/ConfigSchema.md` 为唯一来源，数值以 `Design/Config.md` 与用户裁决为准）：

| 键 | 类型 | 用途 | 当前状态 |
|---|---|---|---|
| `servo.channels[FIRE].standby_deg` | int（0–180） | 待机角 | 已落地（90，用户裁决 #3/#8） |
| `servo.channels[FIRE].action_deg` | int（0–180） | 发射角 | 已落地（180，用户裁决 #3/#8） |
| `timing.fire_angle_hold_ms` | int | 发射角保持时间（100 ms） | 已定义（用户裁决 #1：唯一权威键） |
| `timing.fire_max_action_ms` | int | 发射机构最大动作时间（300 ms），超出判 `F-SERVO-001` | 已定义 |

- 定位方式：`getArraySize("servo.channels")` + 逐槽位 `getStringAt(..., "mechanism") == "FIRE"` 遍历，**禁止硬编码下标**；槽位 `id` 为 `null`（`getIntAt` 返回 `INVALID_TYPE`），本模块不读取（用户裁决 #2）。
- `fire.servo_id` / `fire.standby_deg` / `fire.fire_deg` 逻辑键**不读取**（裁决 #8：无需 `servo_mechanisms` 段）。
- `fire.hold_ms` **不读取**（裁决 #1：`timing.fire_angle_hold_ms` 唯一权威）。
- `servo.strategy.fire_sequence` 固定为 `STANDBY_TO_FIRE_TO_STANDBY`；实现顺序与该值一致，不以代码隐式改写策略。
- 角度越界处理 `servo.strategy.out_of_range_action = REJECT_AND_WARN`：本模块在配置加载与动作发起处做 0～180° 校验（`W-CMD-001`），ATOM-07 闸门二次落实。

### 4.6 时间与线程（§8）

- 内核接收**显式单调时间参数**（`now_us`），由调用方每个控制周期驱动；内核**不自建线程**、不阻塞主循环、不假设回调准时（§2.3 第 6 条、§8.2、§8.4；用户裁决 #1）。
- 保持计时与最大动作时间均使用单调时钟，禁止墙上时钟（§5.7）。
- 周期遵守 `Config.md` §4：系统最长控制周期 100 ms；发射保持 100 ms 由调用周期推进判定为到期，不在内核内睡眠。

### 4.7 CMake 接入（§1.5、§9.5）

现状：`Atoms/ATOM-11_Fire/CMakeLists.txt` 为统一模板，目标 `atom_11_fire`、别名 `Marshland::atom_11_fire`，已被 `Atoms/CMakeLists.txt` 引入并计入 `marshland_atoms` 聚合。`Src/*.cpp` 存在后目标自动为 `STATIC`。

**已新增**链接块（本轮实施，增量编译验证通过）：

```cmake
if(TARGET Marshland::PublicHeaders)
    target_link_libraries(${ATOM_MODULE_TARGET} ${ATOM_MODULE_SCOPE}
        Marshland::PublicHeaders)
endif()
if(TARGET Marshland::atom_07_servo_driver)
    target_link_libraries(${ATOM_MODULE_TARGET} ${ATOM_MODULE_SCOPE}
        Marshland::atom_07_servo_driver)
endif()
```

- 不定义 `project()`，不设置编译器/架构/C++ 标准/全局编译选项，不建可执行文件（§1.4、§9.6）。
- 本模块**不新增** `Atoms/Public/` 头文件（用户裁决 #6）。

## 5. 代码范围

### 5.1 已新增（本轮实施）

见 §4.2 文件清单（8 个文件）。

### 5.2 已修改

1. `Atoms/ATOM-11_Fire/CMakeLists.txt`：补 `Marshland::PublicHeaders` 与 `Marshland::atom_07_servo_driver` 两个链接块（§4.7）。
2. `Atoms/ATOM-11_Fire/PLAN.md`：本版（v0.5）。

### 5.3 明确不修改

- 上层 `NewVersion/CMakeLists.txt`、`Atoms/CMakeLists.txt`、`Entry.cpp`；
- `Config/dart_control.json`（fire 角度已由用户落地，本模块不修改）；
- `ATOM-01`～`ATOM-10`、`ATOM-12`～`ATOM-19` 的任何文件；
- `Design/`、`Rule/` 下任何文件（只读）；
- `Tests/`（位于上层目录；CTest 不属本模块职责，用户裁决 #7）。

## 6. 领域模型与内部步骤

### 6.1 类型（已实施）

- `enum class FireActionState { IDLE, MOVING_TO_FIRE, HOLDING, RETURNING, COMPLETED, FAILED }`
  —— 逐字对齐 `Design/Interface.md` §6.5（`Inc/atom11_fire/fire_types.hpp`）。
- `enum class FireStepOutcome { OK, REJECTED_ESTOP, REJECTED_FAULT, REJECTED_NOT_AUTHORIZED, REJECTED_NOT_READY, ANGLE_REJECTED, SERVO_WRITE_FAILED, ACTION_TIMEOUT }`
  —— 每个原子步骤的独立结果，供调用方逐次确认。
- `struct FireFacts { bool software_estop; bool fault_active; state_arbitration::FireAuthorization authorization; }`
  —— 安全事实与授权结论快照；由适配层每个调用周期经 Public `IStateReader` / `IControlAuthority` 拉取后传入内核（用户裁决 #5）；复用 Public 的 `FireAuthorization`，不自造控制权语义。
- `struct FireConfig { int standby_deg; int fire_deg; int hold_ms; int max_action_ms; }`
  —— 无 `servo_id`（用户裁决 #2）；角度来自 `servo.channels` FIRE 槽位（裁决 #8）。
- 窄端口：`IServoAngleWriter`（内核 → 适配层 → `ServoCore::writeTarget(ServoMechanism::Fire, ...)`）；`IFireEventSink`（内核 → 适配层）与 `IFireEventOutlet`（适配层 → 装配层）。
- 故障归类：角度越界 → `W-CMD-001`（拒绝该目标、不改变当前有效目标）；写入失败或动作超时 → `F-SERVO-001`（严重，禁止发射；清除后由上层按 `Config.md` §14.2 执行一般故障恢复）。

### 6.2 状态机与调用形态（已实施）

```text
IDLE
  -> 校验：FireFacts 急停 / 故障 / 授权结论 + 配置是否就绪
       失败 -> FAILED（不写舵机，不发完成事件）
       通过 -> writeFireTarget(fire_deg)  [适配层 -> ServoCore::writeTarget(Fire, ...)]
                写入失败 -> FAILED（F-SERVO-001，不发完成事件）
                写入成功 -> MOVING_TO_FIRE -> HOLDING（记录基准时刻 now_us）
HOLDING
  -> now_us - start <  hold_us   -> 保持，不再改写舵机
  -> now_us - start >= hold_us   -> writeFireTarget(standby_deg) -> RETURNING
  -> now_us - start >  max_action_us -> FAILED（F-SERVO-001）
  -> 急停 / 故障出现             -> FAILED（不写任何新目标）
RETURNING
  -> 待机角写入失败 -> FAILED（F-SERVO-001，不发完成事件）
  -> 待机角写入成功 -> COMPLETED + 恰好一次完成事件
```

- 一次 `FireAction` 只能产生一次完成事件（`completion_emitted_` 防护）；`FAILED` 不产生完成事件。
- 本模块不自行重复发射：动作进行中再次 `begin` 返回 `REJECTED_NOT_READY`；`ATOM-14` 的一般故障恢复按顺序调用恰好三次，逐次确认结果。
- 手动、自动与 PEF 故障恢复**复用同一动作行为**，无调试专用发射路径。
- 调用方驱动入口：`LifecycleAdapter::beginOnce(now_us)` / `tick(now_us)`；适配层负责拉取 `FireFacts`。

## 7. 验收判定表

| 场景 | 预期结果 | 实施位置 |
|---|---|---|
| `FireFacts.software_estop == true` | 不写任何舵机目标，无完成事件，`REJECTED_ESTOP` | `fire_action.cpp` `begin`/`tick` |
| `FireFacts.fault_active == true` | 不写发射目标，无完成事件，`REJECTED_FAULT` | 同上 |
| `authorization.allowed == false` | 返回 `REJECTED_NOT_AUTHORIZED`，不写发射目标 | `begin` |
| 前置条件全部通过 | 写发射角（`writeFireTarget(fire_deg)`），进入 `HOLDING` | `begin` |
| 发射角写入失败 | `FAILED`，上报 `F-SERVO-001`，无完成事件 | `begin` |
| 保持时间未到 | 不产生第二次写入，状态保持 `HOLDING` | `tick` |
| 保持时间到期 | 写回待机角 | `tick` |
| 待机角写入失败 | 不置 `COMPLETED`，`F-SERVO-001`，无完成事件 | `tick` |
| 待机角写入成功 | 置 `COMPLETED`，恰好一次完成事件 | `tick` + `emitCompleted` |
| 动作总时长超 `timing.fire_max_action_ms`（300 ms） | `ACTION_TIMEOUT` + `F-SERVO-001`，无完成事件 | `tick` |
| 角度超出 0～180° | 拒绝该目标并记录 `W-CMD-001`，不改变当前有效目标 | `LoadFireConfig` / `configure` / `begin` |
| `COMPLETED` 后再次发起 | 作为新一次动作开始，不重复发送上一次完成事件 | `begin`（重置 `completion_emitted_`） |
| 动作进行中再次发起 | `REJECTED_NOT_READY`，不干扰当前动作 | `begin` |
| 一般故障恢复 | 上层恰好调用三次；本模块逐次返回结果 | 调用方契约（§8） |
| 其他电机与其他舵机 | 本模块不产生任何其他执行器目标 | 仅 `ServoMechanism::Fire` 单通道 |

## 8. 与相邻 Atom 的调用契约

| 关系 | 内容 |
|---|---|
| `ATOM-07` → 本模块 | 直接调用（用户裁决 #4）：装配层注入 `marshland::atom07::ServoCore&`；写目标用 `writeTarget(ServoMechanism::Fire, angle_deg)`（发射舵机 id 为 `null` 的既定寻址方式）；不访问 IIC/寄存器 |
| 本模块 → 装配层绑定的接收端 | 完成事件与故障事实经 `IFireEventOutlet` 发布；最终由装配层委托 `IInputReporter::ReportFault(...)` 与相应事件出口（不新增 Public 头，用户裁决 #6） |
| `ATOM-02` → 本模块 | 经 Public `IStateReader` / `IControlAuthority` 查询控制权、安全事实与授权结论（用户裁决 #5）；不包含 `ATOM-02` 私有头，不注入 `StateArbitrator` 具体引用 |
| `ATOM-13` → 本模块 | `FIRE_READY`、停止标志为 `False` 且授权通过后，经 `LifecycleAdapter::beginOnce` / `tick` 驱动一次发射动作 |
| `ATOM-14` → 本模块 | 一般故障恢复中按顺序调用三次；期间所有机构电机零力矩、其他舵机角度不变 |
| 顺序依赖 | `ATOM-07` 必须先于本模块完成初始化（§6.3） |

## 9. 待确认 / 待接入事项

| # | 事项 | 现状与影响 | 当前处理 |
|---|---|---|---|
| 1 | 完成事件契约 | `Design/Atomic/ATOM-11_Fire.md` 要求发送一次完成事件；用户裁决不新增 Public 头；事件载荷与消费方（`ATOM-13` / `ATOM-17` / `ATOM-18`）未确认 | 已落地自持窄出口 `IFireEventOutlet`；装配层实现该接口并适配到最终消费方 |
| 2 | `ServoCore` 实例注入路径 | `ServoCore` 由 ATOM-07 生命周期适配器持有；本模块构造需要其实例引用 | 待装配层（`Entry.cpp`）接线；本模块已声明构造函数依赖 |
| 3 | 注册 `required` 取值 | ATOM-11 属安全相关模块建议 `true`；配置已落地，取 `true` 不再因配置缺失导致启动失败 | 待装配层注册时显式书写 |

已关闭事项（2026-10-15 用户裁决，见 §1.2）：`fire.servo_id` 取消（#2）、`hold_ms` 重复键（#1，`timing.*` 唯一权威）、舵机写入端口形态（#4，直连 ATOM-07）、安全事实来源（#5，Public 契约自查）、单调时钟来源（#1，显式 `now_us`）、驱动模型（#1，外部 tick）、Public 接口新增（#6，不需要）、测试宿主（#7，CTest 不实现）、fire 角度配置落地（#8，已写入 `dart_control.json` `servo.channels[1]`：standby 90 / action 180）、逻辑键→物理路径映射（#8 后作废，无 `servo_mechanisms` 段）。

## 10. 编码前置自检（提交前，取自 `Rule/AtomIntegration.md` §10）

- [x] `Inc/` 中除 `Atoms/Public/` 与 `atom07_servo_driver/` 公开头（用户裁决 #4）外无任何 `ATOM-NN` 私有头
- [x] 未包含 `AppCoordinator.hpp`；未调用 `UpdateInputs()` 或字段 setter；控制权与授权仅经 Public `IStateReader` / `IControlAuthority` 查询（用户裁决 #5）
- [x] `ILifecycleModule` 由独立翻译单元实现，除「Public 契约查询 → 内核入参」翻译外无业务逻辑
- [x] `initialize()` 失败时如实返回 `success = false`，`error_message` 定位到具体键与 `ConfigStatus`
- [x] `shutdown()` 为 `noexcept`、幂等、对未初始化状态安全
- [x] 配置经 `IConfigView` 按 `Rule/ConfigSchema.md` 键名读取；检查了 `ConfigStatus`，未把非 `OK` 当作成功
- [x] 无自解析 JSON、无臆造默认值；配置缺失时 `LoadFireConfig` 如实失败
- [x] 急停锁定期间本模块自身拒绝发射舵机命令，未把安全寄托于调用方查询 `RunGate()`；动作进行中每 tick 复检急停/故障
- [x] 未把 `accepted == false` / `allowed == false` 当作成功
- [x] 时间使用单调时钟（显式 `now_us`）；周期符合 `Config.md` §4
- [x] 无 `main()`、无独立可执行文件、无 CTest；CMake 提供 `atom_11_fire` 与 `Marshland::atom_11_fire`，链接 `Marshland::PublicHeaders` 与 `Marshland::atom_07_servo_driver`
- [x] aarch64 交叉编译通过且在 `-Wall -Wextra -Wpedantic` 下零警告（**仅增量编译**：`cmake --build cmake-build-ubuntu-arm --target atom_11_fire`，WSL `aarch64-linux-gnu-g++` 13.3，产物 `libatom_11_fire.a` 为 ARM aarch64 ELF）
- [x] 本 `PLAN.md` 已更新接入点、所需配置键、`[待定]` 项与顺序依赖

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | — | 初版计划：目标与边界、初步设计、文件与构建安排、待确认事项 |
| 0.2 | 2026-10-14 | 按最新规范重写：补 `Rule/AtomIntegration.md` 1.3 接入要求（依赖方向、内核+适配器两层、`InitializeServoOutputs` 注册、三个契约面、`IConfigView` 配置键表、单调时间与外部 tick、CMake 修正）；`FireActionState` 统一为 `Interface.md` §6.5 字面量；修正旧稿目标名 `atom11_fire` 为实际的 `atom_11_fire`；补验收判定表、相邻 Atom 调用契约、待确认事项与编码前置自检 |
| 0.3 | 2026-10-15 | 按用户授权复核例外文件 `Design/Atomic/ATOM-11_Fire.md`：单一职责、原子步骤、安全约束、验收条件与本计划一致，无冲突、无超范围；核实本目录 `CMakeLists.txt` 现状与 §4.7 描述一致；新增 §1.1 复核说明 |
| 0.4 | 2026-10-15 | 用户裁决全部外部决策项并授权核查配置与公共契约：`fire.servo_id` 取消（PWM 舵机无 id，按 `mechanism=FIRE`/通道 5 寻址）；角度暂定 `servo_mechanisms.fire` 方案；舵机写入直连 `Marshland::atom_07_servo_driver`；安全事实与授权改经 Public `IStateReader` / `IControlAuthority` 自查；不新增 Public 接口；CTest 不实现 |
| 0.5 | 2026-10-15 | 用户裁决 #8：不新增 `servo_mechanisms` 段，角度经 `servo.channels` 按 `mechanism == "FIRE"` 定位读取（standby 90 / action 180，配置已落地）；`fire.*` 键需求作废；§9 收敛为 3 项。**代码已实施**：`Inc/atom11_fire/` 5 个头文件 + `Src/` 3 个实现文件；`CMakeLists.txt` 补 `Marshland::PublicHeaders` 与 `Marshland::atom_07_servo_driver` 链接块；WSL aarch64 增量编译通过、零警告 |
