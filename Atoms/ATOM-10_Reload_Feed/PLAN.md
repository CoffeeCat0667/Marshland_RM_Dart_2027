# ATOM-10 Reload Feed 工作计划

> 版本：3.1
> 更新日期：2026-10-15
> 状态：**内核与生命周期适配器已实现**（观点乙），待 ARM 增量构建与零警告验证。
> 设计同步基线：PRD 0.10、PEF 1.5、Interface 1.8、Config 1.4、Atoms 0.12（2026-10-15 核实）
> 规则同步基线：AtomIntegration 1.3、ATOM5ConnectProject 1.0、**ConfigSchema 1.9**（本目录经一次性授权修订）

## 1. 文档定位与例外阅读授权

本文件是本工作目录唯一的模块工作计划，记录 ATOM-10 的接入边界、依赖、配置需求、冲突登记与执行步骤。
它不是设计文档。按本工作目录 `AGENTS.md` §1、§2，默认只读本目录、禁止读写上层目录，例外仅三项
（`Atoms/Public` 可读写、`Design/Atomic/ATOM-10_Reload_Feed.md` 只读、上层 `NewVersion/CMakeLists.txt` 只读）。

已执行的一次性例外：

| 日期 | 例外 | 结果 |
|---|---|---|
| 2026-10-14 | E-1 `Design/`、E-2 `Rule/`、E-3 `Atoms/ATOM-03_SBUS/` 只读 | 形成 v2.0 计划（见版本记录） |
| 2026-10-15 | `Design/Atomic/ATOM-10_Reload_Feed.md` 重读 | 基线未变化 |
| 2026-10-15 | `Design/` 目录文档一次性只读（PRD/PEF/Interface/Config/Atoms 相关段落） | 核实最新换弹/供弹规范，基线升版见文件头 |
| 2026-10-15 | `Rule/ConfigSchema.md` 一次性**修订**授权 | 已执行，v1.8 → v1.9（见 §5.3） |

用户裁决（2026-10-15）：

1. **C5 裁决：观点乙** —— 本目录实现"纯逻辑内核 + 生命周期适配器"，已落地。
2. **供弹机构硬件本身变更，不再使用角度控制**；ConfigSchema 修订去除角度值。
3. 换弹速度目标采用**单个幅值键** `reload.speed_rpm` = 2000 + 代码内方向约定。
4. `RuntimeStep::InitializeServoOutputs` 注册的 `required` 取值由装配层/启动策略决定，本目录无须实现。

## 2. 职责边界

### 2.1 ATOM-10 的设计边界（仅作为接口依据）

来源：`Design/Atomic/ATOM-10_Reload_Feed.md`（2026-10-10 基线）、`Design/PRD.md` §8.2/§8.3、
`Design/Interface.md` §5.1/§10.6、`Design/PEF.md` §4.1/§5、`Design/Config.md` §4/§5.7，及 2026-10-15 用户裁决。

- 换弹执行器为**同一机构左右两台 M3508**（右 id 3/node 0x203、左 id 4/node 0x204），`VELOCITY` 模式；
- 抬起：左 `+幅值`、右 `-幅值`；放下：左 `-幅值`、右 `+幅值`；幅值读 `reload.speed_rpm`（2000）；
- 两台目标**均成功提交后**才开始单调时钟计时（不以反馈到目标为起点），各按 `timing.reload_lift_run_s` /
  `timing.reload_lower_run_s`（均 3 s）运行；
- 计时到期后向两台提交 `0 rpm`，两台均成功且动作未被取消/失败时判定到位；到位与供弹等待期间保持
  **零转速速度闭环**（0 rpm 目标保持于 ATOM-05）；零转速闭环 ≠ 零力矩，不得互换；
- 手动停止、急停、单侧故障：取消动作与计时，**两侧零力矩**；迟到完成事件以动作代数（generation）拒绝；
- 不设置换弹动作超时；不执行换弹软件校准、回零，本版本不接入换弹限位；
- 初始化只建立逻辑默认"抬起"状态与两侧 `0 rpm` 目标，不主动抬起，不声称已测量物理位置；
- **供弹机构硬件已变更，不再使用角度控制**（2026-10-15 用户裁决）：动作为位置模型
  **待机位 → 换弹位 → 保持 `feed.hold_ms` → 待机位**；回到待机位目标后才标记供弹完成；
  写入失败（如 IIC 失败）不得报告供弹完成；超过 `timing.feed_max_action_ms`（1000 ms）判供弹失败；
- 供弹后 2 s 等待由 **ATOM-13/PEFExecutor 独立计时**（从供弹触发时刻起算），ATOM-10 只经
  `FeedStatus::last_trigger_micros` 提供触发事实，不重新实现该等待；
- 恢复抬起再次失败交 ATOM-14 进入 `FAULT` 锁定，不自动循环重试；
- 控制源接管边界为"当前双电机动作组完成"（经 `ReloadStatus::action_in_progress()` 暴露）；
  急停、手动停止、故障不等待该边界。

ATOM-10 **不得**实现 CAN 驱动、IIC 驱动、PID 算法、编码器解析、控制权仲裁或发射授权。

### 2.2 已实现的模块形态（观点乙）

    ATOM-10_Reload_Feed/
    ├─ AGENTS.md
    ├─ CMakeLists.txt            # STATIC/INTERFACE 自动切换 + Marshland::PublicHeaders 依赖
    ├─ PLAN.md
    ├─ Inc/atom10_reload_feed/
    │   ├─ reload_feed_types.hpp        # ReloadSide/Action/State、FeedState、命令结果与状态结构
    │   ├─ reload_feed_interfaces.hpp   # IReloadMotorPair、IFeedActuator 窄接口（能力声明）
    │   ├─ reload_feed_config.hpp       # ReloadFeedConfig + LoadReloadFeedConfig(IConfigView)
    │   ├─ reload_feed_core.hpp         # ReloadFeedCore 纯逻辑内核
    │   └─ lifecycle_adapter.hpp        # ReloadFeedLifecycleAdapter（唯一 ILifecycleModule）
    └─ Src/
        ├─ reload_feed_config.cpp       # 五键逐项查询 + ConfigStatus/范围检查
        ├─ reload_feed_core.cpp         # 换弹定时状态机 + 供弹时序内核
        └─ lifecycle_adapter.cpp        # 薄壳（≤ 50 行，无业务逻辑）

内核关键设计：

- 命令入口：`requestLift(source, kind, request_id)`（普通 `CONTROL` / 恢复 `SAFETY_RECOVERY`）、
  `requestLower`、`requestFeedOnce`；安全入口：`requestManualStop`、`notifySoftwareEstopLocked/Released`、
  `notifySideFault`；周期推进：`poll()`；状态查询：`reloadStatus()` / `feedStatus()`。
- 需裁决的动作先 `SubmitIntent` 并检查 `accepted`；`accepted == false` 不得当作成功。
- `IControlAuthority`/`IInputReporter` 采用**晚绑定**（方案 C，2026-10-15 架构裁决，见
  `Atoms/Public/GeneralBugs/20261015-Entry.md`）：构造期不注入，装配层在 AppCoordinator 构造后、
  `Start` 前调 `bindControlAuthority()`/`bindFaultReporter()`；未绑定期间需仲裁命令按
  `IntentRejected`（`NOT_READY`）fail-safe 拒绝，零力矩等安全输出不受绑定状态影响。
- 急停锁定期间模块**自身**拒绝普通出力命令（`EstopLocked`），不把安全寄托于调用方查询 `RunGate()`；
  `SAFETY_RECOVERY` 交仲裁裁决；零力矩按 `IntentKind::SAFETY` 提交（记录性质，命令无条件发出，fail-safe）。
- 机构故障态拒绝普通命令（`FaultLatched`），恢复抬起（`SAFETY_RECOVERY`）允许尝试。
- 全部计时使用 `IMonotonicClock::nowMicros()`；模块无线程、无阻塞。
- 初始化/动作失败如实返回，`error_message` 含失败键名与 `ConfigStatus`。

## 3. 目录与 CMake 契约

### 3.1 现状（2026-10-15）

`Inc/`、`Src/` 已按 §2.2 落地源码；`Src/` 非空，CMake 自动切换为 **STATIC** 库。

| 项目 | 取值 | 状态 |
|---|---|---|
| 模块目标 | `atom_10_reload_feed` | 正确 |
| 别名 | `Marshland::atom_10_reload_feed` | 正确 |
| `Marshland::PublicHeaders` 依赖 | 文件尾部 `if(TARGET ...) target_link_libraries(...)` | **已补** |
| `Threads::Threads` | 模块无线程 | 不需要 |
| `Atoms/CMakeLists.txt` 聚合 | — | **未核实**（读取受限，见 P-06） |

### 3.2 红线（不变）

无 `main()`；头文件只在 `Inc/`；源码只在 `Src/`；适配器独立翻译单元 ≤ 50 行且无业务逻辑；
`Inc/` 不包含任何其他 ATOM 的私有头，也不包含 `AppCoordinator.hpp`；不设置编译器/架构/C++ 标准/编译选项。

## 4. 接入契约面与依赖

### 4.1 三个契约面（`Rule/AtomIntegration.md` §4）

| 契约面 | ATOM-10 的落实 |
|---|---|
| 生命周期面 | `ReloadFeedLifecycleAdapter`（唯一 `ILifecycleModule` 实现） |
| 输入上报面 | 仅经注入的 `IInputReporter*`（可选）上报严重级故障；不注入 `AppCoordinator&`/`StateArbitrator&`，不调用 `UpdateInputs()` 或字段 setter |
| 仲裁面 | 动作先 `SubmitIntent` 并检查 `accepted`；零力矩 `SAFETY`、恢复抬起 `SAFETY_RECOVERY`、普通动作 `CONTROL` |

### 4.2 端口与现状

| 用途 | 契约 | 现状 |
|---|---|---|
| 生命周期 | `runtime::ILifecycleModule`（`Public/ILifecycle.hpp`） | 已接入（适配器） |
| 配置查询 | `config::IConfigView`（`Public/IConfigView.hpp`） | 已接入（装载器） |
| 单调时钟 | `app::IMonotonicClock`（`Public/IMonotonicClock.hpp`） | 已接入（内核） |
| 意图/授权 | `app::IControlAuthority::SubmitIntent` | 已接入（内核，**晚绑定** `bindControlAuthority()`） |
| 故障上报 | `app::IInputReporter::ReportFault` | 已接入（可选，晚绑定 `bindFaultReporter()`） |
| 电机命令（速度目标/零力矩/可用性） | `IReloadMotorPair`（本模块声明） | **已声明，待装配层适配 ATOM-05** |
| 供弹执行器（换弹位/待机位） | `IFeedActuator`（本模块声明，位置模型） | **已声明，待装配层适配** |
| ATOM-13 流程驱动 / ATOM-14 安全恢复 | 内核公共方法 | **已提供，待装配层接线** |

`ATOM5ConnectProject.md` §9 约定已内化为 `IReloadMotorPair` 契约注释与内核行为：
任一侧失败撤销整对目标并两侧零力矩；计时归 ATOM-10；正常到位提交 `VELOCITY = 0`，不得以零力矩替代。

### 4.3 顺序依赖与注册

真实顺序依赖（`Rule/AtomIntegration.md` §6.3）：ATOM-04 → ATOM-05 → ATOM-10；ATOM-07 → ATOM-10。
注册只能在装配层完成；`RuntimeStep::InitializeServoOutputs` 与 ATOM-11 共用同一步骤，FIFO；
`required` 取值由装配层/启动策略决定（用户 2026-10-15 明确本目录无须实现）；
`addModule()` 返回 `false` 时必须读取 `lastRegistrationError()`。

## 5. 配置接入

### 5.1 ATOM-10 实际读取的键（`LoadReloadFeedConfig`，逐项检查 `ConfigStatus`）

| 键 | 类型/单位 | 设计值 | 用途 |
|---|---|---|---|
| `reload.speed_rpm` | int / rpm | 2000（>0） | 换弹速度目标**幅值**；方向为代码内约定（抬起左正右负、放下左负右正） |
| `timing.reload_lift_run_s` | int / s | 3 | 抬起运行时间 |
| `timing.reload_lower_run_s` | int / s | 3 | 放下运行时间 |
| `feed.hold_ms` | int / ms | 100 | 换弹位保持时间 |
| `timing.feed_max_action_ms` | int / ms | 1000 | 供弹最大动作时间（超时判失败） |

不读取（归属他方）：`timing.feed_to_reload_lower_delay_s`（2 s 等待归 ATOM-13）、`motor[]` 映射与 PID
（归 ATOM-05）、`feed.servo_id`（执行器编号，归装配层适配实现方）。热加载：`configVersion()` 变化后由
装配层重新调用装载器并 `updateConfig()`。

### 5.2 ConfigSchema.md v1.9 修订内容（2026-10-15，一次性授权，已执行）

1. §2.7.1 新增 `reload.speed_rpm` 单幅值键（2000，方向模块内约定）；
2. §2.7.2 供弹机构硬件变更、不再用角度控制：`feed.angle1_deg`/`feed.angle2_deg` 作废，
   `feed.hold_ms` 承载保持时间；
3. `timing.feed_angle2_hold_ms` 标注作废；
4. 删除旧四舵机残留 `servo.strategy.reload_lift_group`/`reload_lower_group`；
   `servo.strategy.feed_sequence` 改为 `STANDBY_TO_ACTION_TO_STANDBY`；
5. §2.10.3 注明 FEED 角度字段固定 `null`；待裁决 #1/#4 收敛至 FIRE。

### 5.3 遗留（不在授权范围）

- **`Rule/ConfigSchema.schema.json` 未同步**：仍含旧 `servo_mechanisms.reload[]` 与供弹角度约束，
  缺 `reload.speed_rpm`；迁移属 ATOM-15 接入范围，待实施。
- **初始 `Config/dart_control.json` 未迁移**：同上。
- 在迁移完成前，ATOM-15 按 schema 校验可能拒绝/缺失新键——这是已登记的已知缺口，不得以代码默认值绕过。

## 6. 冲突与偏差登记

| ID | 冲突/偏差 | 状态 |
|---|---|---|
| C1 | 换弹速度目标无配置键 | **已解决**：ConfigSchema v1.9 新增 `reload.speed_rpm`（schema.json/初始 JSON 迁移遗留，见 §5.3） |
| C2 | `timing.reload_*_settle_ms` 被引用但不存在 | **已解决**（ConfigSchema v1.7 明确不存在） |
| C3 | 换弹 PID 数值（Config §5.7 `[待定]` ↔ §5.3 已给值） | 登记；ATOM-10 不读 PID，数值权威归 ATOM-05 |
| C4 | 供弹角度双份定义 | **已解决**：2026-10-15 用户裁决硬件变更、废除角度；Design 文档（PRD §8.3、Config §9.2、Atomic/ATOM-10 供弹小节）仍含角度表述，待 Design 修订 |
| C5 | 模块实现深度 | **已裁决：观点乙**（2026-10-15），已按"内核 + 适配器"落地 |
| C6 | 2 s 等待归属 | 按"ATOM-10 只提供触发事实、ATOM-13 计时"实现，已固化于内核 |
| C7 | SBUS CH12 映射 | **已解决**（Config 1.3 / ConfigSchema 1.6 统一为 CH12=变力专用） |
| C8 | CAN 接口名大小写（`CAN0` ↔ `can0`） | 旁证登记，与 ATOM-10 无直接关系 |
| C9 | Design 各文档供弹角度表述滞后于 2026-10-15 硬件变更裁决 | **新登记**：PRD §8.3、Config §5.7/§9.2、Interface §10、PEF §5、Atomic/ATOM-10 供弹小节仍描述角度式供弹；本目录无权修订 Design，等待更新 |
| C10 | Entry 集成循环依赖：机构模块构造期需 `IControlAuthority`/`IStateReader`，但 AppCoordinator 晚于模块注册（`Public/GeneralBugs/20261015-Entry.md`） | **已裁决：方案 C（晚绑定）**；ATOM-10 已落地（`bindControlAuthority`/`bindFaultReporter`，未绑定 fail-safe 拒绝）；ATOM-08/09/11 及后续 ATOM-12/13/14/18 需同法处理；已写入 `Rule/ATOM10ConnectProject.md` §5，建议固化进 `AtomIntegration.md` |

## 7. 待接入点登记

| ID | 事项 | 当前状态 |
|---|---|---|
| P-01 | 公共输入/仲裁/时钟窄端口签名 | **已落地**（`Public/` 实际头文件已核实并接入） |
| P-02 | 装配层把 ATOM-05 适配为 `IReloadMotorPair` | 未落地（装配层工作） |
| P-03 | 装配层把供弹执行器驱动适配为 `IFeedActuator` | 未落地；**适配目标受供弹硬件变更影响，待 Design 明确新机构后确定**（原路径为 ATOM-07） |
| P-04 | ATOM-13 调 `requestLift/requestFeedOnce/requestLower` 的接线 | 未落地（装配层工作） |
| P-05 | ATOM-14 安全恢复调 `requestLift(SAFETY_RECOVERY)` 的接线 | 未落地（装配层工作） |
| P-06 | `Atoms/CMakeLists.txt` 是否已聚合本模块 | **未核实**（读取受限） |
| P-07 | 注册 `required` 取值 | 用户明确：由装配层/启动策略决定，本目录无须实现 |
| P-08 | `ConfigSchema.schema.json` 与初始 JSON 迁移 | 见 §5.3（ATOM-15 接入范围） |

## 8. 构建、验证与红线

1. 代码变更必须**增量编译**；**禁止**删除构建目录或 `--clean-first`；**禁止**全量重编译。
2. 目标平台 Ubuntu ARM；**禁止** x86 编译链；只允许 WSL aarch64 交叉编译或目标机原生编译：

       cmake --build cmake-build-ubuntu-arm --target marshland -j

3. `-Wall -Wextra -Wpedantic` 零警告。
4. 模块不独立编译；**禁止**测试用 `main()` 或可执行目标。
5. **验证状态：本模块源码尚未经编译验证**——构建需经上层工程与 ARM 工具链，超出本工作目录
   可操作范围；待用户安排构建后按本条与 §8.6 自检项逐条核对。
6. 自检项（`Rule/AtomIntegration.md` §10 裁剪）落实对照：
   - `Inc/` 不含其他 ATOM 私有头与 `AppCoordinator.hpp` —— 已遵守；
   - `ILifecycleModule` 独立翻译单元 ≤ 50 行、无业务逻辑 —— `Src/lifecycle_adapter.cpp`（48 行）；
   - `initialize()` 如实返回失败、`error_message` 含键名与状态；`shutdown()` noexcept、幂等、对未初始化安全 —— 已实现；
   - 配置经 `IConfigView` 逐项检查 `ConfigStatus`，无臆造默认值、无 `[待定]` 填值 —— `Src/reload_feed_config.cpp`；
   - 安全动作分类提交（`SAFETY`/`SAFETY_RECOVERY`/`CONTROL`），`accepted == false` 不当成功 —— 内核已实现；
   - 急停锁定期间自身拒绝普通命令 —— `EstopLocked`/`FaultLatched` 自闭锁；
   - 单调时钟、周期 ≤ 100 ms 约定 —— 内核无线程，`poll()` 由装配层驱动；
   - 无 `main()`、无独立可执行文件、无全局编译选项 —— 已遵守。

## 9. 后续执行步骤

1. **编译验证（当前首要）**：经上层工程做 ARM 增量构建，零警告；若 `Atoms/CMakeLists.txt` 未聚合本模块，
   提请装配层补充（P-06）。
2. 提请 ATOM-15 负责人实施 `ConfigSchema.schema.json` 与初始 JSON 迁移（P-08）。
3. 等待 Design 对供弹新机构的修订（C9），随后确定 `IFeedActuator` 的装配层实现方（P-03）。
4. 装配层完成 P-02/P-04/P-05 接线后，联调核对 §2.1 全部行为与验收条件。
5. 后续任何代码变更仍需用户明确许可 + 增量 ARM 构建验证。

## 10. 当前状态

- 观点乙已落地：类型、窄接口、配置装载、纯逻辑内核、生命周期适配器五个头文件 + 三个源码文件；
  CMake 为 STATIC 库并声明 `Marshland::PublicHeaders` 依赖；
- ConfigSchema.md 已修订至 v1.9（`reload.speed_rpm` 入库、供弹角度模型废除）；
- 冲突 C1/C2/C4/C5/C7 已解决，C9 新登记（Design 供弹角度表述滞后）；
- **源码未经编译验证**；P-02～P-06、P-08 为装配层/他模块待办。

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | — | 初版：目录级 CMake 初始化与旧四舵机换弹模型下的组装计划。 |
| 2.0 | 2026-10-14 | 依据一次性只读例外重读 `Design/`、`Rule/`、`Atoms/ATOM-03_SBUS/`：同步设计/规则基线；改为双 M3508 + 单舵机供弹基线；更正模块目标名；新增契约面、配置键表、冲突登记（C1～C8）与待接入点（P-01～P-08）。未修改任何代码。 |
| 2.1 | 2026-10-15 | 重读原子规格确认基线未变；更正 §3.1 现状描述（空目录问题）。未修改任何代码。 |
| 3.0 | 2026-10-15 | 用户裁决 C5=观点乙、供弹硬件变更废除角度、`reload.speed_rpm` 单幅值键；经一次性授权修订 ConfigSchema.md → v1.9；实现"纯逻辑内核 + 生命周期适配器"全部源码并补 CMake 公共契约依赖；基线升至 Interface 1.8 / Config 1.4 / Atoms 0.12 / ConfigSchema 1.9；冲突 C1/C2/C4/C5/C7 解决，新登记 C9。**源码待 ARM 增量构建验证。** |
| 3.1 | 2026-10-15 | 按架构裁决实施方案 C（晚绑定）化解 Entry 集成循环依赖（C10）：`Dependencies` 移除 `authority`/`reporter`，新增 `bindControlAuthority()`/`bindFaultReporter()`，未绑定期间需仲裁命令 fail-safe 拒绝（`NOT_READY`），零力矩不受影响；新建 `Rule/ATOM10ConnectProject.md` 接入规则文档。**源码待 ARM 增量构建验证。** |
