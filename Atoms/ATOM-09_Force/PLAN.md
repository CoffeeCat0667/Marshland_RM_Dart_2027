# ATOM-09_Force Coding Plan

> 状态：**计划已冻结，代码尚未实施**（`Inc/`、`Src/` 不存在；本文件不含已实施声明）。
> 版本：0.4
> 更新日期：2026-10-14
> 同步基线：PRD 0.10、PEF 1.5、Interface 1.8、Config 1.3；`Rule/AtomIntegration.md` 1.3、`Rule/ConfigSchema.md` 1.7
> 裁决状态：`Q-01`～`Q-26` **已全部裁决**（见 §8），可进入 Coding。
> 验证方式：本轮仅文档与静态依赖核对；**未编译**。

## 0. 本轮依据

| 来源 | 用途 |
|---|---|
| 本目录 `AGENTS.md` | 本工作目录唯一工作规则来源 |
| 根 `AGENTS.md` | **按用户裁决不适用于本模块 Agent**（见 Q-01） |
| `Rule/AtomIntegration.md` 1.3 | 接入规范（依赖方向、两层结构、三契约面、配置、CMake、自检清单） |
| `Rule/ConfigSchema.md` 1.7 + `ConfigSchema.schema.json` | 配置键名与类型的唯一来源 |
| `Design/Config.md` 1.3、`Design/Interface.md` 1.8、`Design/Atoms.md` 0.12 | 数值/契约/Atom 索引 |
| `Design/Atomic/ATOM-09_Force.md`、`ATOM-05/10/13/14`、`PEF.md` | 本 Atom 职责与上下游边界 |
| `Atoms/Public/*.hpp`、`Atoms/ATOM-03_SBUS` | 已落地公共契约与实现范式 |

## 1. 冻结的功能范围

### 1.1 单一职责

接受一个变力目标，交给 Config 映射的变力电机（**电机 6，node `0x206`，`0x1FF` 帧槽位 1**）闭环执行，并提供目标、当前反馈与动作状态。**本 Atom 不安排发射流程顺序、不计发次。**

### 1.2 内部原子操作

1. 校验变力目标的类型、模式、授权与 Config 范围。
2. 提交目标给 `ForceController`。
3. 交给 ATOM-05 对应电机执行闭环/PID（经窄接口）。
4. 接收一条电机反馈并更新状态。
5. 执行配置时间 X 秒的计时（**仅自动模式**）。
6. X 秒到期置为完成。
7. 查询目标、反馈、动作状态与错误。
8. 通用电机/PID/CAN 故障按 Q-17 结论处理。

### 1.3 明确不做

- 不实现 `main()`、不建可执行文件、不设编译选项。
- 不做 CAN/IIC 寄存器访问、PID 运算、反馈解码（归 ATOM-04/05）。
- 不做发射授权、控制权仲裁、系统模式迁移（归 ATOM-02）。
- 不做安全编排、急停解除、一般故障恢复（归 ATOM-14）。
- 无本机构专属异常恢复策略（`force.has_dedicated_fault_handling = false`）。
- 不新增 WEB 动作；`force_set` 已由 `Config.md` §11.4 定义。

## 2. 架构形态

### 2.1 纯逻辑内核，不注册生命周期（Q-07）

按 `AtomIntegration.md` §3.1(a)：不持有设备、不建线程、不需要"初始化/释放"语义的纯逻辑模块**不应注册为生命周期模块**。

- **不实现 `ILifecycleModule`，不提供 `lifecycle_adapter`，不注册任何 `RuntimeStep`。**
- 由装配层构造并注入；推进由外部周期调用 `update()`（Q-04）。

### 2.2 依赖方向

```text
Entry.cpp / AppCoordinator（装配层）
  -> ForceController（本模块纯逻辑内核）
       -> Atoms/Public：IConfigView、IMonotonicClock、IStateReader、
                        ISbusSnapshotReader、ControlIntent/IControlAuthority、
                        FaultSeverity、ForceTypes/ForceInterfaces（新增）
       -> 本模块私有窄接口：IMotorCommandSink、IForceFaultSink
       -> C++ 标准库
```

**禁止**包含任何 ATOM 私有头、`AppCoordinator.hpp`、自解析 JSON、自建线程。

## 3. 接口与数据来源（Q-06 / Q-09 / Q-10 结论）

### 3.1 数据来源选择（先查控制权，再按 SBUS > WEB > AUTO 取数）

1. **查询当前控制权归属**：注入 `app::IStateReader`，取 `ReadState().context().control_source`（类型 `marshland::ControlSource`，优先级由 `MarshlandTypes.hpp` 的 `ControlSourceRank` 给出，**不再自造优先级**）。
2. **SBUS 持有** → 注入 `sbus::ISbusSnapshotReader`，每周期读 `readSbusSnapshot()`，取
   `scaled_channels[11]`（索引 11 = CH12，已确认即**变力专用 rpm**，单位 rpm，窗口 `-2000`～`+2000`）。
   该值作为**手动目标**；不应用通用缩放 `x`。快照 `online == false` 时不产生新命令。
3. **WEB 持有** → 目标由 `force_set` 一次性提交，经装配适配进入 `IForceController::submitTarget()`；
   **`Atoms/Public` 现有头文件中不存在 WEB 手动目标的数据载体**（本轮已检索全部 Public 头），故由 §3.3 新增契约承载。
4. **AUTO 持有** → 由 ATOM-13 调用 `submitTarget(target, ForceMode::TIMED)` 提交，本模块不自取。

### 3.2 模式语义（Q-10）

| 模式 | 触发方 | 完成判据 | 重复提交 |
|---|---|---|---|
| `ForceMode::TIMED` | 自动流程（ATOM-13） | **恰为** `timing.force_time_s` 到期 | `REJECTED_BUSY`（Q-12） |
| `ForceMode::MANUAL` | SBUS CH12 / WEB `force_set` | **无时限**，持续有效至新目标、控制源切换或安全事件 | 覆盖（last-write-wins，Q-12） |

### 3.3 计划新增的公共契约（Q-03 已授权，实施前置清单）

> 目的：ATOM-13 / ATOM-16 / ATOM-17-18 需要调用或查询 ATOM-09，而 `AtomIntegration.md` §6.1 禁止横向包含私有头。

**① `Atoms/Public/ForceTypes.hpp`**（纯类型，仅依赖 `MarshlandTypes.hpp`）

- `enum class ForceMode { TIMED, MANUAL }`
- `enum class ForceActionState { IDLE, RUNNING, COMPLETED, REJECTED, FAULT }`
- `enum class ForceSubmitResult { ACCEPTED, REJECTED_INVALID, REJECTED_RANGE, REJECTED_CONFIG, REJECTED_BUSY, REJECTED_SAFETY, MOTOR_COMMIT_FAILED }`
- `struct ForceTarget { std::int32_t target_rpm; marshland::ControlSource source; std::uint32_t request_id; }`
- `struct ForceMotorFeedback { bool valid; bool has_received; std::int32_t rpm; std::uint64_t timestamp_us; }`
- `struct ForceStatus { ForceActionState state; ForceMode mode; std::int32_t target_rpm; ControlSource source; ForceMotorFeedback feedback; std::uint64_t deadline_us; std::uint32_t generation; const char* error_code; }`

**② `Atoms/Public/ForceInterfaces.hpp`**（纯抽象，供调用方注入）

- `class IForceController`：
  - `ForceSubmitResult submitTarget(const ForceTarget&, ForceMode) noexcept`
  - `void update(std::int64_t now_us) noexcept`
  - `ForceStatus status() const noexcept`
  - `void stop() noexcept`

**不新增**：ATOM-09 自身的依赖端口（`IMotorCommandSink`、`IForceFaultSink`）放在本模块 `Inc/atom09_force/force_interfaces.hpp`，由装配层适配 ATOM-04/05/14；这样 ATOM-05/14 无需包含 ATOM-09 的头。

## 4. 配置接入（Q-08）

一律经注入的 `config::IConfigView`，按 `Rule/ConfigSchema.md` 键名查询，逐项检查 `ConfigStatus`；**禁止**臆造默认值、**禁止**自解析 JSON、**禁止**消费 `[待定]`/未列出键。

### 4.1 ATOM-09 将读取的键（现状核对）

| 键 | 状态 | 用途 |
|---|---|---|
| `motor[].role`、`motor[].id` | **已存在** | 解析 `role == "force"` 得到电机 ID（唯一匹配，不硬编码 6） |
| `force.control_mode` | 已存在 | 必须为 `CLOSED_LOOP_PID` |
| `force.target_unit` | 已存在 | 必须为 `rpm` |
| `force.target_range` | 已存在 | WEB `force_set` 目标允许范围（对称 ±15000） |
| `force.completion_criterion` | 已存在 | 必须为 `TIMED`（TIMED 模式） |
| `force.target_default_source` | 已存在 | 必须为 `SBUS_CH12` |
| `force.has_dedicated_fault_handling` | 已存在 | 必须为 `false`；为 `true` 时停止并报告 |
| `timing.force_time_s` | 已存在 | TIMED 模式的完成判据 X 秒 |
| `timing.max_control_cycle_ms` | 已存在 | 校验外部 `update()` 间隔上限（100） |
| `sbus.joystick_center`、`sbus.valid_min`、`sbus.valid_max` | 已存在 | 校验 CH12 原始量合法性（1024 / 364 / 1684） |
| `timing.force_max_action_ms` | 已存在但**不再消费** | 按 Q-14 方案 B 删除超时分支（见 §5.4） |

### 4.2 申请新增的键（请您加入 `Config/dart_control.json` + `ConfigSchema`）

| 新键 | 类型 | 建议默认 | 用途 |
|---|---|---|---|
| `force.center_deadband_rpm` | int（≥0） | `0` | 判定"CH12 中位"的转速死区，抑制中位抖动；0 表示严格 `scaled == 0` |
| `force.center_action` | enum str | `ZERO_TORQUE` | 中位时的处置，按 Q-13 固定为 `ZERO_TORQUE`（可选 `ZERO_SPEED` 保留扩展） |

> 除上述 2 项外，**无其他必须新增的配置键**。两键未加入前，模块按规则 **fail-closed**：如实报告配置不可用，不臆造默认值。
> 建议同步处理：`timing.force_max_action_ms` 在 Q-14=B 后对 ATOM-09 失去意义，可保留给 ATOM-14 监控或标注弃用。

## 5. 行为设计

### 5.1 状态与结果

- `ForceActionState`：`IDLE` / `RUNNING` / `COMPLETED` / `REJECTED` / `FAULT`。
- 每次成功提交使 `generation` 递增，用于丢弃迟到完成事件。

### 5.2 提交 `submitTarget`

1. 校验目标有限性、`force.control_mode`/`target_unit`、范围（MANUAL 用 `force.center_deadband_rpm` 与 CH12 窗口；WEB 用 `force.target_range`）。
2. 校验配置可用；非 `OK` → `REJECTED_CONFIG`，**不改变**当前有效目标。
3. 安全复核（fail-closed）：`StateReadView::estop_latched()`、`run_gate()`、活动故障 → `REJECTED_SAFETY`；**不代替 ATOM-02 决策**（Q-05：调用方先 `SubmitIntent(CONTROL)`）。
4. 提交电机：`IMotorCommandSink::submitVelocityTarget(id, rpm)`；失败 → `MOTOR_COMMIT_FAILED`，**不**进入 `RUNNING`。
5. 成功：TIMED 记录 `deadline_us = now + force_time_s * 1e6`；MANUAL 无 `deadline_us`。

### 5.3 推进 `update(now_us)`

1. 仅用单调时钟（`IMonotonicClock::nowMicros()`）。
2. 时间倒退/非法 → 保持状态并记录错误，不误判完成。
3. TIMED 且 `now >= deadline_us` → `COMPLETED`（幂等）。
4. **MANUAL 不因时间到期改变状态**；仅由新目标/控制源切换/安全事件改变。

### 5.4 超时分支（Q-14 = 方案 B）

`timing.force_max_action_ms` 与 `timing.force_time_s` 数值相同且完成优先，**超时分支不可达**，按裁决**删除**该分支并在此登记：ATOM-09 不再因超时置 `FAULT`；电机/PID/编码器/CAN 故障仍由 Q-17 路径判定。

### 5.5 CH12 中位与手动路径（Q-09 / Q-13）

1. 仅当**控制权归属为 SBUS** 时消费 CH12。
2. `|scaled_channels[11]| <= force.center_deadband_rpm` → 判为中位，按 `force.center_action` 执行 `ZERO_TORQUE`：调用 `IMotorCommandSink::stopMotor(force_motor_id)`（**不是** 0 rpm 闭环），状态回 `IDLE`。
3. 非中位 → 以该 rpm 提交速度目标，进入 MANUAL/`RUNNING`。
4. 控制权不再为 SBUS（切 WEB/AUTO/失联）→ 立即停止消费 CH12，按新归属取数；不恢复已终止的 TIMED 动作。

### 5.6 反馈与故障（Q-17）

1. 只接受映射电机反馈，其他电机反馈不得覆盖。
2. 无效反馈不刷新有效时间、不伪造转速。
3. `IMotorCommandSink::faultLatched(id)` 为真 → 状态 `FAULT`，经 `IForceFaultSink` 上报**"变力动作失败"事实**；电机/PID/编码器/CAN 的原始故障码由 ATOM-05/04 各自上报 ATOM-14，**本模块不重复登记**（避免同一故障双报）。
4. 本模块**不清除**故障、不自动重试。

### 5.7 查询（Q-11）

返回值（`ForceSubmitResult`）+ 轮询只读 `status()`；**不引入回调线程**。

## 6. 文件与工程

### 6.1 计划新增文件

```text
Atoms/Public/
├── ForceTypes.hpp                  # 新增公共类型（§3.3①）
└── ForceInterfaces.hpp             # 新增 IForceController（§3.3②）

Atoms/ATOM-09_Force/
├── Inc/atom09_force/
│   ├── force_types.hpp             # 模块内部补充类型（若公共类型已足够则只做别名）
│   ├── force_config.hpp            # IConfigView -> ForceConfig（含 role 解析）
│   ├── force_interfaces.hpp        # IMotorCommandSink / IForceFaultSink（私有窄接口）
│   └── force_controller.hpp        # 内核（无线程、无 I/O、无设备）
└── Src/
    ├── force_config.cpp
    └── force_controller.cpp
```

命名空间 `marshland::atom09`；包含进入方式 `atom09_force/xxx.hpp`；**不创建** `lifecycle_adapter`。

### 6.2 工程改动（Q-02 已授权）

1. `Atoms/ATOM-09_Force/CMakeLists.txt`：补
   `if(TARGET Marshland::PublicHeaders) target_link_libraries(... Marshland::PublicHeaders) endif()`；
   不新增线程依赖、不定义 `project()`、不设编译选项。
2. `Atoms/Public/CMakeLists.txt`（或 `Headers/CMakeLists.txt`）：登记 2 个新增公共头。
3. 不改上层 `CMakeLists.txt`、`Entry.cpp`（装配接线由上层负责人完成；Q-21 授权我**只读**核对）。

## 7. 验收判定表

| 场景 | 预期结果 |
|---|---|
| 合法规目标（`role="force"` 电机、范围内） | 提交到电机 6/`0x206`，状态 `RUNNING`，不影响其他电机 |
| 目标超范围/非有限 | 拒绝，**不改变**当前有效目标 |
| 模式/单位不符 | 拒绝，不提交电机 |
| 急停锁存或活动故障 | fail-closed 拒绝 |
| 电机提交失败 | `MOTOR_COMMIT_FAILED`，不进入 `RUNNING` |
| TIMED：`X-1` 秒 / `X` 秒 | 仍 `RUNNING` / 恰转 `COMPLETED` |
| TIMED 中重复提交 | `REJECTED_BUSY`，原计时不变 |
| MANUAL 任意时长 | 不转 `COMPLETED`；新目标覆盖并更新 `generation` |
| 控制权 = SBUS，CH12 = 1684 | 提交 +2000 rpm 速度目标 |
| 控制权 = SBUS，CH12 = 1024（或处于死区内） | **电机零力矩**（`stopMotor`），状态 `IDLE` |
| 控制权 ≠ SBUS | 不消费 CH12，按 WEB/AUTO 路径取数 |
| 非映射电机反馈 | 不覆盖变力反馈 |
| 时间倒退 | 不误判完成，记录错误 |
| `faultLatched` 为真 | `FAULT` + 上报动作失败事实，不重复登记原始故障码，不自动重试 |
| 第 1～4 发各调用一次 | 每发恰一次；本模块不自排发次 |
| 配置键缺失/类型错误/越界（含 2 个新键） | 配置不可用 → 如实失败，不臆造默认值 |

## 8. 裁决登记（`Q-01`～`Q-26`，2026-10-14 全部裁决）

| 编号 | 事项 | 裁决结论 |
|---|---|---|
| Q-01 | 实施授权与规则冲突 | **授权实施**；按本目录 `AGENTS.md` + `Rule/AtomIntegration.md` 执行，根 `AGENTS.md` 不适用于本模块 Agent |
| Q-02 | CMake 补 `Marshland::PublicHeaders` | **许可** |
| Q-03 | 新增 `Atoms/Public` 契约头 | **许可**，按 §3.3 清单先提交后实施 |
| Q-04 | `update()` 周期调用方 | 装配层周期线程按 `timing.max_control_cycle_ms` 调用 |
| Q-05 | 授权链形态 | 调用方先 `SubmitIntent(CONTROL)`；本模块仅做 fail-closed 复核 |
| Q-06 | 调用契约 | 新增 `ForceTypes.hpp` + `ForceInterfaces.hpp`；先查控制权再按 `SBUS > WEB > AUTO` 取数（§3.1） |
| Q-07 | 生命周期注册 | **不注册**，不实现 `ILifecycleModule` |
| Q-08 | 电机 ID 与配置字段 | **不硬编码**；按 `motor[].role=="force"` 解析；新增键见 §4.2（请用户加入） |
| Q-09 | CH12 读取方式 | 确认 `scaled_channels[11]` = CH12 专用 rpm，**直接读** |
| Q-10 | 模式语义 | **双模式**：自动 TIMED 限 X 秒；手动（SBUS/WEB）不限 |
| Q-11 | 完成/故障通知 | 返回值 + 轮询只读 getter |
| Q-12 | 运行中重复提交 | TIMED → `REJECTED_BUSY`；MANUAL → 覆盖 |
| Q-13 | CH12 中位（1024 → 0 rpm） | **保持电机零力矩**（`ZERO_TORQUE`，非 0 rpm 闭环） |
| Q-14 | `force_max_action_ms` 超时 | **方案 B**：删除超时分支并登记（§5.4） |
| Q-15 | `target_range` 与 CH12 窗口 | 各自按来源上限校验：WEB `force_set` ±15000；CH12 ±2000 |
| Q-16 | 手动模式门槛 | 由 ATOM-02/ATOM-16 把关，ATOM-09 仅 fail-closed 复核 |
| Q-17 | 反馈/故障归属与去重 | 判定归 ATOM-05；本模块只上报"动作失败"事实，不重复登记原始故障码 |
| Q-18 | `mechanism_state` 聚合 | 装配层聚合，本模块只提供只读 getter |
| Q-19 | 命名冻结 | 按 §6.1（`marshland::atom09`、`Inc/atom09_force/`、`atom_09_force`） |
| Q-20 | 验证方式 | 我只做静态核对；编译由用户在目标 ARM 主机/WSL 增量执行 |
| Q-21 | 只读上层接线文件 | **许可**一次性只读 `Atoms/CMakeLists.txt`、`Entry.cpp` |
| Q-22 | 模块级测试入口 | **不需要**（规则禁止 `main()`） |
| Q-23 | `Interface.md` §10.1 映射陈旧 | **已修**：1.8 版更正为 变力 6/`0x206`、YAW 5/`0x205`，补 CAN 节点列 |
| Q-24 | `ConfigSchema.md` 换弹「四个舵机」 | **已更正**：1.7 版 §2.7.1 改为双 M3508；`servo_mechanisms.reload` 键结构迁移待确认，未实施 |
| Q-25 | 章节号 §5.5 → §5.6/§5.7 | **已修**：§2.5/§2.6/§2.7 及待裁决第 1 项已对齐 §5.6/§5.7 |
| Q-26 | `Config.md` 历史版本行 | **保留历史原文**（1.3 行已声明取代 0.9 行） |

## 9. 实施顺序

1. 落地 `Atoms/Public/ForceTypes.hpp`、`ForceInterfaces.hpp`，登记进 Public CMake。
2. 冻结核内私有接口与类型（`force_interfaces.hpp`、`force_types.hpp`）。
3. 实现 `force_config.cpp`（含 `role=="force"` 解析、2 个新键、`ConfigStatus` 检查）。
4. 实现 `force_controller.cpp`（提交/双模式/中位零力矩/反馈/状态）。
5. 补本模块 CMake 的 `Marshland::PublicHeaders` 链接。
6. 静态自检（`AtomIntegration.md` §10 清单）+ 判定表复核。
7. 增量编译：WSL aarch64 交叉编译或目标 ARM 主机编译，`-Wall -Wextra -Wpedantic` 零警告（由用户执行）。

## 10. 明确不修改范围

- 上层 `NewVersion/CMakeLists.txt`、`Entry.cpp`；
- ATOM-01/02/04/05/10/13/14/15/16/17/18/19 的私有实现；
- `Design/`、`Rule/` 中除本轮 Q-23/24/25 已授权更正外的其他内容；
- 不删除现有文件，不做无关重构。

## 11. 修订记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | （原稿，无记录） | 首版方案；电机映射误写 ID 4；接口为占位描述 |
| 0.2 | 2026-10-14 | 重写：修正变力电机为 6/`0x206` 并按 `role` 解析；确立纯逻辑内核不注册生命周期；改为 `IConfigView` + 窄接口注入；给出配置键、验收判定表、CMake 缺口与 11 项登记 |
| 0.3 | 2026-10-14 | 统一 CH12 为变力专用映射；`force.target_default_source = SBUS_CH12`；同步 `Config.md` 1.3、`Interface.md` 1.7、`ConfigSchema.md` 1.6 与 schema |
| 0.4 | 2026-10-14 | **裁决落地**：`Q-01`～`Q-26` 全部裁决并登记；确定控制权→数据来源选择、`ForceTypes/ForceInterfaces` 公共契约清单、`force.center_deadband_rpm`/`force.center_action` 新增键申请、双模式语义、CH12 中位零力矩、删除超时分支；更正 `Interface.md` 1.8（§10.1 电机映射）与 `ConfigSchema.md` 1.7（§2.7.1 双 M3508、章节号）。**仍仅修改文档，未实施代码、未编译。** |
