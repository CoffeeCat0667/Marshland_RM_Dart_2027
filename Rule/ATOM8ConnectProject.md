# ATOM-08 接入工程结论 —— 对 `GeneralBugs/20261015-Entry.md` 的负责人评审

**日期**：2026-10-15
**出具方**：ATOM-08_Charge 负责人（工作目录 `Atoms/ATOM-08_Charge`）
**评审对象**：`Atoms/Public/GeneralBugs/20261015-Entry.md`（Entry.cpp 集成 ATOM-08/09/10/11 架构冲突报告）
**结论等级**：原报告定性为 "P0 架构冲突 / 循环依赖死锁"；本评审结论为 **不成立 —— 属装配层构造顺序问题，零 ATOM 改动可解**

---

## 1. 总结论

1. 原报告所称"循环依赖死锁"（模块构造 → 需要 AppCoordinator → AppCoordinator 需要模块先注册 → 注册需要模块先构造）**不成立**，其推理基石是一个与 Public 契约事实不符的前提。
2. 该问题**不需要修改 ATOM-08/09/10/11 的任何代码**，也不需要修改 Public 契约；仅需调整 `Entry.cpp` 中的对象构造顺序。
3. ATOM-08 的构造函数注入设计（`IControlAuthority&` / `IStateReader&`）严格符合 `Rule/AtomIntegration.md` §4.2 / §4.3 / §6.2，**不做变更**。
4. 同意原报告的短期处置（回滚 `Entry.cpp` 至 ATOM-07 稳定版本、暂停接入），不同意其根因定性与"需要架构决策"的结论。

---

## 2. 事实核查：原报告的核心前提与 Public 契约不符

原报告 §2.1 声称"`AppCoordinator` 必须在所有生命周期模块注册完成后才能构造"。核查结果：

### 2.1 `AppCoordinator` 构造函数只保存引用，无注册前置条件

`Atoms/Public/AppCoordinator/Src/AppCoordinator.cpp:28-33`：

```cpp
AppCoordinator::AppCoordinator(runtime::RuntimeCoordinator& runtime,
                               state_arbitration::StateArbitrator& authority,
                               IMonotonicClock& clock) noexcept
    : runtime_(runtime), authority_(authority), clock_(clock)
{
}
```

构造函数体内**没有任何**"注册已完成"的检查。

### 2.2 真正的时序约束在 `Start()`，且仅要求"注册先于 Start"

`Atoms/Public/AppCoordinator/Src/AppCoordinator.cpp:51-81`：

- `Start()` 入口检查 `runtime_.state() == runtime::RuntimeState::Created`；
- 在 `Start()` 时才经 `RegisteredModuleCount(runtime_)` 查询已注册模块数（要求 > 0）；
- 之后才执行 `runtime_.start()`。

注册信息在 `Start()` 时才被查询，证明**注册窗口一直开放到 `Start()` 为止**。正确约束是：

> 所有 `addModule()` 必须先于 `AppCoordinator::Start()`，而不是先于 `AppCoordinator` 的构造。

### 2.3 推论

"ATOM-08 适配器必须在 `AppCoordinator` 构造之前注册"这一约束在 Public 契约中**不存在**。原报告§8.1"尝试 2"自己也承认在 `AppCoordinator` 之后构造内核在对象层面可行，只是被上述不存在的前提挡住。

---

## 3. 正确解法：纯 Entry.cpp 构造顺序重排

```cpp
// 1. 基础设施 + 硬件/服务层模块注册（ATOM-01/03/04/05/07/15 适配器）
RuntimeCoordinator runtime{context};
runtime.addModule({...});

// 2. 先构造 AppCoordinator（此时不要求全部模块已注册）
AppCoordinator app{runtime, arbitrator, clock};

// 3. 构造 ATOM-08 内核，注入 app 提供的 Public 接口
marshland::atom08::ChargeController charge_controller(
    config_store, clock, charge_motor_sink, charge_limit_switches,
    app,          // IControlAuthority&
    app,          // IStateReader&
    sbus_core, charge_fault_reporter);
marshland::atom08::LifecycleAdapter charge_adapter(charge_controller, config_store);

// 4. 注册业务层适配器（仍在 Start() 之前，满足唯一真实约束）
runtime.addModule({...});

// 5. 最后启动
app.Start(&detail);
```

- 改动范围：仅 `Entry.cpp` 的语句顺序。
- 对 ATOM-08/09/10/11：**零改动**；对 Public 契约：**零改动**；对已接入的 7 个模块：构造顺序不受影响。

---

## 4. 补充：`RuntimeStep` 封闭枚举问题（我方 PLAN 早已登记）

`Atoms/Public/ILifecycle.hpp:50-67` 明确：`RuntimeStep` 是**封闭的硬件/服务初始化步骤枚举**，注释写明"业务系统状态……不得作为 RuntimeStep 塞进本枚举"。其中**没有 ATOM-08 对应的步骤**。

此事实已登记于 `Atoms/ATOM-08_Charge/PLAN.md` §2（"`RuntimeStep` 无 ATOM-08 对应步骤；`addModule()` 的步骤归属、`required` 与对象寿命由顶层装配/设计方决定"）与 §13（待外部裁决项 HB-4）。因此：

- "ATOM-08 适配器必须经 `addModule()` 注册"本身就是顶层装配方的**待决假设**，不是既定架构；
- 业务层模块由装配层在 `AppCoordinator` 构造之后显式调用 `initialize()` 也完全可行；
- 原报告把两个未决的装配层选择误当成了 ATOM 模块的设计缺陷。

---

## 5. 对原报告各方案的态度

| 方案 | 立场 | 理由 |
|---|---|---|
| **Entry.cpp 重排**（原报告未识别） | **首选** | 零 ATOM 改动、零 Public 改动，符合全部 Public 契约 |
| 方案 A（工厂延迟构造） | 不必要 | 且原报告自相矛盾：声称"不修改 ATOM 模块代码"，却要求修改 4 个 LifecycleAdapter 实现 |
| 方案 B（提前构造 AppCoordinator） | 原报告误判 | 不需要修改 `AppCoordinator`/`RuntimeCoordinator` 职责边界，只需调整 Entry.cpp 构造顺序 |
| 方案 C（setter 注入） | **明确反对** | 引入"部分初始化对象"风险期；`AtomIntegration.md` §4.3 要求构造即具备完整安全门控能力，setter 空窗期内 `authority_` 为空是安全隐患；原报告自己也承认其违反"不修改 ATOM 代码"约束 |
| 方案 D（fail-closed 代理） | 备选 | 仅在"重排"被证伪时采用。代理属装配层代码：`AppCoordinator` 就绪前 `SubmitIntent` 返回 `accepted=false`、`ReadState` 返回 `UNINITIALIZED`——与 ATOM-08 默认拒绝的安全语义天然一致 |

---

## 6. ATOM-08 侧不做变更的声明

1. `Inc/atom08_charge/charge_controller.hpp:25-32` 的构造注入签名**保持不变**：
   - 依赖注入 `IControlAuthority` / `IStateReader` 是 `AtomIntegration.md` §4.2/§4.3 的强制要求（受控动作必须经 `SubmitIntent()` 仲裁、状态必须经 `IStateReader` 读取）；
   - §6.2 方案 B 要求模块自声明窄接口、由装配层适配，ATOM-08 已照此实现；
   - 改为 setter 注入或工厂注入均无契约依据。
2. `LifecycleAdapter` 维持"唯一 `ILifecycleModule` 实现、独立 TU、无业务逻辑、不自注册"的现状（`PLAN.md` §3.6）。
3. 注册步骤选择、`required`、对象寿命与 tick 周期调用方，仍属顶层装配/设计方职责（`PLAN.md` §3.2、§13 HB-1/HB-4）。

---

## 7. 边界声明与验证依据

1. 本评审未读取 `Entry.cpp` 与 `RuntimeCoordinator.hpp`（后者为 ATOM-01 私有头，超出 ATOM-08 工作目录的授权阅读范围）。
2. 结论依据全部来自可核实的 Public 契约：
   - `Atoms/Public/AppCoordinator/Inc/AppCoordinator.hpp`
   - `Atoms/Public/AppCoordinator/Src/AppCoordinator.cpp`
   - `Atoms/Public/ILifecycle.hpp`
3. 若顶层装配方能举证 `RuntimeCoordinator::addModule()` 存在本评审不可见的隐藏约束（例如在 `AppCoordinator` 构造后冻结注册），本结论将重新评估；在此之前，以第 1 节总结论为准。

---

## 8. 建议行动

1. 顶层装配方按第 3 节重排 `Entry.cpp` 构造顺序后重试集成；
2. 无需召开架构评审，无需修改 `Rule/AtomIntegration.md`；
3. 原报告 `GeneralBugs/20261015-Entry.md` 建议标注"根因已修正定性：装配层构造顺序问题，非架构冲突"，避免后续模块（ATOM-12/13/14/18）按错误结论设计。
