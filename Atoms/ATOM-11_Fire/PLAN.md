# ATOM-11_Fire 实现计划（Coding Plan）

> 状态：规划已按最新规范重写；**尚未实施任何代码**，`Inc/`、`Src/` 均未创建。
> 文档版本：0.2
> 更新日期：2026-10-14
> 同步基线：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8；`Rule/AtomIntegration.md` 1.3；`Rule/ConfigSchema.md` 1.4
> 验证方式：静态检查与判定表；本模块不建立独立可执行测试入口（`Rule/AtomIntegration.md` §9.5）
> 授权：本轮仅授权更新本 `PLAN.md`。任何代码实施需另行明确授权（根 `AGENTS.md` 第 1 节）。

## 1. 本次更新说明

本轮按用户授权只读读取以下内容，未修改其中任何文件：

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

本次实际修改的文件只有本 `PLAN.md`。

## 2. 与架构设计的一致性检查（`NewVersion/AGENTS.md` 第 3 节）

1. 需求来源：`Design/Atomic/ATOM-11_Fire.md` 的单一职责是对发射机构配置的 `x` 号舵机提供一次发射动作及结果状态，与本计划 §3 冻结范围一致。
2. 调用边界：授权判定不属于本模块，明确归属 `ATOM-02` / `ATOM-13` / `ATOM-14`；本模块不决定能否发射、不调度调用时机。
3. 硬件边界：`ATOM-07` 是 PCA9685A/IIC 唯一底层；本模块不直接访问 IIC/PCA9685A 寄存器，只通过注入的窄端口请求单通道目标。
4. 执行器边界：本模块不修改其他电机或其他舵机目标；三次发射由 `ATOM-14` 编排，本模块不自行重复调用。
5. 结论：**无冲突、未超出设计范围**，可以按本计划实施。若后续需求与 `Design/Atomic/ATOM-11_Fire.md` 的安全约束冲突（例如新增绕过安全校验的调试发射入口、或在急停期间允许写入发射目标），必须先停止并报告，不得实施。

## 3. 已冻结的功能范围

### 3.1 主要职责

对 `x` 号发射舵机执行一次 `FireAction`：

1. 校验急停、控制权/发射授权、流程前置条件与发射舵机权限；
2. 写入发射角目标；
3. 启动并完成发射角保持计时（`Config.md` §4：100 ms）；
4. 保持到期后向同一舵机写回待机角目标；
5. 仅当待机目标写回成功时判定 `COMPLETED`；
6. 满足完成判据后发布一次完成事件。

每一步独立返回结果；任何普通阶段失败都不得虚报完成（`Design/Atomic/ATOM-11_Fire.md`）。

### 3.2 明确不做

- 不决定是否允许发射，不提交 `ControlIntent`，不直接调用仲裁器；
- 不实现四发流程、发次计数、停止发射标志扫描（属 `ATOM-13`）；
- 不实现急停锁存/解除、故障恢复编排、零力矩帧（属 `ATOM-14`）；
- 不直接访问 IIC/PCA9685A、不发送 CAN 报文（属 `ATOM-04` / `ATOM-07`）；
- 不改变其他电机/舵机目标，不为其他机构设置急停例外；
- 不定义 `main()`、不创建可执行文件、不实现 `Design/Atomic` 中其他 Atom 的功能。

## 4. 接入规范符合性（`Rule/AtomIntegration.md` 1.3）

### 4.1 依赖方向

保持 `ATOM-11 → Atoms/Public` 单向。禁止包含 `AppCoordinator.hpp`、`ATOM-02` 私有头或其他 Atom 私有头（§1、§6.1）。内核只依赖 Public 契约头与 C++ 标准库。

### 4.2 结构：内核 + 生命周期适配器（§2）

计划目录（当前为空，待实施）：

```text
Atoms/ATOM-11_Fire/
├── CMakeLists.txt                      （已存在，见 §4.7）
├── PLAN.md                             （本文件）
├── Inc/atom11_fire/
│   ├── fire_types.hpp                  领域类型与状态枚举
│   ├── fire_ports.hpp                  窄端口：舵机写入、事件与故障上报
│   ├── fire_config.hpp                 FireConfig 与 LoadFireConfig(IConfigView&, ...)
│   ├── fire_action.hpp                 FireActionCore（纯逻辑内核）
│   └── lifecycle_adapter.hpp           唯一的 ILifecycleModule 实现声明
└── Src/
    ├── fire_config.cpp
    ├── fire_action.cpp
    └── lifecycle_adapter.cpp           独立翻译单元，目标 ≤ 50 行、无业务逻辑
```

- 内核（`fire_types` / `fire_ports` / `fire_config` / `fire_action`）不含任何 Atom 私有头。
- `lifecycle_adapter` 只做「配置读取 → 内核就绪 → 结果翻译」，不含机构流程、控制权或发射授权判断（§2.3 第 5 条）。
- 业务安全校验发生在内核动作阶段，**不**依赖调用方一定先查 `RunGate()`（§4.2 重要边界）；急停事实传入时内核自身拒绝写入舵机目标。

### 4.3 生命周期注册（§3）

按 §3 映射表，`ATOM-11` 对应 `RuntimeStep::InitializeServoOutputs`，与 `ATOM-10` 同步骤；顺序依赖 `ATOM-07`（IIC/PCA9685A 就绪）先完成（§6.3）。

- 注册只能由装配层（`Entry.cpp`）完成；本模块不自行注册、不互相注册（§7.4）。
- 装配层必须检查 `addModule()` 返回值与 `lastRegistrationError()`（§7.2）。
- `required` 必须显式书写（§7.3）；ATOM-11 属硬件/安全相关模块，**应**为 `true`。其后果与 `[待定]` 配置的交互见 §9 第 1 项。
- 本模块新增 `RuntimeStep` 是不允许的；不得在 `InitializeServoOutputs` 之外的语义处注册。

### 4.4 三个契约面（§4）

| 面 | 本模块角色 |
|---|---|
| 生命周期面 | 实现 `marshland::runtime::ILifecycleModule`，仅 `lifecycle_adapter` 一个实现 |
| 输入上报面 | 只通过注入的窄端口上报完成事件与故障事实；最终由装配层委托 `IInputReporter::ReportFault(...)`。禁止注入或包含具体 `AppCoordinator` / `StateArbitrator` 引用，禁止调用字段 setter 与 `UpdateInputs()` |
| 仲裁面 | 本模块**不提交** `ControlIntent`；`FIRE` 意图仲裁与 `AuthorizeFire` / `EvaluateFireAuthorization` 复核由调用方（`ATOM-13` / `ATOM-14` / `ATOM-16`）按 §4.3.2 顺序完成，本模块消费其结果并按注入的急停/故障事实自行拒绝 |

窄端口采用 §6.2 方案 B（接口注入）：本模块只声明自己的最小能力，由装配层适配真实实现，不横向包含任何 Atom 的 `Inc/`。

### 4.5 配置接入（§5）

- 一律通过 `marshland::config::IConfigView` 查询；禁止自解析 JSON、禁止自造键名、禁止臆造默认值（§5.1～§5.5）。
- 必须检查 `ConfigStatus`；返回非 `OK` 时禁止继续使用该值。

本模块读取的键（键名以 `Rule/ConfigSchema.md` 1.4 为唯一来源，数值以 `Design/Config.md` 为准）：

| 键 | 类型 | 用途 | 当前状态 |
|---|---|---|---|
| `fire.servo_id` | int | `x` 号发射舵机编号 | `[待定]`（`null`）→ 禁止推断 |
| `fire.standby_deg` | int | 待机角 | `[待定]`（`null`）→ 禁止推断 |
| `fire.fire_deg` | int | 发射角 | `[待定]`（`null`）→ 禁止推断 |
| `timing.fire_angle_hold_ms` | int | 发射角保持时间（100 ms） | 已定义 |
| `timing.fire_max_action_ms` | int | 发射机构最大动作时间（300 ms），超出判 `F-SERVO-001` | 已定义 |
| `servo.strategy.out_of_range_action` | str | 越界处理（`REJECT_AND_WARN` → `W-CMD-001`） | 已定义 |

- `servo.strategy.fire_sequence` 固定为 `STANDBY_TO_FIRE_TO_STANDBY`；本模块实现顺序必须与该值一致，不以代码隐式改写策略。
- `fire.hold_ms` 与 `timing.fire_angle_hold_ms` 是同一语义的两个键，存在重复；本计划暂以 `timing.*` 为权威并登记为待裁决事项（§9 第 2 项），**不得**同时读两个键后静默择一。

### 4.6 时间与线程（§8）

- 内核接收**显式单调时间参数**（`now_us`），由调用方每个控制周期驱动；内核**不自建线程**、不阻塞主循环、不假设回调准时（§2.3 第 6 条、§8.2、§8.4）。
- 保持计时与最大动作时间均使用单调时钟，禁止墙上时钟（§5.7）。
- 周期遵守 `Config.md` §4：系统最长控制周期 100 ms；发射保持 100 ms 由调用周期推进判定为到期，不在内核内睡眠。

### 4.7 CMake 接入（§1.5、§9.5）

现状：`Atoms/ATOM-11_Fire/CMakeLists.txt` 已是统一模板，目标 `atom_11_fire`、别名 `Marshland::atom_11_fire`，已被 `Atoms/CMakeLists.txt` 第 27 行引入并计入 `marshland_atoms` 聚合。

**计划新增**（与 `ATOM-03_SBUS` 模板对齐，当前缺失）：

```cmake
if(TARGET Marshland::PublicHeaders)
    target_link_libraries(${ATOM_MODULE_TARGET} ${ATOM_MODULE_SCOPE}
        Marshland::PublicHeaders)
endif()
```

- 不定义 `project()`，不设置编译器/架构/C++ 标准/全局编译选项，不建可执行文件（§1.4、§9.6）。
- `Src/*.cpp` 出现后模板自动由 `INTERFACE` 变为 `STATIC`，无需手改。
- 本模块**不需要**新增 `Atoms/Public/` 头文件：调用方各自声明窄接口、装配层做适配，避免把实现细节上提为公共契约。

## 5. 代码范围

### 5.1 计划新增

见 §4.2 的 `Inc/atom11_fire/` 与 `Src/` 文件清单（8 个文件）。

### 5.2 计划修改

1. `Atoms/ATOM-11_Fire/CMakeLists.txt`：补 `Marshland::PublicHeaders` 链接块（§4.7）。
2. `Atoms/ATOM-11_Fire/PLAN.md`：随实施进度更新接口、判定结果与剩余风险，不把未验证内容写成已完成。
3. `Atoms/Public/`：**预计不需要**。若后续确认必须新增公共类型（例如完成事件的跨模块契约），须单独申请授权并同步 `Atoms/Public/Headers/CMakeLists.txt` 的头文件列表。

### 5.3 明确不修改

- 上层 `NewVersion/CMakeLists.txt`、`Atoms/CMakeLists.txt`、`Entry.cpp`；
- `ATOM-01`～`ATOM-10`、`ATOM-12`～`ATOM-19` 的任何文件；
- `Design/`、`Rule/` 下任何文件（只读）；
- `Tests/`（位于上层目录，不在本工作目录的允许写入范围内）。

## 6. 领域模型与内部步骤

### 6.1 类型（拟）

- `enum class FireActionState { IDLE, MOVING_TO_FIRE, HOLDING, RETURNING, COMPLETED, FAILED }`
  —— 逐字对齐 `Design/Interface.md` §6.5，禁止增删成员或改名。
- `enum class FireStepOutcome { OK, REJECTED_ESTOP, REJECTED_FAULT, REJECTED_NOT_AUTHORIZED, REJECTED_NOT_READY, ANGLE_REJECTED, SERVO_WRITE_FAILED, ACTION_TIMEOUT }`
  —— 每个原子步骤的独立结果，供调用方逐次确认。
- `struct FireActionRequest { state_arbitration::FireAuthorization authorization; bool software_estop; bool fault_active; }`
  —— 复用 Public 的 `FireAuthorization`，避免自造控制权语义。
- `struct FireConfig { int servo_id; int standby_deg; int fire_deg; int hold_ms; int max_action_ms; }`。
- 故障归类：角度越界 → `W-CMD-001`（拒绝该目标、不改变当前有效目标）；写入失败或动作超时 → `F-SERVO-001`（严重，禁止发射；清除后由上层按 `Config.md` §14.2 执行一般故障恢复）。

### 6.2 状态机与调用形态

```text
IDLE
  -> 校验：急停 / 故障 / 授权 / 配置是否就绪
       失败 -> FAILED（不写舵机，不发完成事件）
       通过 -> 写入发射角 `fire.fire_deg`
                写入失败 -> FAILED（F-SERVO-001，不发完成事件）
                写入成功 -> MOVING_TO_FIRE -> HOLDING（记录基准时刻）
HOLDING
  -> now_us <  基准 + hold_ms   -> 保持，不再改写舵机
  -> now_us >= 基准 + hold_ms   -> 写回 `fire.standby_deg` -> RETURNING
  -> 动作总时长 > max_action_ms -> FAILED（F-SERVO-001）
RETURNING
  -> 待机角写入失败 -> FAILED（保持未完成，不发完成事件）
  -> 待机角写入成功 -> COMPLETED + 发布恰好一次完成事件
```

- 一次 `FireAction` 只能产生一次完成事件；`FAILED` 不得产生完成事件。
- 本模块不自行重复发射：`ATOM-14` 的一般故障恢复按顺序调用本动作恰好三次，逐次确认结果与累计计数。
- 手动、自动与 PEF 故障恢复**复用同一动作行为**，不得设置调试专用的发射路径。

## 7. 验收判定表

| 场景 | 预期结果 |
|---|---|
| `software_estop == true` | 不写任何舵机目标，无完成事件，返回 `REJECTED_ESTOP` |
| `fault_active == true` | 不写发射目标，无完成事件 |
| `authorization.allowed == false` | 按 `FireAuthorization.reason` 返回拒绝，不写发射目标 |
| 前置条件全部通过 | 先写入发射角 `fire.fire_deg`，状态进入 `HOLDING` |
| 发射角写入失败 | `FAILED`，上报 `F-SERVO-001`，无完成事件 |
| 保持时间未到 | 不产生第二次写入，状态保持 `HOLDING` |
| 保持时间到期 | 写入待机角 `fire.standby_deg` |
| 待机角写入失败 | 不置 `COMPLETED`，无完成事件 |
| 待机角写入成功 | 置 `COMPLETED`，恰好一次完成事件 |
| 动作总时长超过 `timing.fire_max_action_ms` | `FAILED` + `F-SERVO-001`，不发送完成事件 |
| 角度超出项目允许范围（0～180°） | 拒绝该目标并记录 `W-CMD-001`，不改变当前有效目标 |
| `COMPLETED` 后再次发起一次动作 | 作为新一次动作开始，且不重复发送上一次的完成事件 |
| 一般故障恢复 | 上层恰好调用三次；本模块逐次返回结果，不合并为一次不可分割操作 |
| 其他电机与其他舵机 | 本模块不产生任何其他执行器目标 |

## 8. 与相邻 Atom 的调用契约

| 关系 | 内容 |
|---|---|
| `ATOM-07` → 本模块 | 提供单通道舵机目标写入能力；本模块经自持窄接口（拟 `IServoAngleSink`）使用，不包含 `ATOM-07` 私有头 |
| 本模块 → 装配层绑定的接收端 | 发布完成事件与故障事实；最终由装配层委托 `IInputReporter::ReportFault(...)` 与相应事件出口 |
| `ATOM-13` → 本模块 | `FIRE_READY`、停止标志为 `False` 且授权通过后调用一次发射动作 |
| `ATOM-14` → 本模块 | 一般故障恢复中按顺序调用三次；期间所有机构电机零力矩、其他舵机角度不变 |
| `ATOM-02` ← 调用方 | 本模块不直接依赖：授权结果由调用方取得后经 `FireActionRequest` 传入 |
| 顺序依赖 | `ATOM-07` 必须先于本模块完成初始化（§6.3） |

## 9. 待确认 / 待接入事项

| # | 事项 | 现状与影响 | 本计划当前处理 |
|---|---|---|---|
| 1 | `fire.servo_id` / `fire.standby_deg` / `fire.fire_deg` 为 `[待定]` | `ConfigSchema.md` §2.10.3 明确禁止 `ATOM-07/10/11` 推断；`Config.md` §14.2 要求关键硬件映射缺失时初始化失败并禁止动作 | `LoadFireConfig` 在这些键不可用时如实失败；同时需装配层确认 `InitializeServoOutputs` 的 `required` 取值（取 `true` 会导致整机启动失败，直到发射舵机参数落地） |
| 2 | `timing.fire_angle_hold_ms` 与 `fire.hold_ms` 重复 | 两键同义；`ConfigSchema.md` §5 待裁决清单未登记该重复 | 暂以 `timing.*` 为权威；请裁决唯一键并同步 schema |
| 3 | 舵机写入端口的实际形态 | `ATOM-07` 的单通道写入 API 未在本轮授权范围内确认；`Rule/AtomIntegration.md` §11 第 7 项称公共调用端口尚未落地 | 本模块自持窄接口，由装配层适配；是否直接链接 `Marshland::atom_07_servo_driver` 待装配层裁决 |
| 4 | 安全事实来源 | 急停/故障/授权是注入事实，还是本模块经窄端口复核，尚未裁决 | 端口落地前按注入事实拒绝并登记为待接入；禁止注入具体 `AppCoordinator` / `StateArbitrator` 引用 |
| 5 | 完成事件契约 | `Design/Atomic/ATOM-11_Fire.md` 要求发送一次完成事件，但 `Design/LifecycleEvents.md` 只覆盖 ATOM-01 启动/关闭事件，Public 中无对应类型 | 自持窄事件 sink；载荷与消费方（`ATOM-13` / `ATOM-17` / `ATOM-18`）待确认 |
| 6 | 单调时钟来源 | `Rule/AtomIntegration.md` §8.1 称 `IMonotonicClock` 仍在 `AppCoordinator` 具体头内；但 `Atoms/Public/IMonotonicClock.hpp` 已存在、已登记进 `Marshland::PublicHeaders`，且 `ATOM-03` 已依赖它 | 内核接收显式 `now_us` 以回避该分歧；建议同步修订 `Rule/AtomIntegration.md` §8.1 |
| 7 | 驱动模型 | 外部 tick 驱动，还是单次调用内阻塞 100 ms | 采用外部 tick + 显式单调时间；若上层要求阻塞式，需先确认是否允许占用主循环 100 ms |
| 8 | 测试宿主 | `Rule/AtomIntegration.md` §9.5 禁止本模块自建 `main()`；`Tests/` 属上层目录，不在本工作目录写入范围 | 本模块只提供可注入替身的内核；测试宿主由上层装配/测试配置提供 |

以上事项在确认前不得以猜测值写入代码（`Rule/ConfigSchema.md` §1、`Rule/AtomIntegration.md` §5.5）。

## 10. 编码前置自检（提交前，取自 `Rule/AtomIntegration.md` §10）

- [ ] `Inc/` 中无任何 `ATOM-NN` 私有头，只含 `Atoms/Public/` 与标准库
- [ ] 未包含 `AppCoordinator.hpp`；未调用 `UpdateInputs()` 或字段 setter
- [ ] `ILifecycleModule` 由独立翻译单元实现，该文件 ≤ 50 行且无业务逻辑
- [ ] `initialize()` 失败时如实返回 `success = false`，`error_message` 可定位到具体设备或调用
- [ ] `shutdown()` 为 `noexcept`、幂等、对未初始化状态安全
- [ ] 配置经 `IConfigView` 按 `Rule/ConfigSchema.md` 键名读取；检查了 `ConfigStatus`，未把非 `OK` 当作成功
- [ ] 无自解析 JSON、无臆造默认值、`[待定]` 项未被填值
- [ ] 急停锁定期间本模块自身拒绝发射舵机命令，未把安全寄托于调用方查询 `RunGate()`
- [ ] 未把 `accepted == false` / `allowed == false` 当作成功
- [ ] 时间使用单调时钟；周期符合 `Config.md` §4
- [ ] 无 `main()`、无独立可执行文件；CMake 提供 `atom_11_fire` 与 `Marshland::atom_11_fire`
- [ ] aarch64 交叉编译通过且在 `-Wall -Wextra -Wpedantic` 下零警告（**仅增量编译**）
- [ ] 本 `PLAN.md` 已更新接入点、所需配置键、`[待定]` 项与顺序依赖

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | — | 初版计划：目标与边界、初步设计、文件与构建安排、待确认事项 |
| 0.2 | 2026-10-14 | 按最新规范重写：补 `Rule/AtomIntegration.md` 1.3 接入要求（依赖方向、内核+适配器两层、`InitializeServoOutputs` 注册、三个契约面、`IConfigView` 配置键表、单调时间与外部 tick、CMake 修正）；`FireActionState` 统一为 `Interface.md` §6.5 字面量；修正旧稿目标名 `atom11_fire` 为实际的 `atom_11_fire`；补验收判定表、相邻 Atom 调用契约、待确认事项与编码前置自检 |
