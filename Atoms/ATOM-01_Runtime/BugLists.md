# ATOM-01_Runtime 缺陷清单（BugLists）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 对象模块 | `Atoms/ATOM-01_Runtime` |
| 权威来源 | `Design/Atomic/ATOM-01_Runtime.md` |
| 接口权威 | `Design/Interface.md` §4.2、§4.3、§5.1、§6.2、§7 |
| 配置权威 | `Design/Config.md` §3、§13、§15 |
| 流程权威 | `Design/PEF.md` §7、§8 |
| 登记日期 | 2026-09-27 |
| 本轮行为 | **仅登记缺陷，未修改任何 Atom 代码**；`Inc/`、`Src/`、`CMakeLists.txt` 均保持原样 |

本文件只记录本次审查发现的缺陷与风险，不提出实现方案、不产出补丁、不修改任何源码。所有条目均给出"证据"，即具体文件与行号，便于逐条复核。

### 0.1 本次审查范围

- `Inc/RuntimeCoordinator.hpp`（116 行）
- `Src/RuntimeCoordinator.cpp`（216 行）
- `PLAN.md`（70 行，用于区分"已知未完成"与"实现缺陷"）

### 0.2 已确认正确、不作为缺陷登记的行为

为避免后续复核重复排查，以下行为经逐条核对后**判定正确**：

1. 必需模块失败时立即进入 `InitFailed` 并逆序释放已启动模块，符合 `ATOM-01_Runtime.md` 验收条件第 3 条。
2. `failStart()` 与 `shutdown()` 均把 `started` 置回 `false`，因此析构时的二次 `shutdown()` 不会对同一模块重复调用 `shutdown()`。
3. `start()` 在非 `Created` 状态下重复调用时返回既有结果而不重新初始化，避免重复初始化副作用。
4. `addModule()` 拒绝重复 `RuntimeStep`，保证每个步骤唯一。
5. 全部 `ILifecycleModule` 调用点均有 `try/catch(...)` 兜底，模块抛异常不会终止启动流程。

---

## 1. 缺陷汇总

| 编号 | 标题 | 严重度 | 位置 |
|---|---|---|---|
| ATOM01-BUG-001 | 本地 `RuntimeState` 与权威 `SystemMode` 双轨并存，启动结果从不驱动系统状态机 | 高 | `Inc/RuntimeCoordinator.hpp` L11–18；`Src/RuntimeCoordinator.cpp` 全文 |
| ATOM01-BUG-002 | `canExecuteActuatorCommands()` 以本地状态为判据，会把手动/调试/校准模式锁死 | 高 | `Src/RuntimeCoordinator.cpp` L173–176 |
| ATOM01-BUG-003 | `shutdown()` 之后协调器不可重启，且失败仅表现为"静默返回" | 中 | `Src/RuntimeCoordinator.cpp` L98–102、L143–150 |
| ATOM01-BUG-004 | `start()` 的异常保护不覆盖内存分配与结果写回路径 | 中 | `Src/RuntimeCoordinator.cpp` L104–131 |
| ATOM01-BUG-005 | `ModuleRegistration::required` 无默认值，漏填时行为未定义且可产生安全误判 | 中 | `Inc/RuntimeCoordinator.hpp` L73–77；`Src/RuntimeCoordinator.cpp` L81–86 |
| ATOM01-BUG-006 | `RuntimeContext` 以字符串字典占位，无配置结构、无必需项校验、无 PID 加载 | 中 | `Inc/RuntimeCoordinator.hpp` L37–42；`Src/RuntimeCoordinator.cpp` 全文 |
| ATOM01-BUG-007 | 模块间共享契约（`ILifecycleModule` 等）未落入 `Atoms/Public` | 中 | `Inc/RuntimeCoordinator.hpp` L37–77 |
| ATOM01-BUG-008 | 无结构化步骤事件，不满足"每次状态迁移须可查询且有原因" | 低 | `Src/RuntimeCoordinator.cpp` L108–137 |
| ATOM01-BUG-009 | 每步骤仅允许注册一个模块，与设计文档按"能力"而非"步骤"列举模块的口径存在张力 | 低 | `Inc/RuntimeCoordinator.hpp` L73–77；`Src/RuntimeCoordinator.cpp` L75–79 |

---

## 2. 缺陷详述

### ATOM01-BUG-001 本地 `RuntimeState` 与权威 `SystemMode` 双轨并存，启动结果从不驱动系统状态机

**严重度**：高 → **不修复（2026-09-27，v0.4）**

> **处置**：按现有分层设计保留。ATOM-01 只表达硬件生命周期（`RuntimeState`），业务 `SystemMode` 的映射唯一由 `Atoms/Public/AppCoordinator` 承担（见 `Atoms/ATOM-01_Runtime/PLAN.md` §5.1）。本条不再视为待修缺陷，**不实施**"由 `RuntimeCoordinator` 驱动 `SystemMode`"（那会形成第二驱动源）。以下保留原始记录供追溯。

**现象**

`RuntimeCoordinator` 持有自有的 6 值状态枚举：

```cpp
// Inc/RuntimeCoordinator.hpp L11-18
enum class RuntimeState {
    Created,
    Starting,
    Standby,
    InitFailed,
    ShuttingDown,
    Stopped,
};
```

而全工程权威状态模型是 `Atoms/Public/MarshlandTypes.hpp` L20–32 的 `SystemMode`（11 项，逐字取自 `Design/Interface.md` §6.2）：

```cpp
enum class SystemMode : std::uint8_t {
    UNINITIALIZED, INITIALIZING, INIT_FAILED,
    STANDBY, MANUAL, AUTO_FIRE, DEBUG, CALIBRATION,
    RECOVERY, SOFTWARE_ESTOP, FAULT
};
```

两者存在三重冲突：

1. **语义重复**：`RuntimeState::Standby` 与 `SystemMode::STANDBY`、`RuntimeState::InitFailed` 与 `SystemMode::INIT_FAILED` 表达同一含义却各自定义。
2. **无法映射**：`RuntimeState::Stopped`、`Created`、`ShuttingDown` 在 `Config.md` §3.1「功能模式」表中**没有任何对应模式**。
3. **链路断裂**：`Src/RuntimeCoordinator.cpp` 全文没有出现 `SystemMode`、`StateArbitrator` 或 `RequestModeTransition` 的任何调用。

**证据**

- `Inc/RuntimeCoordinator.hpp` L11–18：本地状态枚举定义。
- `Atoms/Public/MarshlandTypes.hpp` L20–32：权威 `SystemMode` 定义。
- `Design/Interface.md` §4.3「启动顺序」第 9 条：要求"建立系统状态为'待机'或'初始化失败'"。
- `Design/Atomic/ATOM-01_Runtime.md` 内部原子操作第 13、14 条：明确以 `STANDBY` / `INIT_FAILED` 作为切换目标。
- `Src/RuntimeCoordinator.cpp` L139、L200：仅写入本地 `RuntimeState`。

**影响**

启动协调完成（`RuntimeState::Standby`）后，权威状态机中的 `SystemMode` 仍停留在 `UNINITIALIZED`。按 `Design/Interface.md` §7.2 第 1 条「未完成初始化不得进入自动发射或手动动作状态」，系统将被上层判定为未初始化，**初始化成功这一结果无法传递给状态机与对外状态接口**。

**修复方向（仅记录，不在本轮实施）**

取消平行的本地状态枚举，由 `RuntimeCoordinator` 通过 `RequestModeTransition` 驱动同一份 `SystemMode`；若必须保留本地生命周期细分（`Created`/`ShuttingDown`），需建立显式映射表并在转换点显式通知状态机。

---

### ATOM01-BUG-002 `canExecuteActuatorCommands()` 以本地状态为判据，会把手动/调试/校准模式锁死

**严重度**：高 → **已修复（2026-09-27，v0.3）**

> **修复实施**：按用户授权**移除** `RuntimeCoordinator::canExecuteActuatorCommands()`（`Inc/RuntimeCoordinator.hpp`、`Src/RuntimeCoordinator.cpp`）。移除前全仓库检索确认无其他调用点，对应职责已由 `Atoms/Public/AppCoordinator::RunGate()` / `CanRunActuators()` 承担。本模块此后只暴露 `state()` / `stepResults()` 等硬件生命周期事实。以下保留原始记录供追溯。

**现象**

```cpp
// Src/RuntimeCoordinator.cpp L173-176
bool RuntimeCoordinator::canExecuteActuatorCommands() const noexcept
{
    return state_ == RuntimeState::Standby;
}
```

该判据只承认"待机"可执行执行器命令。

**证据**

- `Design/Interface.md` §7.1 状态转换总表：`待机 → 进入手动 → 手动`，主要动作为「接受控制源命令」。
- `Design/Config.md` §11.4：`charge_start`、`reload_lift`、`feed_once`、`force_set`、`yaw_set`、`servo_set`、`fire_once` 等动作**允许模式**明确包含「手动、调试」。
- `Design/Config.md` §11.2：`/api/calibration` 的调用即进入校准模式，校准过程需要执行机构动作。

**影响**

即使协调器自身状态已是 `Standby`，一旦系统进入 `MANUAL`、`DEBUG`、`CALIBRATION` 或 `AUTO_FIRE`，该方法仍会返回 `false`（因为它只看本地 `RuntimeState`，而这些模式下本地状态可能已不是 `Standby`，在 ATOM01-BUG-001 修复后更会彻底失配）。上层若以该方法作为安全闸门，将**直接阻断 P1 优先级的手动控制能力**。

**修复方向（仅记录，不在本轮实施）**

判据应基于权威 `SystemMode` 的可操作模式集合（`ATOM-02` 的 `IsOperationalMode()` 已定义 `STANDBY/MANUAL/AUTO_FIRE/DEBUG/CALIBRATION`），并叠加"初始化已完成"这一独立布尔条件，二者不可混为一谈。

---

### ATOM01-BUG-003 `shutdown()` 之后协调器不可重启，且失败仅表现为"静默返回"

**严重度**：中 → **已修复（2026-09-27，v0.3）**

> **修复实施**：采纳"允许重启"。`start()` 在 `Stopped` 状态下先回到 `Created`、清空 `step_results_` 并复位各模块 `started` 标志，再重新执行启动序列。`InitFailed` **不**直接重启（`Design/Interface.md` §7.1 无 `INIT_FAILED -> INITIALIZING` 边），必须先经 `shutdown()` 回到 `Stopped`，即"重启前必须有一次显式关闭"。以下保留原始记录供追溯。

**现象**

```cpp
// Src/RuntimeCoordinator.cpp L143-150
void RuntimeCoordinator::shutdown() noexcept
{
    if (state_ == RuntimeState::Stopped || state_ == RuntimeState::Created) {
        if (state_ == RuntimeState::Created) {
            state_ = RuntimeState::Stopped;
        }
        return;
    }
    ...
    state_ = RuntimeState::Stopped;
}
```

```cpp
// Src/RuntimeCoordinator.cpp L98-102
RuntimeStartResult RuntimeCoordinator::start()
{
    if (state_ != RuntimeState::Created) {
        return RuntimeStartResult{state_, step_results_};
    }
    ...
}
```

`shutdown()` 把状态单向推进到 `Stopped` 且永不回到 `Created`，而 `start()` 仅在 `Created` 下工作。

**影响**

1. 任何一次 `shutdown()` 之后，`start()` 都只会返回既有结果，**不再执行任何初始化**；调用方无法从返回值上区分"未启动"与"已停止"以外的语义（`RuntimeState::Stopped` 会出现在 `RuntimeStartResult.state` 中）。
2. 析构函数（L64–67）会调用 `shutdown()`，因此"构造 → 析构 → 再构造"以外的复用路径全部失效。
3. 与 `ATOM-01_Runtime.md` 安全约束「启动失败须提供模块名和错误原因给状态/诊断接口」相比，重启被拒绝这一情况**没有任何错误原因可查询**。

**修复方向（仅记录，不在本轮实施）**

明确界定"单次生命周期"语义：若允许重启，`Stopped` 应可回到 `Created` 并清空 `step_results_`；若不允许，`start()` 在非 `Created` 状态下应返回显式拒绝结果而非复用上一次的结果结构。

---

### ATOM01-BUG-004 `start()` 的异常保护不覆盖内存分配与结果写回路径

**严重度**：中 → **已修复（2026-09-27，v0.3）**

> **修复实施**：`start()` 整个启动序列纳入统一 `try/catch(...)`；异常时 `failStart()` 回滚为 `InitFailed` 并释放已启动模块；新增私有 `currentResult()` 兜底构造返回值（其内部亦捕获分配失败，退化为"只有状态"）。`push_back` 改为 `std::move`（配合已 `reserve` 的容量，消除扩容与元素拷贝失败点）。`start()` 声明为 `noexcept`，与 `AppCoordinator::Start` 的 `noexcept` 契约一致，消除异常外泄导致的 `std::terminate` 风险。以下保留原始记录供追溯。

**现象**

`start()` 的 `try/catch` 只包住单次模块 `initialize()` 调用：

```cpp
// Src/RuntimeCoordinator.cpp L104-131（节选）
state_ = RuntimeState::Starting;
step_results_.clear();
step_results_.reserve(modules_.size());   // L106：堆分配，不在 try 内

for (auto& registered : modules_) {
    StepResult step_result{};
    ...
    try {
        step_result.result = registered.module->initialize(context_);   // L115：仅此处在 try 内
        ...
    } catch (const std::exception& exception) {
        ...
    } catch (...) {
        ...
    }

    registered.started = step_result.result.success;
    step_results_.push_back(step_result);   // L131：堆分配，不在 try 内
    ...
}
```

**影响**

- L106 `reserve()` 与 L131 `push_back()` 均可抛 `std::bad_alloc` / `std::length_error`。
- 这两处异常会直接逃出 `start()`。此时 `state_` 已被置为 `Starting`（L104）且**不会被回滚**，已成功初始化的模块也**不会被释放**（`failStart()` 未被调用）。
- 结果是协调器停留在 `Starting` 这一既非成功也非失败的状态，同时泄漏已初始化的模块资源，违反 `ATOM-01_Runtime.md` 安全约束「启动失败须提供模块名和错误原因」与验收条件第 3 条。

**补充**：`start()` 自身未声明 `noexcept`，异常外泄在语言层面合法，因此这是一个"状态不可恢复"缺陷而非"程序终止"缺陷，但危害等价。

**修复方向（仅记录，不在本轮实施）**

把分配到结果落盘的整段流程纳入统一 `try/catch`；或在构造阶段完成全部分配，使 `start()` 运行期零分配；或将 `start()` 声明为 `noexcept` 并把异常统一转换为 `InitFailed` 结果。

---

### ATOM01-BUG-005 `ModuleRegistration::required` 无默认值，漏填时行为未定义且可产生安全误判

**严重度**：中 → **已修复（2026-09-27，v0.3）**

> **修复实施**：`ModuleRegistration` 补默认值——`RuntimeStep step{RuntimeStep::CreateDiagnostics};` 与 `bool required{true};`。`required` 默认取"必需"（更安全的一侧），消除漏填时的未定义行为与"必需模块失败被静默降级"。以下保留原始记录供追溯。

**现象**

```cpp
// Inc/RuntimeCoordinator.hpp L73-77
struct ModuleRegistration {
    RuntimeStep step;
    bool required;
    std::unique_ptr<ILifecycleModule> module;
};
```

`required` 与 `step` 均为**未初始化**的标量成员（对比同文件 L45–48 的 `ModuleInitResult`、L51–54 的 `StepResult` 都写了 `{false}` / `{true}` 默认值）。

调用方若使用聚合初始化且漏写 `required`，该成员将保持不确定值：

```cpp
// Src/RuntimeCoordinator.cpp L81-86
RegisteredModule candidate{
    registration.step,
    registration.required,   // 未定义值被直接采纳
    std::move(registration.module),
    false,
};
```

**影响**

- 若不确定值恰为 `false`，则一个**必需模块初始化失败时不会被判为失败**，循环继续（L133），系统最终进入 `Standby`（L139）—— 这违反 `ATOM-01_Runtime.md` 验收条件第 3 条「任一必需项失败时进入 INIT_FAILED，且发射请求被拒绝」。
- 属于"静默安全降级"，无法通过日志或返回值察觉。

**修复方向（仅记录，不在本轮实施）**

为 `required` 提供默认值（必需模块是更安全的默认），或在 `addModule()` 中校验并拒绝含不确定语义的注册。

---

### ATOM01-BUG-006 `RuntimeContext` 以字符串字典占位，无配置结构、无必需项校验、无 PID 加载

> **处置（2026-09-27，v0.4）：不实施。** 按要求不允许越权实现或跨 Atom 实现：有类型的配置数据模型、必需项校验与 PID 加载属 ATOM-15_Config_Persistence 职责，并需 `Atoms/Public` 契约决策。本模块不新建配置模型、不引入跨 Atom 依赖。以下保留原始记录供追溯。

**严重度**：中（当前表现为"设计缺口成立、实现尚未落地"）

**现象**

```cpp
// Inc/RuntimeCoordinator.hpp L37-42
struct RuntimeContext {
    // The coordinator deliberately does not parse configuration. Adapters may
    // use this path and the opaque values supplied by the owning application.
    std::string configuration_path;
    std::unordered_map<std::string, std::string> values;
};
```

**证据（设计要求）**

`Design/Atomic/ATOM-01_Runtime.md` 内部原子操作：

- 第 2 条：加载并解析 JSON 配置文档。
- 第 3 条：校验必需配置项；可选项按 Config 默认规则处理。
- 第 4 条：加载电机 PID 参数。

`Design/Config.md` §17 第 1 条：「本表是所有硬件通道、报文、数值参数、运行参数和配置型策略的**唯一来源**」；第 8 条：「实现不得创建与本表冲突的隐含默认值」。

**影响**

- 当前设计把配置解析完全外推给适配器，而每个适配器只能拿到 `unordered_map<std::string,std::string>`（全字符串、无类型、无范围、无单位）。19 个模块将各自解析同一份 JSON，**"Configuration.md 是唯一来源"这一约束在类型层面失去保障**，只靠纪律维持。
- L3–4 的注释「The coordinator deliberately does not parse configuration」与 `ATOM-01_Runtime.md` 第 2、3 条存在直接张力：若协调器不解析，第 2、3 条由谁承担、以什么接口承担，尚无约定。
- 第 4 条 PID 加载在 `RuntimeCoordinator.cpp` 中**完全没有实现**（`RuntimeStep::LoadPidParameters` 仅是一个步骤名）。

**说明**：`PLAN.md` §4「阶段 B」承认配置加载、解析与必需项校验待做，因此本条部分属于**已知未完成**。但其"以无类型字符串字典作为跨模块配置契约"的部分是**设计层面的缺陷**，不因时间推移自动消失，故仍予登记。

**修复方向（仅记录，不在本轮实施）**

在 `Atoms/Public/` 定义有类型、有范围、有默认值的配置数据模型与只读提供者接口，由配置模块（ATOM-15）实现、由本协调器在启动早期校验必需项。

---

### ATOM01-BUG-007 模块间共享契约（`ILifecycleModule` 等）未落入 `Atoms/Public`

**严重度**：中 → **已修复（2026-09-27）**

> **状态更新**：本条已实施上提改造，见 §5。以下保留原始记录供追溯。

**现象**

以下类型目前全部定义在 `Inc/RuntimeCoordinator.hpp` 内：

| 类型 | 行号 | 谁需要 |
|---|---|---|
| `ILifecycleModule` | L64–71 | `ATOM-01_Runtime.md` 内部原子操作第 5–11 条列举的**全部**下游模块（诊断、配置、PID、CAN、电机/编码器、GPIO、IIC/PCA9685A、舵机、SBUS/视觉、HTTP/WebSocket） |
| `RuntimeContext` | L37–42 | 同上 |
| `ModuleInitResult` | L44–48 | 同上 |
| `StepResult` | L50–55 | 诊断/状态查询接口 |
| `RuntimeState` / `RuntimeStep` | L11–32 | 状态查询接口 |

**影响**

- 其余 18 个 ATOM 若要接入启动流程，必须 `#include` 本模块的 `Inc/RuntimeCoordinator.hpp`，形成"每个模块横向依赖 ATOM-01"的耦合，而 `PLAN.md` §4 阶段 A 明确目标是「设计模块适配接口，避免在 ATOM-01 中耦合具体硬件实现」。
- 反之，若各模块在 `Public/` 自造同名接口，则违反 `ATOM-02_PLAN.md` §3.1 已确立的纪律「禁止各 ATOM 重复定义同名枚举/类型」。
- `ILifecycleModule` 是所有 19 个模块的**唯一公共接入点**，其缺席会导致其余模块无法开工。

**修复方向（已实施，见 §5）**

`ILifecycleModule`、`RuntimeContext`、`ModuleInitResult`、`StepResult` 已提升至 `Atoms/Public/ILifecycle.hpp`。

补充说明：`RuntimeState` / `RuntimeStep` 因是 `StepResult` 与 `RuntimeStartResult` 的成员类型，一并上提（否则 `StepResult` 无法独立成立）。`RuntimeCoordinator` 类本体仍属 ATOM-01 私有。

**仍待处理（不在本轮范围）**

- `ATOM01-BUG-001`：上提只解决了"类型归属"，**没有**解决"启动结果不驱动状态机"。翻译点已由 `Atoms/Public/AppCoordinator` 承接，但 `RuntimeState` 与 `SystemMode` 双轨并存的根因仍在。
- `ATOM01-BUG-002`：已按授权移除 `canExecuteActuatorCommands()`，见 §5.6。

---

### ATOM01-BUG-008 无结构化步骤事件，不满足"每次状态迁移须可查询且有原因"

**严重度**：低 → **契约已定义（2026-09-27，v0.4）；代码未实施**

> **处置**：公共契约由 ATOM-01 定义，落于 `Design/LifecycleEvents.md`（事件字段、事件类型、等级下限、生产/消费边界；`ILifecycleEventSink` 置于 `Atoms/Public/`，由 ATOM-18 实现、AppCoordinator 注入）。按裁决不允许越权/跨 Atom 实现，故 ATOM-01 内的事件产出代码**尚未实施**，需另行授权。以下保留原始记录供追溯。

**现象**

启动过程只产出 `std::vector<StepResult>` 终态快照：

```cpp
// Inc/RuntimeCoordinator.hpp L57-62
struct RuntimeStartResult {
    RuntimeState state{RuntimeState::Created};
    std::vector<StepResult> steps;
    bool succeeded() const noexcept;
};
```

对比 `ATOM-02` 已实现的 `StateEvent`（含 `kind`/`reason`/`timestamp_us`/`sequence`/`run_id`）与 `Interface.md` §19 `LogEvent`（8 字段）。

**证据**

- `Design/Interface.md` §7.2 第 6 条：「状态迁移必须产生可查询的状态事件和原因」。
- `Design/Config.md` §16.2：「模块初始化结果」必须记录；§16.3 规定日志字段含 `timestamp`、`level`、`module`、`event`、`source`、`state`、`request_id`、`error_code`、`payload`。

**影响**

- 无时间戳，无法回答"哪个模块在何时初始化失败"。
- 无事件序号/locale，无法与 `ATOM-18`（诊断与日志）的事件流按序对齐。
- 无 `level`，无法区分"可选模块失败（可继续）"与"必需模块失败（致命）"的日志等级。

**修复方向（仅记录，不在本轮实施）**

在启动推进点产出结构化事件并交由诊断模块消费，而非只保留终态向量。

---

### ATOM01-BUG-009 每步骤仅允许注册一个模块，与设计文档按"能力"而非"步骤"列举模块的口径存在张力

**严重度**：低 → **已修复（2026-09-27，v0.4）**

> **修复实施**：裁决为"1 步骤 : N 模块"。`addModule()` 不再拒绝重复 `RuntimeStep`，同一步骤内保持注册先后顺序（FIFO），不同步骤仍按 `RuntimeStep` 升序执行；失败原因通过新增的 `lastRegistrationError()` / `ModuleRegistrationError` 查询。改动仅限 ATOM-01 私有 API，无跨 Atom 依赖。以下保留原始记录供追溯。

**现象**

```cpp
// Src/RuntimeCoordinator.cpp L75-79
for (const auto& existing : modules_) {
    if (existing.step == registration.step) {
        return false;   // 同一步骤第二次注册被拒绝
    }
}
```

而 `ATOM-01_Runtime.md` 的步骤 5–11 是**能力域**描述，例如第 6 条「逐个初始化电机和编码器通道」、第 9 条「初始化舵机输出状态」——在 `ATOM-05`、`ATOM-07`、`ATOM-08`、`ATOM-10`、`ATOM-11` 各自独立成模块的现状下，这些能力域天然需要**多个模块**参与同一步骤。

**影响**

- 若严格按"一个 `RuntimeStep` 一个模块"实现，则必须把多个 ATOM 合并进一个适配器，ADR 未讨论；若放开，则会丢失步骤内的先后顺序保证。
- 当前 `return false` 的拒绝结果在调用点未被检查（`RuntimeCoordinator` 不记录也未上报 addModule 失败），可能造成模块**静默未注册**。

**修复方向（仅记录，不在本轮实施）**

明确"步骤"与"模块"是多对一还是一对一，并在 `addModule()` 失败时提供可查询原因。

---

## 3. 与 `PLAN.md` 已声明未完成项的对应关系

为避免与 `PLAN.md` 中"待确认/阶段 D"重复计数，对应关系如下：

| 本清单条目 | `PLAN.md` 状态 | 是否属于已知未完成 |
|---|---|---|
| ATOM01-BUG-006（配置解析、必需项校验、PID 加载缺失） | §4 阶段 B、§6 待确认 | 部分是（无类型字符串契约部分不是） |
| ATOM01-BUG-008（结构化事件） | §5 未提及 | 否 |
| ATOM01-BUG-007（Public 共享契约） | §5 未提及 | 否 |
| ATOM01-BUG-001、002、003、004、005、009 | §5 声明"已完成第一阶段和第二阶段基础实现" | **否，均为已实现代码中的缺陷** |

---

## 5. 改造与修复记录

> §5.1～§5.5 为 `ATOM01-BUG-007` 的共享契约上提改造记录；§5.6 为 `ATOM01-BUG-002/003/004/005` 的缺陷修复记录。

### 5.1 变更内容

经用户授权，执行共享契约上提。**仅搬移类型定义，未改动任何一行逻辑。**

| 文件 | 变更 |
|---|---|
| `Atoms/Public/ILifecycle.hpp` | **新建**。`RuntimeState`、`RuntimeStep`、`toString(RuntimeState)`、`toString(RuntimeStep)`、`RuntimeContext`、`ModuleInitResult`、`StepResult`、`ILifecycleModule` |
| `Atoms/ATOM-01_Runtime/Inc/RuntimeCoordinator.hpp` | 移除上述定义，新增 `#include "ILifecycle.hpp"`；`RuntimeStartResult`、`ModuleRegistration`、`RuntimeCoordinator` 保留原地 |
| `Atoms/ATOM-01_Runtime/Src/RuntimeCoordinator.cpp` | 移除两个 `toString` 实现（已随类型上提为 `inline`）；其余逻辑未动 |
| `Atoms/Public/CMakeLists.txt` | **新建**。`Marshland::PublicHeaders`（叶子契约目标）、`Marshland::Public`（聚合） |
| `Atoms/Public/AppCoordinator/` | **新建**。顶层装配与桥接层，见 §5.3 |
| `NewVersion/CMakeLists.txt` | `add_subdirectory(Atoms)` 提前至可执行文件之前，新增 `add_subdirectory(Atoms/Public)`，`marshland` 改为链接 `marshland_public` |

### 5.2 依赖方向变化

改造前：`ATOM-01 <- 其余 18 个 ATOM`（横向耦合）。

改造后：

```text
marshland (Entry.cpp)
  └── marshland_public
        ├── marshland_public_headers   （叶子：MarshlandTypes.hpp + ILifecycle.hpp）
        ├── marshland_atoms            （19 个 ATOM）
        └── marshland_app              （AppCoordinator 桥接层）
              ├── atom_01_runtime ──┐
              ├── atom_02_state_arbitration ──┤
              └── public_headers ─────────────┴──> Atom -> Public，无环
```

其余 18 个 ATOM 此后只需 `#include "ILifecycle.hpp"`，不再需要包含 ATOM-01 的私有头。

### 5.3 桥接层的位置与边界

`Atoms/Public/AppCoordinator/` 是工程中**唯一**允许同时包含 `RuntimeCoordinator.hpp` 与 `StateArbitration.hpp` 的地方，承担三项职责：

1. 启动结果 → `SystemMode` 的**唯一**映射点（`UNINITIALIZED → INITIALIZING → STANDBY / INIT_FAILED`）。
2. `ExternalInputs` 的**唯一**写入点（字段级上报 + 合并后提交），用于规避 `ATOM02-BUG-005` 的整体替换缺陷。
3. 运行闸门 `RunGate()` = 硬件就绪 AND 业务模式可操作（区分 `HARDWARE_NOT_READY` 与 `MODE_NOT_OPERATIONAL`）。

**边界**：桥接层不注册模块、不仲裁控制权、不判定发射授权、不编排急停/故障恢复、不推进四发流程。凡上述功能均属对应 Atom，禁止在桥接层实现，否则会形成与状态机并存的"第二真相"。

**对 ATOM-01 的接口影响**：无。桥接层通过既有公开接口 `start()` / `shutdown()` / `stepResults()` / `succeeded()` 消费本模块，未要求本模块新增任何 API。

### 5.4 本轮新增发现

| 编号 | 标题 | 严重度 | 说明 |
|---|---|---|---|
| ATOM01-BUG-010 | 启动协调器无法向状态机通知初始化开始，`INITIALIZING` 只能由外部代劳 | 低 | `ATOM-01_Runtime.md` 内部原子操作第 13、14 条只覆盖终态（`STANDBY` / `INIT_FAILED`），未定义"开始初始化"的通知点。当前由 `AppCoordinator` 在调用 `start()` 前代为推进 `SystemMode`，属可行的规避，但**状态的驱动源被拆在两处**。建议在 `RuntimeCoordinator::start()` 内补一次开始通知回调，或明确"启动前状态迁移由装配层负责"并写入 `PLAN.md`。 |

### 5.5 构建验证（已执行）

按 `AGENTS.md` 要求使用 WSL aarch64 交叉编译，**增量编译**（未删除构建目录、未使用 `--clean-first`）：

```bash
cmake --build cmake-build-ubuntu-arm --target marshland -j
```

结果：

- `atom_01_runtime` 重新编译并链接（上提改造后首次编译，确认 `ILifecycle.hpp` 解析正确）。
- `marshland_app` 编译并链接成功（桥接层首次编译）。
- `marshland` 链接成功。
- 产物架构：`ELF 64-bit LSB pie executable, ARM aarch64` —— 符合目标平台要求。
- 在 `-Wall -Wextra -Wpedantic` 下**零警告**，满足 `PLAN.md` §6.3「警告视为需清零」。

---

### 5.6 缺陷修复记录（ATOM01-BUG-002 / 003 / 004 / 005）

经用户逐项确认后实施，只改动 `Inc/RuntimeCoordinator.hpp` 与 `Src/RuntimeCoordinator.cpp`。

| 编号 | 修复方式 |
|---|---|
| ATOM01-BUG-002 | 按授权**移除** `RuntimeCoordinator::canExecuteActuatorCommands()`。移除前全仓库检索确认其无其他调用点，对应职责已由 `Atoms/Public/AppCoordinator::RunGate()` / `CanRunActuators()` 承担；本模块此后只暴露硬件生命周期事实（`state()` / `stepResults()`）。 |
| ATOM01-BUG-003 | 采纳"允许重启"：`start()` 在 `Stopped` 状态下回到 `Created`、清空 `step_results_` 并复位各模块 `started` 标志后重新执行启动序列。`InitFailed` 不直接重启（`Design/Interface.md` §7.1 无 `INIT_FAILED -> INITIALIZING` 边），必须先 `shutdown()`。 |
| ATOM01-BUG-004 | 整个启动序列纳入统一 `try/catch(...)`；异常时 `failStart()` 回滚为 `InitFailed` 并释放已启动模块。新增私有 `currentResult()` 兜底构造返回值。`push_back` 改为 `std::move`。`start()` 声明为 `noexcept`，与 `AppCoordinator::Start` 的 `noexcept` 契约一致。 |
| ATOM01-BUG-005 | `ModuleRegistration` 补默认值：`RuntimeStep step{RuntimeStep::CreateDiagnostics};`、`bool required{true};`。 |

**同一裁决**：`ATOM01-BUG-010` 采纳其第二条建议——"启动前状态迁移由装配层负责"，已写入 `PLAN.md` §5.1；不在 `start()` 内新增回调，保持状态驱动源唯一。

**仍未实施**：`ATOM01-BUG-001`（与 `AppCoordinator` 的"唯一映射点"分层冲突）、`ATOM01-BUG-006`（归属 ATOM-15，需 `Public` 契约决策）、`ATOM01-BUG-008`（归属 ATOM-18，需 `Public` 契约决策）、`ATOM01-BUG-009`（需先裁决"步骤与模块"的一对多关系）。（以上四条已于 v0.4 裁决，见 §5.7。）

**构建验证**：按用户指示，本轮由用户在 WSL aarch64 环境执行增量编译。本 Agent 所在沙箱无法启动 WSL（`Wsl/Service/CreateInstance/E_ACCESSDENIED`），故未在本会话内编译。

---

### 5.7 缺陷处置记录（ATOM01-BUG-001 / 006 / 008 / 009）

经用户逐项裁决（2026-09-27，v0.4）：

| 编号 | 处置 |
|---|---|
| ATOM01-BUG-001 | **不修复**。`RuntimeState` 与 `SystemMode` 的分层是现行设计：ATOM-01 只表达硬件生命周期，业务模式映射唯一由 `Atoms/Public/AppCoordinator` 承担（`PLAN.md` §5.1）。 |
| ATOM01-BUG-006 | **不实施**。不允许越权实现或跨 Atom 实现：有类型配置模型与必需项校验属 ATOM-15 职责，并需 `Atoms/Public` 契约决策。 |
| ATOM01-BUG-008 | **契约已定义，代码未实施**。公共契约由 ATOM-01 定义并落于 `Design/LifecycleEvents.md`；按要求未实施 ATOM-01 内的事件产出代码。 |
| ATOM01-BUG-009 | **已修复**。裁决"1 步骤 : N 模块"：`addModule()` 允许同一步骤注册多个模块，步骤内保持注册先后顺序（FIFO）；新增 `lastRegistrationError()` 提供可查询失败原因。 |

**构建验证**：§5.6 修复后的源码已由审查方在 WSL aarch64 环境执行增量编译验证，
`-Wall -Wextra -Wpedantic` 下**零 error、零 warning**，产物为 `ARM aarch64` ELF。

---

### 5.8 重启能力的跨模块接口后果（2026-09-27 第二轮集成审查）

`ATOM01-BUG-003` 的修复使 `RuntimeCoordinator::start()` 在 `RuntimeState::Stopped`
下可以重新执行启动序列，即**硬件层支持运行中重启**。但该能力**没有对应的业务侧协议**：

`ATOM-02` 的 `kModeEdges` 中 `START` 边只从 `SystemMode::UNINITIALIZED` 出发，
而 `Shutdown()` 之后业务模式停留在 `STANDBY` / `MANUAL`。因此：

```
RuntimeCoordinator::start()  →  成功（硬件重新拉起）
StateArbitrator 侧           →  无合法迁移回到 INITIALIZING，业务侧并未"重新初始化"
```

**这不是 ATOM-01 内部缺陷**（本模块只表达硬件生命周期，业务模式映射由
`Atoms/Public/AppCoordinator` 承担，见 §5.7 对 `ATOM01-BUG-001` 的处置），
但它是本次修复引入的**能力不对称**。

#### 裁决结果（用户，2026-09-27）：**不支持运行中重启**

`ATOM-02` 保持现有边表不变（见 `Atoms/ATOM-02_State_Arbitration/BugLists.md`
**ATOM02-BUG-015**，该条已据此关闭）。由此产生一项**待 ATOM-01 处置的动作**：

> **建议收紧本模块的 `start()`**：移除或显式禁止 `RuntimeState::Stopped -> Created`
> 的重启路径，使硬件层与业务层语义一致。
>
> 理由：当前两侧分叉——`RuntimeCoordinator::start()` 声称可重启，而业务侧不可能
> 回到可初始化状态，任何调用方若以 `start()` 的返回值推定"已重新初始化"都会得到错误结论。
> 若本模块单独保留重启能力，该能力在整机语义中是**不可用**的，只会成为误用来源。
>
> 备选（若认为硬件层自测需要重启能力）：保留能力但在 `PLAN.md` 与头注释中明确
> "整机语义不支持运行中重启，本能力仅供底层自测"，并确保没有任何上层路径可达。

#### 支撑该裁决的设计依据

`Design/PEF.md` §8.3 第 3 条明确规定软件急停人工解除后「**不需要重新初始化**」。
若允许"运行中重启 = 重新初始化"，则与 `PEF.md` §8.3 直接冲突。因此"不支持运行中重启"
不仅是一个工程取舍，也是与现有流程文档一致的选择。

#### 已在本模块之外实施的规避

`Atoms/Public/AppCoordinator::Start()` 在 `SystemMode != UNINITIALIZED` 时返回
`kRestartNotSupported` 并报告当前模式，**永久拒绝**第二次初始化；不调用 `Reset()`，
以免清空诊断事件（`Interface.md` §7.2 第 6 条要求事件可查询）。

#### 构建验证

§5.6 修复后的源码已由审查方在 WSL aarch64 环境执行增量编译验证，
`-Wall -Wextra -Wpedantic` 下**零 error、零 warning**，产物为 `ARM aarch64` ELF。

---

## 6. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-09-27 | 首次登记。基于 `Inc/RuntimeCoordinator.hpp`、`Src/RuntimeCoordinator.cpp`、`PLAN.md` 的静态审查形成 9 条缺陷；本轮未修改任何 Atom 代码。 |
| 0.2 | 2026-09-27 | ATOM01-BUG-007 已修复：执行共享契约上提（新增 `Atoms/Public/ILifecycle.hpp`），`RuntimeCoordinator.hpp`/`.cpp` 改为包含该公共头，逻辑零改动。新增桥接层 `Atoms/Public/AppCoordinator`（记录于 §5.3）。新登记 ATOM01-BUG-010。aarch64 增量编译通过、零警告、产物架构已验证。 |
| 0.3 | 2026-09-27 | 修复 ATOM01-BUG-002（按授权移除 `canExecuteActuatorCommands()`）、003（`Stopped` 允许重启）、004（`start()` 全流程异常保护并声明 `noexcept`）、005（`ModuleRegistration` 补默认值）。裁决 ATOM01-BUG-010：启动前迁移归装配层，写入 `PLAN.md` §5.1。详见 §5.6。 |
| 0.4 | 2026-09-27 | 修复 ATOM01-BUG-009（1 步骤 : N 模块 + `lastRegistrationError()`）；新增公共契约文档 `Design/LifecycleEvents.md` 定义 ATOM01-BUG-008 的事件契约（代码侧未实施）；裁决 ATOM01-BUG-001 不修复、ATOM01-BUG-006 不实施。详见 §5.7。 |
| 0.5 | 2026-09-27 | 第二轮集成审查（`AppCoordinator` ↔ `StateArbitrator`）。新增 §5.8 登记"重启能力的跨模块接口后果"：本模块的 `Stopped → Created` 重启（ATOM01-BUG-003）在业务侧无对应协议，已由桥接层以 `kUninitializedRequired` 显式拒绝规避，重启语义待 ATOM-02 裁决（ATOM02-BUG-015）。§5.7 的构建验证状态更新为"已由审查方执行，零警告"。**本轮未修改 `Inc/`、`Src/`、`CMakeLists.txt` 任何一行。** |
| 0.6 | 2026-09-27 | 用户裁决：**不支持运行中重启**（ATOM02-BUG-015 已据此关闭）。§5.8 更新为裁决结果与**待本模块处置的动作**：建议收紧 `start()` 的 `Stopped -> Created` 重启路径，使硬件层与业务层语义一致；支撑依据为 `PEF.md` §8.3 第 3 条「急停人工解除后不需要重新初始化」。**本文件仅为建议登记，未修改本模块代码。** |
