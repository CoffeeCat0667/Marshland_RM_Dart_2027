# ATOM 接入规范（ATOM Integration Specification）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 文档名称 | ATOM 跨模块接入规范 |
| 适用对象 | `Atoms/ATOM-03_*` ～ `Atoms/ATOM-19_*` 的**全部**模块及其专属 Agent |
| 关联规则 | 根目录 `AGENTS.md`（工作行为约束）、各 `Atoms/ATOM-NN_*/AGENTS.md`（目录级规则） |
| 权威设计 | `Design/Interface.md`（模块边界与接口契约）、`Design/Config.md`（唯一配置来源）、`Design/Atoms.md`（19 个功能类索引） |
| 已实现基线 | `Atoms/ATOM-01_Runtime`（硬件生命周期）、`Atoms/ATOM-02_State_Arbitration`（业务状态机与控制权仲裁）、`Atoms/Public/`（共享契约 + `AppCoordinator` 桥接层） |
| 版本 | 1.0 |
| 编制日期 | 2026-09-27 |
| 性质 | **规范性规则文件**，不是设计文档的复述。凡与 `Design/` 冲突，以 `Design/` 为准并应同步修订本文件 |

本文件解决一个此前完全空白的问题：**19 份 `PLAN.md` 中没有任何一份说明如何接入已实现的 ATOM-01/02。** 若不统一约定，每个模块会各自发明接法，方向互不相同且不可组合。

### 0.1 规范性用语

- **必须**：不满足即视为接入失败。
- **禁止**：不得以任何方式绕过。
- **应**：默认必须满足；不满足必须在模块 `PLAN.md` 中记录理由。
- **可以**：允许但不是强制。

---

## 1. 依赖方向（不可协商）

```text
                    ┌─────────────────────────────────────────┐
                    │  Atoms/Public/         （共享契约层）    │
                    │  MarshlandTypes.hpp                      │
                    │  ILifecycle.hpp                          │
                    │  StateArbitrationTypes.hpp               │
                    └─────────────────────────────────────────┘
                                    ▲
                    ┌───────────────┴───────────────┐
                    │                               │
        ┌───────────┴──────────┐        ┌───────────┴──────────┐
        │ ATOM-01_Runtime      │        │ ATOM-02_State_       │
        │ ATOM-03 .. ATOM-19   │        │        Arbitration   │
        └──────────────────────┘        └──────────────────────┘
                    ▲
                    │
        ┌───────────┴──────────────────────────────────────────┐
        │ Atoms/Public/AppCoordinator/  （顶层装配与桥接层）      │
        │ 唯一允许同时依赖 ATOM-01 与 ATOM-02 的地方             │
        └──────────────────────────────────────────────────────┘
                    ▲
        ┌───────────┴──────────┐
        │ Entry.cpp            │  （进程入口 + 对象图组装）
        └──────────────────────┘
```

**硬性规则：**

1. **必须**保持依赖方向 `ATOM → Public`。
2. **禁止**任何 ATOM 反向包含 `AppCoordinator.hpp`。
   `AppCoordinator` 是顶层装配件，不是共享契约；它依赖 ATOM，被 ATOM 依赖会立即形成环。
3. **禁止** ATOM 之间横向包含对方的 `Inc/` 私有头（见 §6）。
4. **禁止**在 ATOM 内定义 `main()`、创建可执行文件、或设置架构/编译器/C++ 标准/编译选项。
5. ATOM 的 `CMakeLists.txt` **必须**提供目标 `atom_NN_<名称>` 与别名 `Marshland::atom_NN_<名称>`，且不定义 `project()`。

---

## 2. 代码结构：内核 + 适配器两层

每个 ATOM **应**按以下结构组织：

```text
Atoms/ATOM-NN_<名称>/
├── CMakeLists.txt
├── PLAN.md
├── Inc/atomNN_<名称>/
│   ├── <领域类型>.hpp          ┐
│   ├── <核心组件>.hpp          │ ① 纯逻辑内核
│   └── ...                     ┘
├── Src/
│   ├── <核心组件>.cpp          ┐ ① 纯逻辑内核实现
│   └── ...                     ┘
└── Inc/atomNN_<名称>/lifecycle_adapter.hpp  ┐ ② 生命周期适配器（薄壳）
    Src/lifecycle_adapter.cpp                ┘
```

### 2.1 ① 纯逻辑内核

**必须**满足：

- 只依赖 `Atoms/Public/` 的契约头与 C++ 标准库；
- **禁止**包含任何 `ATOM-NN` 的头（包括 ATOM-01/02 的私有头 `RuntimeCoordinator.hpp` / `StateArbitration.hpp`）；
- 尽量与硬件解耦：真实设备访问放在可替换的后端接口之后，便于上层以测试替身验证。

**理由**：内核可独立验证、可替换实现，且修改它不会波及本模块之外。

### 2.2 ② 生命周期适配器

**必须**：

- 是唯一实现 `marshland::runtime::ILifecycleModule` 的类；
- 是独立翻译单元（独立 `.hpp`/`.cpp`），不与内核实现文件混写；
- 只做"初始化的编排与结果翻译"，**禁止**在其中编写业务逻辑；
- 目标规模 **≤ 50 行**；超过说明职责混入了本层。

**建议命名与形态**：

```cpp
// Inc/atomNN_<名称>/lifecycle_adapter.hpp
#pragma once

#include "ILifecycle.hpp"          // Atoms/Public
#include "<本模块内核头>.hpp"

namespace marshland::atomNN {

class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    explicit LifecycleAdapter(Core& core) noexcept : core_(core) {}

    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atomNN-<名称>"; }

private:
    Core& core_;                   // 只持引用：内核寿命由装配层保证（见 §7）
};

} // namespace marshland::atomNN
```

```cpp
// Src/lifecycle_adapter.cpp
runtime::ModuleInitResult LifecycleAdapter::initialize(const runtime::RuntimeContext& context)
{
    runtime::ModuleInitResult result{};
    result.module_name = name();

    // 只读取本模块需要的配置项。禁止自行解析 JSON、禁止臆造未配置的值。
    const auto it = context.values.find("<本模块配置键>");
    if (it == context.values.end()) {
        result.success = false;
        result.error_message = "missing required config key: <本模块配置键>";
        return result;                       // 必需项缺失必须如实失败
    }

    if (!core_.start(it->second)) {
        result.success = false;
        result.error_message = "<具体失败原因，足以定位到设备或调用>";
        return result;
    }

    result.success = true;
    return result;
}

void LifecycleAdapter::shutdown() noexcept
{
    core_.stop();                            // 必须 noexcept 且幂等
}
```

### 2.3 适配器必须遵守的六条

| # | 约束 | 依据 |
|---|---|---|
| 1 | `initialize()` **必须**如实反映成败；**禁止**把失败转成成功 | `Design/Atomic/ATOM-01_Runtime.md` 验收条件 |
| 2 | `error_message` **必须**足以定位到具体设备或调用 | 同上，安全约束 |
| 3 | `shutdown()` **必须** `noexcept`、**必须**幂等，且对"从未成功 `initialize()`"的调用安全 | `RuntimeCoordinator` 只对 `started == true` 的模块调用，但失败回滚路径时序不同 |
| 4 | 配置**只读** `RuntimeContext`；**禁止**自解析 JSON、**禁止**臆造 `[待定]` 值或与 `Config.md` 冲突的隐含默认值 | `Design/Config.md` §17 第 4、8 条 |
| 5 | **禁止**在适配器内判断机构流程、控制权或发射授权 | 属 ATOM-02 / ATOM-13 |
| 6 | **禁止**在适配器内创建线程；`initialize()` 不得长时间阻塞 | `Config.md` §4 控制周期约束 |

---

## 3. `RuntimeStep` ↔ ATOM 映射

`RuntimeStep` 是 ATOM-01 的私有枚举，**ATOM-01 之外的模块无法扩展它**。`RuntimeCoordinator` 的语义是：

- 不同步骤按 `RuntimeStep` 声明序执行；
- **同一步骤内允许注册多个模块**，按注册先后顺序执行（FIFO）；
- 一个模块只能属于一个步骤。

当前映射如下。`✅` 表示有直接对应的步骤；`⚠️` 表示语义勉强；`❌` 表示**无对应步骤**（见 §3.1）。

| RuntimeStep | 应注册的 ATOM | 状态 |
|---|---|---|
| `CreateDiagnostics` | ATOM-18（诊断与日志 sink 的构造） | ⚠️ 名字对得上，语义是"创建诊断资源" |
| `LoadConfiguration` | ATOM-15（配置加载与校验） | ⚠️ 同上，ATOM-01 自身不解析配置 |
| `ValidateConfiguration` | —（归属 ATOM-15 内部步骤） | — |
| `LoadPidParameters` | ATOM-05（PID 参数加载） | ✅ |
| `InitializeSocketCan` | ATOM-04 | ✅ |
| `InitializeMotorAndEncoderChannels` | ATOM-05 | ✅ |
| `InitializeGpioAndLimitInputs` | ATOM-06 | ✅ |
| `InitializeIicPca9685` | ATOM-07 | ✅ |
| `InitializeServoOutputs` | ATOM-10（换弹/供弹舵机）、ATOM-11（发射舵机） | ✅ 一步骤 N 模块 |
| `InitializeSbusAndVision` | ATOM-03（SBUS）、ATOM-12（视觉/YAW） | ⚠️ 名称把两个独立设备域捆在一起 |
| `StartHttpAndWebSocket` | ATOM-16、ATOM-17 | ✅ |
| — | **ATOM-08 蓄力、ATOM-09 变力、ATOM-13 PEF、ATOM-14 安全/恢复、ATOM-19 调试/校准** | ❌ 无对应步骤 |

### 3.1 无对应步骤的模块怎么办

分两类处理：

**(a) 纯逻辑模块 —— 不实现 `ILifecycleModule`**

若模块不持有设备、不启动线程、不需要"初始化/释放"这对语义，则**不应**强行注册为生命周期模块，
而应由装配层直接构造并注入到需要它的模块。适用：`ATOM-13`（PEF 流程执行器，纯编排）、
`ATOM-19`（调试/校准，按请求驱动）。

**(b) 持有设备或线程的模块 —— 需要 ATOM-01 扩展 `RuntimeStep`**

`ATOM-08`、`ATOM-09` 需要启动电机控制线程，`ATOM-14` 需要持有安全相关资源。
在 `RuntimeStep` 扩展之前，这些模块的接入被阻塞。

**已登记的待办**：见 `Atoms/ATOM-01_Runtime/BugLists.md`。建议新增：

```cpp
InitializeChargeAndForce,     // ATOM-08 / ATOM-09
InitializeSafetyServices,     // ATOM-14
```

**过渡期做法（在扩展完成前）**：`ATOM-08`/`ATOM-09` 的电机**设备**初始化可由
`ATOM-05`（`InitializeMotorAndEncoderChannels`）承担；`ATOM-08`/`ATOM-09` 只保留**动作逻辑**，
由 PEFExecutor 驱动，不注册为生命周期模块。此过渡必须在模块 `PLAN.md` 中显式记录。

---

## 4. 模块对外的三个契约面

每个模块**必须**明确自己在每个契约面上的角色。

### 4.1 生命周期面 —— 实现 `ILifecycleModule`

见 §2、§3。仅用于"初始化/释放"。

### 4.2 输入上报面 —— 只经 `AppCoordinator::ReportXxx`

**这是强制要求，不是建议。**

`StateArbitrator` 虽然暴露了字段级 setter（`SetSbusStatus` / `SetWebOnline` / `SetYawArrived` /
`SetStopFire` / `SetFault` / `SetSoftwareEstopInput` / `SetClock`），但：

1. **禁止**任何 ATOM 直接调用这些 setter；
2. **禁止**任何 ATOM 调用 `StateArbitrator::UpdateInputs()`；
3. **必须**通过装配层注入的 `AppCoordinator&` 的对应 `ReportXxx()` 上报。

**理由**：`AppCoordinator` 是外部输入的**唯一写入点**。存在两条独立写入路径时，一次整体替换式
提交会清零其他来源写入的字段（历史缺陷 `ATOM02-BUG-005`）。该风险已由"单一写入点"消除，
任何绕过都会重新引入。

| 上报内容 | 应调用的接口 |
|---|---|
| SBUS 在线与自动模式标志 | `ReportSbus(bool online, bool auto_mode)` |
| WEB 客户端连接状态 | `ReportWebOnline(bool)` |
| YAW 到位 bool | `ReportYawArrived(bool)` |
| 停止发射标志 | `ReportStopFire(bool)` |
| 故障状态与等级 | `ReportFault(bool active, FaultSeverity)` |
| 软件急停事实 | `ReportSoftwareEstop(bool)` |
| 单调时钟刷新 | `ReportClock()`（每轮上报后调用一次） |

**关于软件急停**：`ReportSoftwareEstop(true)` 只**置位并锁存**，`false` **不可能**解除锁存。
解除的唯一路径是 `ConfirmSoftwareEstop()` → 解除动作 → `ReleaseSoftwareEstopLatch()`，
由 `ATOM-14` 编排（见 `Atoms/ATOM-14_Safety_Recovery/Guide.md`）。

**关于闸门**：`AppCoordinator::RunGate()` 是**普通执行器动作**的闸门，返回
`ALLOWED` / `HARDWARE_NOT_READY` / `ESTOP_LATCHED` / `FAULT_ACTIVE` /
`MODE_NOT_OPERATIONAL` / `NOT_INITIALIZED`。

> **重要边界**：各模块**不应**把安全寄托于"调用方一定会查 `RunGate()`"。
> 尤其在急停锁定期间，模块自身**必须**拒绝普通命令；`RunGate()` 只是第二道防线。

### 4.3 仲裁面 —— 提交意图前先仲裁

需要状态机裁决的动作（自动启动/停止、发射、停止发射标志清除、安全动作），
**必须**按 `SubmitIntent(const ControlIntent&)` 提交并检查 `IntentDecision::accepted`。

**禁止**把 `accepted == false` 当作成功。

#### 4.3.1 安全动作必须按类别使用 `IntentKind::SAFETY` / `SAFETY_RECOVERY`

`IntentKind` 的两个安全类别**不可混用**，区分标准是**该动作是否让执行器出力**：

| 动作 | 意图类别 | 放行阶段 |
|---|---|---|
| 零力矩帧发送 | `SAFETY`（不出力） | 急停锁定阶段**即可** |
| 换弹机构抬起 | `SAFETY_RECOVERY`（出力） | **仅人工确认之后**（`SystemMode != SOFTWARE_ESTOP`） |
| 人工确认解除后的蓄力双电机回零 | `SAFETY_RECOVERY`（出力） | 同上 |
| 一般故障恢复中的回零、三次发射、换弹抬起 | `SAFETY_RECOVERY`（出力） | `RECOVERY` / `FAULT` 期间 |
| 普通机构动作 | `CONTROL` | 受模式与故障闸门约束 |
| 发射 | `FIRE` | 优先级先由 `SubmitIntent` 判定 |
| 停止发射标志置位 / 清除 | `STOP_FIRE_SET` / `STOP_FIRE_CLEAR` | 见 `Config.md` §11.4 模式限制 |

`IntentKind::SAFETY` 在 `EvaluateIntent` 中位于**急停 / 故障 / 模式闸门之前**无条件放行；
`IntentKind::SAFETY_RECOVERY` 额外要求在 `SystemMode != SOFTWARE_ESTOP` 时才放行，
否则返回 `RejectReason::SOFTWARE_ESTOP`。

**两类误用都会导致系统不可恢复**：

- 用 `CONTROL` 提交任何安全动作 → 被 `NOT_READY` / `SOFTWARE_ESTOP` / `FAULT_ACTIVE` 拒绝，
  后果是**急停无法解除、故障恢复无法执行**。
- 在急停锁定阶段用 `SAFETY_RECOVERY` 提交换弹抬起 → 被 `SOFTWARE_ESTOP` 拒绝。
  锁定阶段**只允许不出力行为**（`PEF.md` §8.2）；换弹抬起必须在人工确认之后进行。

依据：`Design/Config.md` §15「安全动作例外」、`Design/Interface.md` §17.3 第 7 条 / §18.1、
`Design/PEF.md` §8.2 与 §8.3。

#### 4.3.2 发射授权：先仲裁，再复核

分两步，**顺序不可颠倒**：

1. **优先级仲裁** —— **必须**先 `SubmitIntent(ControlIntent{ kind = FIRE, source = ... })`，
   并检查 `IntentDecision::accepted`。优先级只在 `EvaluateIntent` 的 `FIRE` 分支判定。
2. **前置条件复核** —— 再调用 `EvaluateFireAuthorization` / `AuthorizeFire` /
   `AuthorizeManualFire` 复核急停 / 故障 / 停止发射 / YAW 到位。

**注意 `EvaluateFireAuthorization` 现有一个显式 `control_holder` 入参**（`ATOM02-BUG-002` 修复）：

```cpp
FireAuthorization EvaluateFireAuthorization(const ArbitrationContext& ctx,
                                            ControlSource requesting_source,
                                            ControlSource control_holder) noexcept;
```

`control_holder` 是调用方**显式**给出的"当前拥有控制权的来源"：

- `NONE`（无人持有）→ `RejectReason::NO_CONTROL_AUTHORITY`；
- 请求源优先级**低于**持有者 → `LOWER_PRIORITY_CONFLICT`
  （自动流程在 WEB/SBUS 持有控制权时**不得越权发射**）；
- 同源或更高优先级（SBUS/WEB 介入自动流程）→ 通过。

另有一个以本模块控制源选择结果作为持有者的**便捷重载**（两参版本）。
调用方**应**根据自己掌握的实际情况选择重载，不要假设两者等价。

**禁止**把 `accepted == false` 或 `allowed == false` 当作成功。

> 权威签名与语义以 `Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp` 与
> `Atoms/Public/StateArbitrationTypes.hpp` 的注释为准；本节只规定使用方式。

---

## 5. 配置使用

1. **必须**通过 `RuntimeContext::values` 读取本模块配置项，`configuration_path` 仅供参考。
2. **禁止**自行打开或解析 JSON 配置文件。
3. **禁止**为缺失项臆造默认值。`Config.md` 中标记 `[待定]` 的项**必须**保持未实现状态，
   在模块 `PLAN.md` 中登记为待接入点。
4. 具体通道号、角度、CAN ID、阈值、PID 数值**必须**以 `Design/Config.md` 为唯一来源。
5. 需要时序的参数**必须**使用单调时钟（见 §8），**禁止**使用墙上时钟。

> 已知约束：`RuntimeContext::values` 目前是无类型字符串字典，配置模型与必需项校验
> 归属 `ATOM-15`。在 ATOM-15 落地前，各模块**应**在本模块内做本模块字段的类型转换与
> 范围校验，并把校验失败作为 `initialize()` 的失败原因上报。

---

## 6. 模块间依赖

### 6.1 禁止横向包含

**禁止** `#include "ATOM-05_Motor_PID/Inc/..."` 这类跨模块私有头包含。
它会把两个模块的编译单元绑死，并使依赖图不可分析。

### 6.2 优先接口注入

当一个模块需要另一个模块的能力时：

| 方案 | 评价 |
|---|---|
| A. 直接链接目标并在代码中调用对方具体类型 | 简单，但形成强耦合，且需在 CMake 中声明 ATOM→ATOM 链接 |
| **B. 在本模块声明一个窄接口，由装配层注入对方实现**（**应**采用） | 依赖单向、可测试、替换实现不需改动调用方 |

**推荐形态**：调用方声明自己需要的**最小能力**，不引用对方类型。

```cpp
// 在调用方模块内（不包含对方任何头）
namespace marshland::atom08 {

class IMotorCommandSink {                 // 只声明"我需要什么"
public:
    virtual ~IMotorCommandSink() = default;
    virtual bool setVelocity(int motor_id, double rpm) noexcept = 0;
    virtual bool setZeroTorque(int motor_id) noexcept = 0;
};

} // namespace marshland::atom08
```

装配层负责把 `ATOM-05` 的实现适配到这个接口。若确需方案 A，**必须**在模块 `PLAN.md`
中记录理由与依赖方向。

### 6.3 顺序依赖不等于链接依赖

`RuntimeCoordinator` **只保证执行顺序**，**不建立模块间链接**。例如
"ATOM-05 必须先于 ATOM-12" 只是执行序，不代表 ATOM-12 可以调用 ATOM-05 的符号。

**已确认的真实顺序依赖**（供注册顺序参考）：

```text
ATOM-04 (CAN)    必须先于  ATOM-05 / ATOM-12   （电机与 YAW 走 CAN）
ATOM-07 (IIC)    必须先于  ATOM-10 / ATOM-11   （舵机走 PCA9685A）
ATOM-06 (GPIO)   必须先于  ATOM-08             （双限位回零判据）
```

---

## 7. 注册与所有权

注册**只能**在装配层（`Entry.cpp`）完成：

```cpp
auto core   = std::make_unique<marshland::atom04::CanAdapter>( /* 依赖 */ );
auto life   = std::make_unique<marshland::atom04::LifecycleAdapter>(*core);

runtime.addModule(runtime::ModuleRegistration{
    runtime::RuntimeStep::InitializeSocketCan,
    true,                              // required
    std::move(life)});
```

**硬性约束：**

1. `LifecycleAdapter` **只持有内核的引用**，因此**内核对象的寿命必须长于适配器**。
   装配层**必须**同时保管两者（如上例），否则悬垂。
2. `addModule()` 返回 `false` 时**必须**检查 `lastRegistrationError()`，
   **禁止**静默忽略注册失败。
3. `required` **必须**显式书写，**禁止**依赖默认值（`true`）。硬件与安全相关模块必须为
   `true`；仅真正可降级的服务模块可为 `false`。
4. **禁止**在模块内部自行注册自己；**禁止**模块之间互相注册。
5. 空注册表会被 `AppCoordinator::Start()` 拒绝（防静默假就绪），因此新增模块后
   必须确认注册确实生效。

---

## 8. 时间、周期与线程

1. **必须**使用单调时钟。`AppCoordinator` 提供 `IMonotonicClock` 契约，
   其唯一实现是 `Atoms/Public/AppCoordinator/Inc/SystemMonotonicClock.hpp`。
2. 若模块需要自己的周期任务，**应**复用同一 `IMonotonicClock` 实例，而不是各自
   直接调用 `steady_clock::now()`，以便上层统一注入与替换。
3. 周期**必须**遵守 `Design/Config.md` §4 的规定（系统最长控制周期 100 ms；
   电机闭环/PID/CAN/GPIO/SBUS/WebSocket 均为 20 ms）。
4. 线程调度为 Linux `SCHED_OTHER`，固定周期任务用单调时钟**绝对期限**并监测超期。
   这是**尽力调度，不构成硬实时保证** —— **禁止**假设回调一定准时到达。
5. **禁止**在网络或日志路径上同步阻塞控制与安全任务。
6. 线程的创建与回收**应**放在 `ILifecycleModule::initialize()` / `shutdown()` 内，
   使生命周期事实可被 `RuntimeCoordinator` 观测。

---

## 9. 构建与验证

1. **必须**采用增量编译；**禁止**删除构建目录或使用 `--clean-first`。
2. **禁止**使用 x86 编译链。只允许：
   - 在 WSL 内用 aarch64 交叉工具链编译；或
   - 上传到目标 Ubuntu ARM 主机后在其上编译。
3. 编译**必须**在 `-Wall -Wextra -Wpedantic` 下**零警告**。
4. 交叉编译命令（首次 configure 之后日常只需 `--build`）：

```bash
cmake --build cmake-build-ubuntu-arm --target marshland -j
```

5. 模块**不需要**独立编译，也**不得**创建测试用 `main()`；行为验证依赖
   `PLAN.md` 中的判定表与上层集成验证。
6. 顶层 `CMakeLists.txt` 自带 C++23 与 ARM 架构守卫；**禁止**在模块内重复设置标准或架构。

---

## 10. 编码前置自检清单

提交前逐项确认：

- [ ] `Inc/` 中没有任何 `ATOM-NN` 的头包含（只有 `Atoms/Public/` 与标准库）
- [ ] 未包含 `AppCoordinator.hpp`
- [ ] `ILifecycleModule` 由独立翻译单元实现，且该文件 ≤ 50 行、无业务逻辑
- [ ] `initialize()` 失败时如实返回 `success = false` 且 `error_message` 可定位
- [ ] `shutdown()` 为 `noexcept`、幂等、对未初始化状态安全
- [ ] 未自解析 JSON；缺失配置键如实失败；无臆造默认值、无 `[待定]` 项被填值
- [ ] 外部输入**全部**经 `AppCoordinator::ReportXxx` 上报，未直接调用任何 `SetXxx`
- [ ] 外部输入未调用 `StateArbitrator::UpdateInputs()`
- [ ] 安全动作按类别提交：零力矩用 `IntentKind::SAFETY`；换弹抬起、蓄力回零与一般故障恢复动作用 `IntentKind::SAFETY_RECOVERY`；普通动作用 `CONTROL`；发射用 `FIRE`
- [ ] 急停锁定阶段（`SystemMode == SOFTWARE_ESTOP`）**未**用 `SAFETY_RECOVERY` 提交任何出力动作
- [ ] `SubmitIntent` 的 `accepted == false` 未被当作成功
- [ ] 未把安全寄托于调用方查询 `RunGate()`；急停锁定期间自身拒绝普通命令
- [ ] 模块间依赖采用接口注入（或已在 `PLAN.md` 记录直接链接的理由）
- [ ] 时间使用单调时钟；周期符合 `Config.md` §4
- [ ] 模块 `CMakeLists.txt` 提供 `atom_NN_<名称>` + `Marshland::atom_NN_<名称>`，未设全局编译选项
- [ ] 无 `main()`、无独立可执行文件
- [ ] 注册点检查了 `addModule()` 返回值与 `lastRegistrationError()`
- [ ] 内核对象寿命长于其生命周期适配器
- [ ] aarch64 交叉编译通过，`-Wall -Wextra -Wpedantic` 零警告
- [ ] 本模块 `PLAN.md` 已更新：接入点、所需配置键、`[待定]` 项、顺序依赖

---

## 11. 尚未裁决的事项

以下事项影响多个模块，**在本文件更新前不得由单个模块自行决定**：

| # | 事项 | 影响 | 状态 |
|---|---|---|---|
| 1 | 是否扩展 `RuntimeStep`（新增 `InitializeChargeAndForce`、`InitializeSafetyServices`） | ATOM-08 / ATOM-09 / ATOM-14 能否注册为生命周期模块 | 待裁决（见 `ATOM-01_Runtime/BugLists.md`） |
| 2 | 模块间依赖最终采用"接口注入"还是"直接链接" | 全部模块的依赖图形态 | 本文件 §6.2 建议接口注入，待确认生效 |
| 3 | `RuntimeContext` 无类型字典的替代方案（配置模型归属 ATOM-15） | 全部模块的配置读取方式 | 待 ATOM-15 + `Public` 契约决策 |
| 4 | 结构化生命周期事件的落地（`ILifecycleEventSink`） | ATOM-18 与 ATOM-01 的事件流 | 契约已定义于 `Design/LifecycleEvents.md`，代码未实施 |
| 5 | 不上线运行中重启（产品已裁决不支持）；`ATOM-01_Runtime::start()` 的收紧 | 全部模块对重启的假设 | 待 ATOM-01 处置（见 `ATOM-01_Runtime/BugLists.md`） |
| 6 | **急停人工确认的调用归属**：`ConfirmSoftwareEstop()` 由谁触发、与 `ATOM-14` 如何交接顺序未定 | `ATOM-03`、`ATOM-14`、`ATOM-16` 三者的接口缝 | 待裁决（见 `ATOM-02_State_Arbitration/BugLists.md` ATOM02-BUG-004） |
| 7 | **本规范的可见性**：各 `Atoms/ATOM-NN/AGENTS.md` 均未把 `Rule/` 列入允许阅读范围，ATOM Agent 按各自规则不会读到本文件 | 全部需要接入的模块 | 待裁决（见 §13） |

---

## 12. 本规范的可见性（重要）

本文件位于 `Rule/`，符合根 `AGENTS.md` 第 35 行「其他规则文件统一放在 `./Rule` 内」。

**但各 `Atoms/ATOM-NN/AGENTS.md` 的"允许阅读例外"一律只有三项**：

```text
1. 上层 Public 文件夹（Atoms/Public）      —— 允许读取与写入
2. 对应的 Design/Atomic/ATOM-NN_*.md       —— 允许只读
3. 上层 NewVersion/CMakeLists.txt          —— 允许只读
```

且这些规则同时声明「只允许阅读本工作目录下的本 `AGENTS.md`」「禁止读取任何上层目录中的文件」。
**结论：按现有目录规则，ATOM Agent 读不到本文件。**

在可见性问题解决前，本规范的内容**必须**被同步到各 ATOM 可读的位置，否则等于未被采纳。
可选方案：

| 方案 | 做法 |
|---|---|
| A（推荐） | 在 19 份 `Atoms/ATOM-NN/AGENTS.md` 的例外清单中各加一行，允许只读 `Rule/AtomIntegration.md` |
| B | 把本文件复制/移动到 `Atoms/Public/`（所有 ATOM 对该目录已有读写权限） |
| C | 在根 `AGENTS.md` 中加指引 —— **不能解决问题**：子目录规则明确禁止读取上层目录文件 |

---

## 13. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-09-27 | 首次编制。依据已实现的 ATOM-01/02 与 `Atoms/Public` 契约，规定依赖方向、内核+适配器两层结构、`ILifecycleModule` 适配器契约、`RuntimeStep` 映射与缺口、输入上报唯一写入点、`IntentKind::SAFETY` 安全动作契约、配置使用、模块间依赖与接口注入、注册与所有权、时间与线程、构建验证与自检清单。 |
| 1.1 | 2026-09-27 | 同步 ATOM-02 修复轮后的实际接口：`IntentKind` 拆分为 `SAFETY`（不出力，锁定阶段即可）/ `SAFETY_RECOVERY`（出力，仅人工确认后），§4.3.1 与 §10 自检项随之改写；§4.3.2 补充 `EvaluateFireAuthorization` 的显式 `control_holder` 入参与 `NO_CONTROL_AUTHORITY`；§11 新增急停确认归属与本规范可见性两项待裁决；新增 §12 说明可见性问题。 |
