# ATOM-10 Reload Feed 工作计划

> 版本：2.0
> 更新日期：2026-10-14
> 状态：**仅文档**。本计划未授权任何代码、头文件或 CMake 变更。
> 设计同步基线：PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8、Atoms 0.11（均 2026-10-10）
> 规则同步基线：AtomIntegration 1.3、ATOM3ConnectProject 1.2、ATOM4ConnectProject 1.2、
> ATOM5ConnectProject 1.0、ATOM15ConnectProject 1.0、ConfigSchema 1.4

## 1. 文档定位与例外阅读授权

本文件是本工作目录唯一的模块工作计划，只记录 ATOM-10 的接入边界、依赖、配置需求、冲突登记与执行步骤。
它不是设计文档，也不构成实现授权。

按本工作目录 `AGENTS.md` §1、§2，默认只读本目录、禁止读写上层目录，例外仅三项（`Atoms/Public` 可读写、
`Design/Atomic/ATOM-10_Reload_Feed.md` 只读、上层 `NewVersion/CMakeLists.txt` 只读）。

2026-10-14 用户额外授予**一次性只读**例外，用于本次计划更新：

| 例外 | 范围 | 用途 |
|---|---|---|
| E-1 | `NewVersion/Design/`（含 `Design/Atomic/`） | 读取最新设计基线 |
| E-2 | `NewVersion/Rule/` | 读取最新接入规范 |
| E-3 | `NewVersion/Atoms/ATOM-03_SBUS/` | 读取 ATOM 通用接入形态的既有参照 |

约束与说明：

1. 三个例外均为**只读**，且为一次性；不改变 `AGENTS.md` 的默认范围，不授权写入上述目录。
2. E-3 执行时**未**读取 `Atoms/ATOM-03_SBUS/AGENTS.md`（`AGENTS.md` §1 禁止读取任何其他 `AGENTS.md`）。
3. 下列内容**不在**本次例外内，因此本计划中的相关结论一律标注"未核实"：
   `NewVersion/Atoms/CMakeLists.txt`、`NewVersion/Entry.cpp`、`NewVersion/Config/dart_control.json`、
   除 ATOM-03 外的其他 `Atoms/ATOM-*` 目录。
   上层 `NewVersion/CMakeLists.txt` 虽在 `AGENTS.md` 例外内（只读），本次未读取。

## 2. 职责边界与未决冲突

### 2.1 ATOM-10 的设计边界（仅作为接口依据，不授权在本目录复制实现）

来源：`Design/Atomic/ATOM-10_Reload_Feed.md`、`Design/PRD.md` §8.2/§8.3、`Design/Interface.md` §5.1/§10.6、
`Design/PEF.md` §4.1/§5、`Design/Config.md` §4/§5.6。

- 换弹执行器为**同一机构左右两台 M3508**（右 id 3/node 0x203、左 id 4/node 0x204），不是舵机；
- 抬起：左 `+2000 rpm`、右 `-2000 rpm`；放下：左 `-2000 rpm`、右 `+2000 rpm`；
- 两台目标**均成功提交后**才开始计时（不以反馈到目标为计时起点），各按 Config 运行 3 s；
- 计时到期后向两台提交 `0 rpm`，两台均成功且动作未被取消/失败时判定到位；
- 到位与供弹等待期间保持**零转速速度 PID**；零转速闭环与零力矩是两种状态，不得互换；
- 手动停止、急停、单侧故障：取消动作与计时，**两侧零力矩**；迟到完成事件必须被拒绝；
- 不设置换弹动作超时；不执行换弹软件校准、回零，本版本不接入换弹限位；
- 初始化只建立逻辑默认"抬起"状态与两侧 `0 rpm` 目标，不主动抬起；
- 供弹为**单舵机**：角度一 → 角度二 → 保持 100 ms → 角度一；IIC 写入失败不得报告供弹完成；
- 供弹后 2 s 等待由 **PEFExecutor 独立计时**（从供弹触发时刻起算），ATOM-10 不重新实现该等待；
- 恢复抬起再次失败交 ATOM-14 进入 `FAULT` 锁定，不自动循环重试；
- 控制源接管边界为"当前双电机动作组完成"；急停、手动停止、故障不等待该边界。

ATOM-10 **不得**实现 CAN 驱动、IIC 驱动、PID 算法、编码器解析、控制权仲裁或发射授权。

### 2.2 本工作目录允许的工作类型

1. 组织 ATOM-10 的模块目标、依赖与包含关系；
2. 定义 ATOM-10 对外需要的窄接口（能力声明），由装配层注入 ATOM-05 / ATOM-07 / ATOM-13 / ATOM-14 的实现；
3. 接入公共契约（`ILifecycleModule`、`IConfigView`、`IMonotonicClock`、
   `IControlAuthority`、`IInputReporter`、`MarshlandTypes.hpp`、`StateArbitrationTypes.hpp`）；
4. 提供本模块的 CMake 目标、配置键清单与接口一致性检查；
5. 维护本文件（`PLAN.md`）。

以上第 2～4 项涉及实际文件变更时，**必须先获得用户明确授权**。

### 2.3 冲突 C5：模块实现深度未裁决（**阻止开工项**）

| 观点 | 依据 | 结论 |
|---|---|---|
| 甲：本工程只做网关与模块组装，禁止实现 `Design/Atomic` 描述的功能 | 上层工程级 `AGENTS.md`（由运行环境注入；本工作目录规则不授权读取其文件）「工程职责与禁止事项」 | ATOM-10 只应保留组装占位，不得写换弹/供弹逻辑 |
| 乙：每个 ATOM 都要实现"纯逻辑内核 + 生命周期适配器" | `Rule/AtomIntegration.md` §2；且 `Atoms/ATOM-03_SBUS`、`ATOM-04_CAN`、`ATOM-05_Motor_PID` 已按此形态落地（见各自 ConnectProject 规则） | ATOM-10 应实现换弹定时动作与供弹时序内核 |

两条约束结论相反，`Rule/AtomIntegration.md` 未就 ATOM-10 给出裁决，两种形态在同级目录中并存。
**在用户明确裁决前，本目录不得新增任何实现代码**；当前 `CMakeLists.txt` 为无源码的 `INTERFACE` 占位，
与观点甲一致，但这是既成事实，不构成对乙的否定。

## 3. 目录与 CMake 契约

### 3.1 现状（已核实）

`ATOM-10_Reload_Feed/` 当前只有 `AGENTS.md`、`CMakeLists.txt`、`PLAN.md`，无 `Inc/`、无 `Src/`。

`CMakeLists.txt` 已符合统一模块契约：不定义 `project()`、不产生可执行文件、不设置编译器/架构/C++ 标准、
按 `Src/` 是否存在在 `STATIC` 与 `INTERFACE` 之间切换、包含目录为本目录 + `Inc/` + `../Public`、
提供别名 `Marshland::atom_10_reload_feed`。

| 项目 | 取值 | 状态 |
|---|---|---|
| 模块目标 | `atom_10_reload_feed` | 与 `AtomIntegration.md` §1.5 的 `atom_NN_<名称>` 一致 |
| 别名 | `Marshland::atom_10_reload_feed` | 一致 |
| 旧计划书中的 `atom10_reload_feed` | — | **错误写法，本版已更正**，不得用于链接 |

### 3.2 已识别的 CMake 缺口（需授权后修改）

1. 未链接 `Marshland::PublicHeaders`。`ATOM-03`、`ATOM-04` 均在文件尾部按
   `if(TARGET Marshland::PublicHeaders) target_link_libraries(...)` 声明该依赖；ATOM-10 只包含
   `../Public` 的包含路径，未声明目标级依赖。使用 Public 契约前应补上（**待授权**）。
2. 若内核使用线程，需要 `Threads::Threads`（参照 `ATOM5ConnectProject.md` §2）。当前无源码，暂不需要。
3. `Atoms/CMakeLists.txt` 是否已包含 `add_subdirectory(ATOM-10_Reload_Feed)` 与聚合目标 ——
   **未核实**（不在允许读取范围）。

### 3.3 候选目录结构（仅在观点乙被采纳时使用，**待授权**）

    ATOM-10_Reload_Feed/
    ├─ AGENTS.md
    ├─ CMakeLists.txt
    ├─ PLAN.md
    ├─ Inc/atom10_reload_feed/
    │   ├─ reload_feed_types.hpp        # 动作、状态、结果等领域类型
    │   ├─ reload_feed_interfaces.hpp   # 本模块所需窄接口（能力声明，不含实现）
    │   ├─ reload_feed_config.hpp       # IConfigView -> 参数
    │   ├─ reload_feed_core.hpp         # 纯逻辑内核
    │   └─ lifecycle_adapter.hpp        # 唯一 ILifecycleModule 实现（薄壳 ≤ 50 行）
    └─ Src/
        ├─ reload_feed_config.cpp
        ├─ reload_feed_core.cpp
        └─ lifecycle_adapter.cpp

约束：无 `main()`；头文件只在 `Inc/`；源码只在 `Src/`；适配器不与内核实现混写；
适配器内不得有业务逻辑；不得包含任何其他 ATOM 的私有头或 `AppCoordinator.hpp`。

## 4. 接入契约面与依赖

### 4.1 三个契约面（`Rule/AtomIntegration.md` §4）

| 契约面 | ATOM-10 的角色 |
|---|---|
| 生命周期面 | 实现 `ILifecycleModule`（唯一实现类为 `lifecycle_adapter`） |
| 输入上报面 | 只经窄端口/回调；不得注入具体 `AppCoordinator&`/`StateArbitrator&`；不得调用 `UpdateInputs()` 或字段 setter |
| 仲裁面 | 需裁决的动作先 `SubmitIntent` 并检查 `accepted`；禁止把 `accepted == false` 当成功 |

### 4.2 所需端口与现状（已核实 `Atoms/Public/` 实际内容）

| 用途 | 契约 | 位置 | 现状 |
|---|---|---|---|
| 生命周期 | `runtime::ILifecycleModule`、`RuntimeContext`、`ModuleInitResult` | `Public/ILifecycle.hpp` | **已存在** |
| 配置查询 | `config::IConfigView`、`ConfigStatus` | `Public/IConfigView.hpp` | **已存在**（ATOM-15 实现，装配层注入） |
| 单调时钟 | `app::IMonotonicClock::nowMicros()` | `Public/IMonotonicClock.hpp` | **已存在** |
| 基础类型 | `SystemMode`、`ControlSource`、`AutoFireState`、`FaultSeverity` | `Public/MarshlandTypes.hpp` | **已存在** |
| 意图/授权 | `app::IControlAuthority::SubmitIntent`、`IntentKind`、`IntentDecision` | `Public/AppInterfaces.hpp`、`Public/StateArbitrationTypes.hpp` | **已存在** |
| 状态读取 | `app::IStateReader::ReadState()`、`RunGateReason` | `Public/AppInterfaces.hpp` | **已存在** |
| 输入上报 | `app::IInputReporter::ReportFault` 等 | `Public/AppInterfaces.hpp` | **已存在** |
| 电机命令（速度目标 / 零力矩 / 停通道） | 需在 ATOM-10 内声明窄接口，由装配层适配 ATOM-05 | — | **未落地** |
| 供弹舵机目标（角度一/角度二 + 保持） | 需窄接口，由装配层适配 ATOM-07 | — | **未落地** |
| 换弹侧故障联动（ATOM-05 → ATOM-10 取消动作与计时） | 需窄接口 | — | **未落地** |
| 流程驱动（ATOM-13 调抬起/供弹/放下） | 需窄接口或装配层注入 | — | **未落地** |
| 安全恢复抬起（ATOM-14 调抬起） | 需窄接口 | — | **未落地** |

`ATOM5ConnectProject.md` §9 已给出 ATOM-10 侧的既有约定，须遵守：

- 分两侧提交速度目标时，任一侧被拒绝或失败，必须撤销整对目标（`stopMotor(3)` / `stopMotor(4)` 等价能力）并取消动作与计时；
- 两个调用不是事务，ATOM-05 内**没有**换弹动作计时器，计时由 ATOM-10 负责；
- 正常到位必须提交两侧 `VELOCITY = 0`（有效闭环目标）；**不得**用 `DISABLED` 替代零转速保持。

### 4.3 顺序依赖与注册步骤

真实顺序依赖（`Rule/AtomIntegration.md` §6.3）：

    ATOM-04 (CAN)   必须先于 ATOM-05 / ATOM-12
    ATOM-05 (Motor) 必须先于 ATOM-10
    ATOM-07 (IIC)   必须先于 ATOM-10 / ATOM-11

注册步骤（`Rule/AtomIntegration.md` §3）：`RuntimeStep::InitializeServoOutputs` 由
ATOM-10（供弹舵机及机构初始状态）与 ATOM-11（发射舵机）共用同一步骤，同步骤内按注册先后 FIFO 执行。
`required` 必须显式书写，最终取值由启动策略决定（ATOM-10 自身不持有设备，硬件资源属 ATOM-05/ATOM-07）。

注册只能在装配层完成；ATOM-10 不得自行注册，也不得注册其他模块；
`addModule()` 返回 `false` 时必须读取 `lastRegistrationError()`。

## 5. 配置接入

### 5.1 读取方式

一律经 `IConfigView` 按 `Rule/ConfigSchema.md` 的键名查询，逐项检查 `ConfigStatus`；
非 `OK` 时禁止使用该值，也不得臆造默认值或填充 `[待定]` 项。ATOM-10 不得打开或解析 JSON。
时序必须使用单调时钟，`configVersion()` 变化后应重新读取。

### 5.2 ATOM-10 需要的键

| 键 | 类型/单位 | 设计值 | 用途 | 状态 |
|---|---|---|---|---|
| `timing.reload_lift_run_s` | int / s | 3 | 抬起运行时间 | 已定义 |
| `timing.reload_lower_run_s` | int / s | 3 | 放下运行时间 | 已定义 |
| `timing.feed_angle2_hold_ms` | int / ms | 100 | 角度二保持 | 已定义 |
| `timing.feed_max_action_ms` | int / ms | 1000 | 供弹最大动作时间 | 已定义 |
| `timing.feed_to_reload_lower_delay_s` | int / s | 2 | 供弹后等待放下 | 已定义，但**归属 ATOM-13/PEFExecutor**，ATOM-10 只提供触发事实（见 C6） |
| `feed.servo_id` | int | 5 | 供弹舵机编号 | 已定义 |
| `feed.angle1_deg` | int / deg | `[待定]` | 待机角度 | 值待定，禁止推断 |
| `feed.angle2_deg` | int / deg | `[待定]` | 换弹位角度 | 值待定，禁止推断 |
| `feed.hold_ms` | int / ms | 100 | 角度二保持 | 已定义 |
| 换弹速度目标幅值（±2000 rpm） | int / rpm | 2000 | 左右抬起/放下速度 | **ConfigSchema 无对应键（C1）** |
| `motor[].id/node/role`（3=右换弹、4=左换弹） | — | 0x203 / 0x204 | 电机映射 | 归 ATOM-05 持有；ATOM-10 经窄接口使用，不直接解析数组 |

### 5.3 配置缺口（不得以代码默认值绕过）

1. **换弹速度目标无键**：`Design/Config.md` §5.6 把 ±2000 rpm 列为配置项，
   但 `Rule/ConfigSchema.md` 的 `reload.*` 仍是旧四舵机模型，没有速度键（见 C1）。
2. **`timing.reload_*_settle_ms` 不存在**：`ConfigSchema.md` §2.6.1 的 `reload[].settle_ms`
   引用该键，但 §2.3 的 timing 表没有它（旧 500 ms 舵机延时 / 1000 ms 换弹超时已按
   `Config.md` §4 取消，见 C2）。
3. **供弹角度双份来源**：`feed.angle1_deg`/`feed.angle2_deg`（§2.6.2）与
   `servo.channels[].standby_deg`/`action_deg`（§2.10.3，舵机 1–5 一律 `null`）并存（见 C4）。

以上缺口只能通过**修订 `Rule/ConfigSchema.md`** 或由用户明确指定读取路径来解决，
不得在本模块内硬编码数值。

## 6. 冲突与偏差登记

| ID | 冲突/偏差 | 冲突双方 | 影响 ATOM-10 的方式 | 处理 |
|---|---|---|---|---|
| C1 | 换弹参数模型：四舵机 vs 双 M3508 | `Rule/ConfigSchema.md` §2.6.1（`reload[]`=舵机 1–4，`servo_id`/`standby_deg`/`lift_deg`/`lower_deg`/`settle_ms`） ↔ `Design/Config.md` §5.6、`Design/Interface.md` §10.6、`Design/PRD.md` §8.2 | 无键可读换弹速度目标与双电机映射 | **阻止实现**：需修订 ConfigSchema（属 `Rule/`，本目录无权修改） |
| C2 | `timing.reload_*_settle_ms` 被引用但不存在 | `Rule/ConfigSchema.md` §2.6.1 ↔ 同文件 §2.3、`Design/Config.md` §4 | 旧舵机到位延时键会误导实现 | 登记；不得实现该键 |
| C3 | 换弹 PID 数值 | `Design/Config.md` §5.6「PID 参数…具体值 `[待定]`」 ↔ 同文件 §5.3（电机 3/4 已给 3/0/0/16000/16000/20ms）、`Rule/ATOM5ConnectProject.md` §8 | ATOM-10 不读 PID，影响间接 | 登记；数值权威归 ATOM-05 |
| C4 | 供弹角度双份定义 | `Rule/ConfigSchema.md` §2.6.2 ↔ §2.10.3（该文件「待裁决」#1、#4 未决） | 供弹角度无唯一权威键 | 登记；禁止推断 |
| C5 | 模块实现深度 | 上层工程级 `AGENTS.md`「禁止实现 `Design/Atomic` 功能」 ↔ `Rule/AtomIntegration.md` §2 | 决定本目录是否写内核 | **阻止开工**：等待用户裁决（§2.3） |
| C6 | 2 s 等待的归属表述 | `Design/Atomic/ATOM-10_Reload_Feed.md`「供弹动作内部原子操作」第 2、5 条把计时写在 ATOM-10 小节内 ↔ 同条明确"PEFExecutor 独立等待"，`Design/Atomic/ATOM-13_Auto_Fire.md` 第 5 条 | 可能被误解为 ATOM-10 自行计时 | 按"ATOM-10 只提供触发事实、ATOM-13 计时"实现；已在 §2.1 固化 |
| C7 | SBUS CH12 映射 | `Design/Interface.md` §5.2（CH12～CH16 保留） ↔ `Rule/ATOM3ConnectProject.md` §8、`Design/Config.md` §7.1（CH12=变力速度） | 与 ATOM-10 无直接关系，但证明 Design/Rule 间存在未同步项 | 旁证登记；不作为 ATOM-10 依据 |
| C8 | CAN 接口名大小写 | `Rule/ConfigSchema.md` §2.7.1 / 初始 JSON 写 `CAN0` ↔ `Rule/ATOM4ConnectProject.md` §10 实际用 `can0` | 与 ATOM-10 无直接关系 | 旁证登记 |

本目录**不具备**修改 `Rule/` 或 `Design/` 的权限；上表只能登记并提出修订请求，不得自行改写。

## 7. 待接入点登记

| ID | 事项 | 当前状态 |
|---|---|---|
| P-01 | 公共输入/仲裁/时钟窄端口的最终签名 | `Rule/AtomIntegration.md` §11 第 7 条：尚未落地 |
| P-02 | 装配层把 ATOM-05 适配为 ATOM-10 的电机命令窄接口 | 未落地 |
| P-03 | 装配层把 ATOM-07 适配为 ATOM-10 的供弹舵机窄接口 | 未落地 |
| P-04 | ATOM-13 请求抬起/供弹/放下的调用链 | 未落地 |
| P-05 | ATOM-14 安全恢复请求换弹抬起（`IntentKind::SAFETY_RECOVERY`） | 未落地；锁定阶段禁止出力动作 |
| P-06 | 换弹速度目标配置键 | 见 C1 |
| P-07 | ATOM-10 的 `RuntimeStep::InitializeServoOutputs` 注册与 `required` 取值 | 需装配层决定 |
| P-08 | `Atoms/CMakeLists.txt` 是否已聚合本模块 | **未核实**（读取受限） |

## 8. 构建、验证与红线

1. 代码变更必须**增量编译**；**禁止**删除构建目录或 `--clean-first`；**禁止**全量重编译。
2. 目标平台为 Ubuntu ARM；**禁止** x86 编译链。只允许 WSL aarch64 交叉编译或目标机原生编译：

       cmake --build cmake-build-ubuntu-arm --target marshland -j

3. 编译必须在 `-Wall -Wextra -Wpedantic` 下零警告。
4. 模块不独立编译；**禁止**创建测试用 `main()`，也不得创建可执行目标。
5. ATOM-10 自身不设置编译器、架构、C++ 标准或编译选项。
6. 自检项（源自 `Rule/AtomIntegration.md` §10，按本模块裁剪）：
   - `Inc/` 内不包含任何其他 ATOM 的私有头，也不包含 `AppCoordinator.hpp`；
   - `ILifecycleModule` 由独立翻译单元实现，该文件 ≤ 50 行且无业务逻辑；
   - `initialize()` 如实返回失败，`error_message` 足以定位；`shutdown()` 为 `noexcept`、幂等、对未初始化安全；
   - 配置经 `IConfigView` 读取并检查 `ConfigStatus`，无臆造默认值、无 `[待定]` 填值；
   - 安全动作按类别提交：零力矩用 `IntentKind::SAFETY`；换弹抬起用 `IntentKind::SAFETY_RECOVERY`；普通动作用 `CONTROL`；
   - 急停锁定阶段（`SystemMode == SOFTWARE_ESTOP`）不得提交任何出力动作；
   - `SubmitIntent` 的 `accepted == false` 不得当作成功；
   - 不把安全寄托于调用方查询 `RunGate()`；锁定期间自身拒绝普通命令；
   - 时间用单调时钟；周期符合 `Config.md` §4（系统最长控制周期 100 ms）；
   - CMake 提供 `atom_10_reload_feed` + `Marshland::atom_10_reload_feed`，无全局编译选项；
   - 无 `main()`、无独立可执行文件。

## 9. 后续执行步骤

在 C5 裁决之前，仅允许第 1 步这类**只读**工作：

1. 只读核对：经用户授权后检查 `Atoms/CMakeLists.txt`、`NewVersion/Entry.cpp`、
   `Config/dart_control.json` 与 ATOM-05/07/13/14 的实际导出接口，确认 §4.2 的端口缺口与 §5.2 的键缺口。
2. 提交 C1、C2、C4 的 `Rule/ConfigSchema.md` 修订请求（由具备 `Rule/` 权限的负责人执行）。
3. C5 裁决后，按裁决结果二选一：
   - 观点甲：保持 `INTERFACE` 占位，只补 `Marshland::PublicHeaders` 依赖声明与接口一致性说明；
   - 观点乙：按 §3.3 结构实现内核与适配器（先接口冻结，再实现，再静态验证）。
4. 任何代码变更前再次取得用户明确许可。
5. 变更后按 §8 做增量 ARM 构建与零警告验证，并逐条核对 §8.6 自检项。

## 10. 当前状态

- 目录级 CMake 已初始化，目标与别名正确，当前无 `Inc/`、`Src/`，模块为 `INTERFACE` 占位；
- 本计划已按 2026-10-10 设计基线与 2026-10-11～10-14 规则基线重写；
- 已登记 8 项冲突/偏差（C1～C8）与 8 项待接入点（P-01～P-08）；
- **C5 与 C1 为阻止开工项**：前者决定是否实现，后者决定换弹速度目标从何处读取；
- 未新增任何功能代码；在 C5 裁决与用户授权之前不创建实现文件。

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | — | 初版：目录级 CMake 初始化与旧四舵机换弹模型下的组装计划。 |
| 2.0 | 2026-10-14 | 依据一次性只读例外重读 `Design/`、`Rule/`、`Atoms/ATOM-03_SBUS/`：同步 PRD 0.10 / PEF 1.5 / Interface 1.5 / Config 0.8 / Atoms 0.11 与 AtomIntegration 1.3 等规则；改为双 M3508 + 单舵机供弹基线；更正模块目标名为 `atom_10_reload_feed`；新增接入契约面、配置键表、冲突登记（C1～C8）、待接入点（P-01～P-08）与阻止开工项。**未修改任何代码或 CMake 文件。** |
