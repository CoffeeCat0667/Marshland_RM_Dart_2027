# ATOM 接入规范（ATOM Integration Specification）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 文档名称 | ATOM 跨模块接入规范 |
| 适用对象 | `Atoms/ATOM-03_*` ～ `Atoms/ATOM-19_*` 的**全部**模块及其专属 Agent |
| 关联规则 | 根目录 `AGENTS.md`（唯一工程级工作规则）；其他规则文件统一位于 `Rule/` |
| 权威设计 | `Design/Interface.md`（模块边界与接口契约）、`Design/Config.md`（唯一配置来源）、`Design/Atoms.md`（19 个功能类索引） |
| 键名规范 | `Rule/ConfigSchema.md`（`dart_control.json` 的键名、类型、单位与结构的唯一来源） |
| 已实现基线 | `Atoms/ATOM-01_Runtime`（硬件生命周期）、`Atoms/ATOM-02_State_Arbitration`（业务状态机与控制权仲裁）、`Atoms/Public/`（共享契约 + `AppCoordinator` 桥接层） |
| 版本 | 1.2 |
| 编制日期 | 2026-09-27 |
| 更新日期 | 2026-10-10 |
| 设计同步基线 | PRD 0.10、PEF 1.5、Interface 1.5、Config 0.8 |
| 性质 | **规范性规则文件**，不是设计文档的复述。凡与 `Design/` 冲突，以 `Design/` 为准并应同步修订本文件 |

本文件统一模块接入方向和调用边界，不能替代尚未落地的公共 API。当前只有生命周期及仲裁数据类型已落入 Public；输入上报、仲裁、安全请求和公共时钟端口仍需确认后实施。根目录 `PLAN.md` 记录启动阻断与同步方案，不能把本文件中的接入要求视为代码已实现，也不扩大根规则允许的网关/装配职责。

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

#include "IConfigView.hpp"         // Atoms/Public —— 配置查询契约
#include "ILifecycle.hpp"          // Atoms/Public
#include "<本模块内核头>.hpp"

namespace marshland::atomNN {

class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    LifecycleAdapter(Core& core, const config::IConfigView& config) noexcept
        : core_(core), config_(config) {}      // 两者均由装配层在构造期注入

    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atomNN-<名称>"; }

private:
    Core& core_;                        // 只持引用：内核寿命由装配层保证（见 §7）
    const config::IConfigView& config_; // 配置视图寿命同样由装配层保证
};

} // namespace marshland::atomNN
```

```cpp
// Src/lifecycle_adapter.cpp
runtime::ModuleInitResult LifecycleAdapter::initialize(const runtime::RuntimeContext& context)
{
    runtime::ModuleInitResult result{};
    result.module_name = name();

    // RuntimeContext 只提供"启动入口"信息；配置值一律经 IConfigView 查询，
    // 键名遵循 Rule/ConfigSchema.md。禁止自行解析 JSON、禁止臆造未配置的值。
    // context.configuration_path 由配置模块（ATOM-15）消费，本模块通常不需要它。
    (void)context;

    std::int64_t value = 0;
    const config::ConfigStatus status = config_.getInt("<本模块配置键>", value);
    if (status != config::ConfigStatus::OK) {
        result.success = false;
        result.error_message = "config '<本模块配置键>' unusable: ";
        result.error_message += config::ToString(status);   // 必须区分失败原因
        return result;                       // 必需项缺失/非法必须如实失败
    }

    if (!core_.start(value)) {
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
| 4 | 配置**只读** `IConfigView`（键名见 `Rule/ConfigSchema.md`）；`RuntimeContext` 只作启动入口，**禁止**自解析 JSON、**禁止**臆造 `[待定]` 值或与 `Config.md` 冲突的隐含默认值 | `Design/Config.md` §17 第 4、8 条；`Atoms/Public/IConfigView.hpp` |
| 5 | **禁止**在适配器内判断机构流程、控制权或发射授权 | 属 ATOM-02 / ATOM-13 |
| 6 | 适配器只委托内核/后端启动或停止工作线程，不内置线程业务循环；`initialize()` 不等待完整机构动作 | `Config.md` §4、§15；线程资源由生命周期边界管理 |

---

## 3. `RuntimeStep` ↔ ATOM 映射

`RuntimeStep` 已定义于公共契约 `Atoms/Public/ILifecycle.hpp`，其生命周期语义由 ATOM-01 负责；其他模块不得自行增加步骤。`RuntimeCoordinator` 的语义是：

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
| `InitializeServoOutputs` | ATOM-10（供弹舵机及机构初始状态）、ATOM-11（发射舵机） | ✅ 换弹双电机依赖 ATOM-05/04，不存在换弹舵机 |
| `InitializeSbusAndVision` | ATOM-03（SBUS）、ATOM-12（视觉/YAW） | ⚠️ 名称把两个独立设备域捆在一起 |
| `StartHttpAndWebSocket` | ATOM-16、ATOM-17 | ✅ |
| — | **ATOM-08 蓄力、ATOM-09 变力、ATOM-13 PEF、ATOM-14 安全/恢复、ATOM-19 调试/校准** | ❌ 无对应步骤 |

### 3.1 无对应步骤的模块怎么办

分两类处理：

**(a) 纯逻辑模块 —— 不实现 `ILifecycleModule`**

若模块不持有设备、不启动线程、不需要"初始化/释放"这对语义，则**不应**强行注册为生命周期模块，
而应由装配层直接构造并注入到需要它的模块。适用：`ATOM-13`（PEF 流程执行器，纯编排）、
`ATOM-19`（调试/校准，按请求驱动）。

**(b) 持有设备或线程的模块 —— 先确认资源归属和注册步骤**

ATOM-08/09 当前计划采用外部 update/tick，不自行创建线程；不能据此认定必须新增启动步骤。电机设备和闭环资源由 ATOM-05 管理时，机构逻辑可以由装配层构造并注入，但必须明确配置就绪、初始状态、调用方和对象寿命。

ATOM-14 等确需独立资源初始化/释放的模块，必须先确认如何映射到既有步骤；只有既有语义无法承载时，才由 ATOM-01 负责人提出公共步骤扩展。当前没有批准新增 `InitializeChargeAndForce` 或 `InitializeSafetyServices`，也不把此事项误记为 ATOM-01 已登记的未修复代码缺陷。

---

## 4. 模块对外的三个契约面

每个模块**必须**明确自己在每个契约面上的角色。

### 4.1 生命周期面 —— 实现 `ILifecycleModule`

见 §2、§3。仅用于"初始化/释放"。

### 4.2 输入上报面 —— 注入窄端口，最终委托 `AppCoordinator::ReportXxx`

**这是强制要求，不是建议。**

`StateArbitrator` 虽然暴露了字段级 setter（`SetSbusStatus` / `SetWebOnline` / `SetYawArrived` /
`SetStopFire` / `SetFault` / `SetSoftwareEstopInput` / `SetClock`），但：

1. **禁止**任何 ATOM 直接调用这些 setter；
2. **禁止**任何 ATOM 调用 `StateArbitrator::UpdateInputs()`；
3. **必须**通过确认后的公共输入端口，或模块声明且由装配层绑定的窄回调上报；装配层实现最终委托对应 `AppCoordinator::ReportXxx()`。
4. **禁止**向 ATOM 注入具体 `AppCoordinator&` 或 `StateArbitrator&`。它们的具体类型和头文件只由装配层使用，模块不因此获得私有头依赖。

**理由**：`AppCoordinator` 是外部输入的**唯一写入点**。存在两条独立写入路径时，一次整体替换式
提交会清零其他来源写入的字段（历史缺陷 `ATOM02-BUG-005`）。窄端口不新增第二份输入状态，也不直接写字段；输入仍由 AppCoordinator 唯一转发。单一写入点不等于线程安全，同步方案见根目录 `PLAN.md` §5，当前尚未实施。

**实施状态**：公共输入端口尚未落地。下表列出装配层最终委托的现有方法，不是 ATOM 可以直接包含具体类调用的接口；实际端口签名确认前不得自行假设可用。

| 上报内容 | 应调用的接口 |
|---|---|
| SBUS 在线与自动模式标志 | `ReportSbus(bool online, bool auto_mode)` |
| WEB 客户端连接状态 | `ReportWebOnline(bool)` |
| YAW 到位 bool | `ReportYawArrived(bool)` |
| 停止发射标志 | `ReportStopFire(bool)` |
| 故障状态与等级 | `ReportFault(bool active, FaultSeverity)` |
| 软件急停事实 | `ReportSoftwareEstop(bool)` |
| 单调时钟刷新 | `ReportClock()`（现有方法；先于事件产生的时序与同步方案见根 `PLAN.md`，代码尚未调整） |

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
**必须**通过注入的窄仲裁端口/回调提交 `ControlIntent`，由装配层最终委托 `SubmitIntent(const ControlIntent&)`，并检查 `IntentDecision::accepted`。确认/解除和模式/流程提交同样不通过包含 ATOM-02 私有头获得调用能力；其窄端口仍待落地。

**禁止**把 `accepted == false` 当作成功。

WEB 的 `STOP_FIRE_CLEAR` 在当前权威控制源为 SBUS 时必须拒绝，使用既有 `LOWER_PRIORITY_CONFLICT`，且拒绝不得清标志、切模式或改变自动流程。不能仅凭 SBUS 在线就拒绝当前控制源为 AUTO 的 WEB 请求。现有代码尚未修复，实施位置与用例见根 `PLAN.md` §4。

#### 4.3.1 安全动作必须按类别使用 `IntentKind::SAFETY` / `SAFETY_RECOVERY`

`IntentKind` 的两个安全类别**不可混用**，区分标准是**该动作是否让执行器出力**：

| 动作 | 意图类别 | 放行阶段 |
|---|---|---|
| 零力矩帧发送 | `SAFETY`（不出力） | 急停锁定阶段**即可** |
| 换弹机构抬起 | `SAFETY_RECOVERY`（出力） | 人工确认后的获准恢复阶段，或一般恢复对应阶段；恢复失败锁定时禁止 |
| 人工确认解除后的蓄力双电机回零 | `SAFETY_RECOVERY`（出力） | 同上 |
| 一般故障恢复中的回零、三次发射、换弹抬起 | `SAFETY_RECOVERY`（出力） | 根因已清除、关联接口有效且处于对应恢复阶段；FAULT 不代表自动获得恢复许可 |
| 普通机构动作 | `CONTROL` | 受模式与故障闸门约束 |
| 发射 | `FIRE` | 优先级先由 `SubmitIntent` 判定 |
| 停止发射标志置位 / 清除 | `STOP_FIRE_SET` / `STOP_FIRE_CLEAR` | 见 `Config.md` §11.4 模式限制 |

有效且来源合法的 `IntentKind::SAFETY` 在 `EvaluateIntent` 中位于**急停 / 故障 / 模式闸门之前**放行；
`IntentKind::SAFETY_RECOVERY` 额外要求在 `SystemMode != SOFTWARE_ESTOP` 时才放行，
否则返回 `RejectReason::SOFTWARE_ESTOP`。

上述是现有仲裁器的类别判定，不是完整恢复授权。ATOM-14 仍需限制当前阶段与机构：恢复抬起再次失败进入 FAULT、保留未解除急停锁存，不自动重试。锁定阶段周期发送全部全零帧；人工确认后的恢复阶段按获准电机目标输出，同组未获准电机为零电流，禁止全零帧交替覆盖恢复出力。再次急停或恢复失败立即撤销出力并恢复全部零力矩。

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

> 上述具体函数是装配层委托到 ATOM-02 的实现端。ATOM 只使用窄端口/回调及 `Atoms/Public/StateArbitrationTypes.hpp`，不得为了调用这些函数包含 ATOM-02 私有头。本节不宣称公共调用端口已经存在。

---

## 5. 配置使用

1. **必须**通过配置查询契约 `marshland::config::IConfigView`（`Atoms/Public/IConfigView.hpp`）读取本模块配置项。
   该接口由 `ATOM-15` 实现，由装配层在**构造期**注入（`ConfigStore` 是普通对象，不依赖"初始化完成"才存在）。
2. **必须**按 `Rule/ConfigSchema.md` 的键名（ID）查询；**禁止**自造键名。
3. 除承担配置解析职责的 ATOM-15 外，**禁止**自行打开或解析 JSON 配置文件。
4. **必须**检查 `ConfigStatus`：返回非 `OK` 时**禁止**继续使用该值。
   仅在 `OK` 时 `out` 被写入，非 `OK` 时 `out` 保持调用前的值。
   **禁止**把非 `OK` 当作成功——那会用未校验值驱动执行器。
5. **禁止**为缺失项臆造默认值。`Config.md` 中标记 `[待定]` 的项在 `ConfigSchema.md` 中为 `null`，
   **必须**保持未实现状态，在模块 `PLAN.md` 中登记为待接入点。
   `ATOM-15` 会把 `null` 报告为"未接入"，消费方**禁止**替换为猜测值。
6. 具体通道号、角度、CAN ID、阈值、PID 数值**必须**以 `Design/Config.md` 为唯一来源；
   键名与类型以 `Rule/ConfigSchema.md` 为唯一来源。
7. 需要时序的参数**必须**使用单调时钟（见 §8），**禁止**使用墙上时钟。

> **`RuntimeContext` 与 `IConfigView` 的分工（已实现事实，不可协商）**
>
> `ILifecycleModule::initialize(const RuntimeContext&)` 收到的是 **const 引用**，
> 且 `RuntimeContext` 被 `RuntimeCoordinator` **按值私有持有**。因此配置模块
> **无法**把解析结果写回该 context 供后续模块读取。
>
> - `RuntimeContext::configuration_path`：装配层交给配置模块的**启动入口**（文件路径）。
> - `RuntimeContext::values`：装配层**可选**预填的通道，**不是**运行期权威读取路径。
> - `IConfigView`：**运行期权威读取路径**，且是唯一支持热加载的路径。
>
> 完整论证见 `Atoms/ATOM-15_Config_Persistence/PLAN.md` §2.1（约束 C1/C2/C3）。

> **热加载**：`Design/Config.md` §13.1 要求"参数更新成功后立即生效"。
> 消费方**应**通过 `IConfigView::configVersion()` 判断是否需要重新读取；
> 版本号自增即表示有效配置已变更。**禁止**假设配置在运行期恒定不变，
> 也**禁止**反向依赖 `ATOM-15` 的具体类型。

---

## 6. 模块间依赖

### 6.1 禁止横向包含

**禁止** `#include "ATOM-05_Motor_PID/Inc/..."` 这类跨模块私有头包含。
它会把两个模块的编译单元绑死，并使依赖图不可分析。

### 6.2 优先接口注入

当一个模块需要另一个模块的能力时：

| 方案 | 评价 |
|---|---|
| A. 直接链接目标，但只使用已批准的 Public 导出契约 | 必须声明 ATOM→ATOM 链接；仍不得包含对方 Inc 私有头或使用具体实现类型 |
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

装配层负责把 ATOM-05 的实现适配到这个接口。若确需方案 A，必须在模块 PLAN.md 记录理由、依赖方向和已批准的 Public 契约；没有导出契约时不能以直接链接为由绕过私有头限制。

### 6.3 顺序依赖不等于链接依赖

`RuntimeCoordinator` **只保证执行顺序**，**不建立模块间链接**。例如
"ATOM-05 必须先于 ATOM-12" 只是执行序，不代表 ATOM-12 可以调用 ATOM-05 的符号。

**已确认的真实顺序依赖**（供注册顺序参考）：

```text
ATOM-04 (CAN)    必须先于  ATOM-05 / ATOM-12   （电机与 YAW 走 CAN）
ATOM-05 (Motor)  必须先于  ATOM-10             （换弹左右电机映射、反馈、PID 就绪）
ATOM-07 (IIC)    必须先于  ATOM-10 / ATOM-11   （供弹/发射舵机走 PCA9685A）
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
5. 空注册表必须被拒绝，且必需对象图必须完整。当前 AppCoordinator 错把启动结果当注册信息，连非空注册表首次启动也会被拒绝；修复方案见根 `PLAN.md` §2，不得把此保护写成已正确实现。

---

## 8. 时间、周期与线程

1. **必须**使用单调时钟。现有 `IMonotonicClock` 定义在 AppCoordinator 具体头内，不能作为 ATOM 的直接依赖；其实现 `SystemMonotonicClock` 当前也包含该头。
2. ATOM 通过确认后的独立公共时钟端口、窄回调或显式单调时间参数获得时间，由装配层绑定现有时钟；公共时钟头尚未提取，禁止假设已经可用。需要统一替换和测试的时间源应由装配层注入。
3. 周期**必须**遵守 `Design/Config.md` §4 的规定（系统最长控制周期 100 ms；
   电机闭环/PID/CAN/GPIO/SBUS/WebSocket 均为 20 ms）。
4. 线程调度为 Linux `SCHED_OTHER`，固定周期任务用单调时钟**绝对期限**并监测超期。
   这是**尽力调度，不构成硬实时保证** —— **禁止**假设回调一定准时到达。
5. **禁止**在网络或日志路径上同步阻塞控制与安全任务。
6. 线程实际创建与回收由内核/平台后端实现，生命周期适配器的 initialize/shutdown 委托并确认这些操作，使资源事实可被 RuntimeCoordinator 观测；适配器不内置线程业务循环。
7. 多来源上报、仲裁、确认/解除及查询必须使用统一同步入口，禁止无保护地跨线程保存仲裁器内部引用。根 `PLAN.md` 推荐装配层门面加统一互斥锁，当前仅是方案，未经确认不得视为已落地。

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
- [ ] 配置经 `IConfigView` 按 `Rule/ConfigSchema.md` 的键名读取；检查了 `ConfigStatus`，未把非 `OK` 当作成功
- [ ] 除 ATOM-15 外未自解析 JSON；缺失配置键如实失败；无臆造默认值、无 `[待定]` 项被填值
- [ ] 外部输入经已确认窄端口/回调，装配层最终委托 AppCoordinator；未注入具体 AppCoordinator/StateArbitrator 引用
- [ ] 外部输入未调用 `StateArbitrator::UpdateInputs()`
- [ ] 安全动作按类别提交：零力矩用 `IntentKind::SAFETY`；换弹抬起、蓄力回零与一般故障恢复动作用 `IntentKind::SAFETY_RECOVERY`；普通动作用 `CONTROL`；发射用 `FIRE`
- [ ] 急停锁定阶段（`SystemMode == SOFTWARE_ESTOP`）**未**用 `SAFETY_RECOVERY` 提交任何出力动作
- [ ] `SubmitIntent` 的 `accepted == false` 未被当作成功
- [ ] WEB 清标志在 SBUS 持有控制权时被拒绝，且拒绝没有业务状态副作用
- [ ] 安全恢复出力不被全零帧覆盖；再次急停/恢复失败立即全部零力矩，FAULT 不自动重试
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
| 1 | ATOM-08/09/14 资源归属、初始化与周期调用方式 | 是否需要生命周期适配器或新步骤 | 待契约确认；不预设新增步骤，见 §3.1 |
| 2 | 模块间依赖最终采用"接口注入"还是"直接链接" | 全部模块的依赖图形态 | 本文件 §6.2 建议接口注入，待确认生效 |
| 3 | 配置读取契约 | 全部模块的配置读取方式 | **已解决**：`IConfigView`（`Atoms/Public/IConfigView.hpp`）+ 键名规范 `Rule/ConfigSchema.md`；由 `ATOM-15` 实现、装配层注入 |
| 4 | 结构化生命周期事件的落地（`ILifecycleEventSink`） | ATOM-18 与 ATOM-01 的事件流 | 契约已定义于 `Design/LifecycleEvents.md`，代码未实施 |
| 5 | 不支持运行中重启 | 全部模块对重启的假设 | 已裁决；Runtime 已收紧，不能继续标记待修复 |
| 6 | WEB/SBUS 人工确认请求交给 ATOM-14，恢复成功后再解除锁存 | ATOM-03/14/16 | 归属已明确，窄安全端口和调用链尚未实施 |
| 7 | 输入、仲裁、安全请求和公共时钟调用端口 | 全部接入模块 | 尚未落地；禁止以具体头包含替代契约 |
| 8 | 统一同步入口与引用生命期 | 所有状态读写方 | 根 `PLAN.md` §5 为推荐方案，待确认实施 |

---

## 12. 本规范的可见性（重要）

本文件位于 `Rule/`，符合根 AGENTS.md 的规则文件位置要求。根规则明确只读取根 AGENTS.md 作为工程级规则，禁止读取子目录 AGENTS.md；本次未读取或修改这些文件，不能继续用未复核的子目录内容推断当前可见性或提出修改它们的要求。

模块交接时应明确提供本文件及相关 Design 的访问路径，并核对接收方实际可读范围。受限时由用户或对应负责人安排访问，不复制规则到 Public 形成第二份真相，也不扩大本工程 Agent 的读取或实现权限。

---

## 13. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-09-27 | 首次编制。依据已实现的 ATOM-01/02 与 `Atoms/Public` 契约，规定依赖方向、内核+适配器两层结构、`ILifecycleModule` 适配器契约、`RuntimeStep` 映射与缺口、输入上报唯一写入点、`IntentKind::SAFETY` 安全动作契约、配置使用、模块间依赖与接口注入、注册与所有权、时间与线程、构建验证与自检清单。 |
| 1.1 | 2026-09-27 | 同步 ATOM-02 修复轮后的实际接口：`IntentKind` 拆分为 `SAFETY`（不出力，锁定阶段即可）/ `SAFETY_RECOVERY`（出力，仅人工确认后），§4.3.1 与 §10 自检项随之改写；§4.3.2 补充 `EvaluateFireAuthorization` 的显式 `control_holder` 入参与 `NO_CONTROL_AUTHORITY`；§11 新增急停确认归属与本规范可见性两项待裁决；新增 §12 说明可见性问题。 |
| 1.2 | 2026-10-10 | 对齐双 M3508 与分阶段安全输出；修正具体类注入、时钟依赖及线程生命周期的矛盾；区分公共类型、待落地调用端口和真实代码状态；同步 WEB 清标志限制与根 PLAN 方案；移除未复核子目录规则推断。仅修正文档，未实施代码。 |
| 1.3 | 2026-10-10 | 配置读取改为经 `IConfigView`（新增 `Atoms/Public/IConfigView.hpp` 纯契约头，非可执行代码）：§0 增键名规范引用，§2.2 适配器示例改为注入 `IConfigView` 并检查 `ConfigStatus`，§2.3 表项 4 改写，§5 全节重写（`RuntimeContext` 与 `IConfigView` 的分工、热加载、`ConfigStatus` 必须检查、键名唯一来源 `Rule/ConfigSchema.md`），§10 自检增设配置项，§11 第 3 项标记已解决。**本轮未修改任何可执行代码**（仅新增一个接口声明头、登记其到 Public 的 CMake 接口目标、并更新文档）。 |
