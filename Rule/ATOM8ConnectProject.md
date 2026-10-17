# ATOM-08 接入工程结论 —— 对 `GeneralBugs/20261015-Entry.md` 的负责人评审

**日期**：2026-10-15
**出具方**：ATOM-08_Charge 负责人（工作目录 `../Atoms/ATOM-08_Control`）
**评审对象**：`Atoms/Public/GeneralBugs/20261015-Entry.md`（Entry.cpp 集成 ATOM-08/09/10/11 架构冲突报告）
**结论等级**：原报告定性为 "P0 架构冲突 / 循环依赖死锁"；本评审结论为 **不成立 —— 属装配层构造顺序问题，零 ATOM 改动可解**
**更新（2026-10-16）**：新增 §9「工作线程接入约定」与 §10「`Entry.cpp` 待办清单（含接口变更强制同步）」——用户裁决：**纯逻辑 ATOM 不创建线程，线程由装配层自持、每模块一条**；模块只暴露 `tick(now_us)` 供该线程调用。

---

## 10. `Entry.cpp` 待办清单（2026-10-16，ATOM-08 接口变更后）

> 本节只列装配层需要做的事；ATOM-08 侧不改 `Entry.cpp`。
> **§10.1 是强制同步项：`IMotorCommandSink` 已变更，旧桩不会编译通过。**

### 10.1 【必须】同步电机适配器桩（否则 `marshland` 编译失败）

ATOM-08 的窄端口已改为（`Inc/atom08_charge/charge_interfaces.hpp`）：

| 旧签名 | 新签名 | 用途 |
|---|---|---|
| `submitVelocityTarget(motor_id, rpm)` | `submitVelocityTarget(motor_id, rpm, ControlSource source, ChargeCommandPurpose purpose)` | 补齐 ATOM-05 `MotorCommand` 必需的 `source` / `purpose`，否则恢复回零会被误标为普通命令 |
| `clearVelocityTarget(motor_id)` | `holdZeroVelocityTarget(motor_id)` + `dropToZeroTorque(motor_id)` | 区分"0 rpm 闭环保持"（正常完成/回零完成）与"撤销目标并零力矩"（普通停止/急停/故障/回滚） |

`Entry.cpp:140-159` 的 `StubChargeMotorCommandSink` 替换为：

```cpp
class StubChargeMotorCommandSink final : public marshland::atom08::IMotorCommandSink {
public:
    StubChargeMotorCommandSink(marshland::atom05::MotorCore& motor_core,
                               std::int32_t output_limit_ma) noexcept
        : motor_core_(motor_core), output_limit_ma_(output_limit_ma) {}

    bool submitVelocityTarget(std::uint8_t motor_id, std::int32_t rpm,
                              marshland::ControlSource source,
                              marshland::atom08::ChargeCommandPurpose purpose) noexcept override {
        const auto p = (purpose == marshland::atom08::ChargeCommandPurpose::SafetyRecovery)
            ? marshland::atom05::CommandPurpose::SAFETY_RECOVERY
            : marshland::atom05::CommandPurpose::CONTROL;
        return motor_core_.submitCommand(marshland::atom05::MotorCommand(
                   motor_id, marshland::atom05::MotorMode::VELOCITY,
                   static_cast<double>(rpm), output_limit_ma_, source, p))
               == marshland::atom05::MotorStatus::OK;
    }

    // 正常完成 / 回零完成：0 rpm 闭环保持（仍带有效速度目标）
    bool holdZeroVelocityTarget(std::uint8_t motor_id) noexcept override {
        return submitVelocityTarget(motor_id, 0, marshland::ControlSource::NONE,
                                    marshland::atom08::ChargeCommandPurpose::Control);
    }

    // 普通停止 / 急停 / 故障撤销 / 提交失败回滚：零力矩
    bool dropToZeroTorque(std::uint8_t motor_id) noexcept override {
        return motor_core_.stopMotor(motor_id) == marshland::atom05::MotorStatus::OK;
    }

private:
    marshland::atom05::MotorCore& motor_core_;
    std::int32_t output_limit_ma_{0};
};
```

构造处改为（`output_limit_ma` 从配置读取，键 `motor.control.output_limit`，**禁止硬编码**）：

```cpp
StubChargeMotorCommandSink charge_motor_sink(motor_core, motor_output_limit_ma);
```

### 10.2 【必须】修启动顺序：当前进程会在构造模块之前退出

`Entry.cpp:305-318` 在 `app.Start()`（`:581`）**之前**调用 `config_store.getString("sbus.serial_device")`；配置只在 `LoadConfiguration` 步骤中装载（ATOM-15 适配器 → `config_store.cpp:1025`），未装载时所有 `get*` 返回 `NOT_LOADED` → 该分支打印错误并 `return 1`，**进程在构造 `AppCoordinator`/ATOM-08/09（`:461+`）之前就退出**。

→ 修法：把该读取移到 `app.Start()` 成功之后，或在读取前显式 `config_store.load(context.configuration_path)`。

### 10.3 【必须】为 ATOM-08 建周期线程

按 §9：每模块一条线程，周期取 `timing.motor_loop_period_ms`（上限 `timing.max_control_cycle_ms`），调用 `charge_controller.tick(clock.nowMicros())`；**先 `join` 再 `app.Shutdown()`/析构**。

### 10.4 【必须】命令桥接（WEB / SBUS / AUTO / ATOM-14 → `request*`）

| 来源 | 应调用 |
|---|---|
| WEB `charge_start` / `charge_stop` / `charge_home`（ATOM-16 `IChargeDispatch` 已声明、未注入） | `requestCharge({source=FromControlSource, requester=ControlSource::WEB, request_id, timestamp_us})` / `requestStopCharge()` / `requestHome({source=FromControlSource, requester=WEB, ...})` |
| SBUS 摇杆 | 装配层判定：`manual_charge`(CH2) 或左右通道非零 → `requestCharge(requester=SBUS)`；回中 → `requestStopCharge()`。**该触发规则目前没有实现方，需顶层裁决**（ATOM-08 只接受请求，不轮询起停） |
| AUTO（ATOM-13） | `requestCharge({source=Explicit, left_rpm, right_rpm, requester=AUTO})`，或让其使用配置恒定速度 |
| ATOM-14 恢复/急停解除 | `requestHome({requester=AUTO 或 NONE, ...})`；模式为 `RECOVERY` 时按 `SAFETY_RECOVERY` 放行 |

### 10.5 其余装配层待办（按链路顺序）

| # | 事项 | 归属 |
|---|---|---|
| 1 | `ILimitSwitchState` 适配器：现为恒"未就绪"桩（`:163-168`）→ 回零永不完成、25 s 后 `HomeTimeout` | ATOM-06 实现 + 装配层适配 |
| 2 | 故障上报适配器：`IFaultReporter::reportChargeFault` → `IInputReporter::ReportFault`；并与 `IFaultRegistry`（code 需存在于 `fault.table[]`、等级由表给出）对齐；"蓄力/回零超时"表项缺失待补 | 装配层 + ATOM-14/18 |
| 3 | 真实安全门：`StubMotorSafetyGate` 恒拒（`Entry.cpp:42`）→ ATOM-05 的 `submitCommand` 一律被拦；需 ATOM-02 + ATOM-14 双批准实现 | ATOM-14（+ATOM-02） |
| 4 | 遥测：把 `ChargeController::status()` 接进 `mechanism_state.charge`（`state`/`target`/`feedback`/`error_code`；反馈可取 ATOM-05 `queryMotor`） | 装配层 + ATOM-16/17 |
| 5 | 整机链接：公共 CAN 契约重构后 ATOM-14 仍编译失败（`CanIoResult`/`CanEvent` → 现为 `CanResult`/`CanEventKind`）；ATOM-16 同类（SBUS `readSbusSnapshot`） | ATOM-14、ATOM-16 |

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

此事实已登记于 `../Atoms/ATOM-08_Control` §2（"`RuntimeStep` 无 ATOM-08 对应步骤；`addModule()` 的步骤归属、`required` 与对象寿命由顶层装配/设计方决定"）与 §13（待外部裁决项 HB-4）。因此：

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

## 9. 工作线程接入约定（2026-10-16 用户裁决）

**结论**：纯逻辑 ATOM（ATOM-08/09/…）**不创建线程**；线程由**装配层**在装配时创建、关闭时结束，**每模块一条**。模块只暴露 `tick(now_us)` 供该线程调用。

### 9.1 责任分工

| 责任 | 归属 |
|---|---|
| 创建/启动工作线程、决定周期与调度 | **装配层**（`Entry.cpp`） |
| 周期内推进模块状态（计时/完成/超时/双限位判定/急停与故障撤销） | 模块 `ChargeController::tick(std::int64_t now_us)` |
| 线程结束与 `join` | **装配层**，且必须在 `app.Shutdown()` 与对象析构**之前** |
| 配置装载、目标撤销 | 模块（`configure()` / `reset()`，仅经 `LifecycleAdapter`） |

### 9.2 装配层参考实现（每模块一条线程）

```cpp
// 周期必须来自配置（Rule/ConfigSchema.md §1.7：禁止臆造默认值）
std::int64_t tick_period_ms = 0;
if (config_store.getInt("timing.motor_loop_period_ms", tick_period_ms) != marshland::config::ConfigStatus::OK) {
    // 如实失败：缺失/非法周期不得用猜测值代替
}

std::jthread charge_worker([&charge_controller, &clock](std::stop_token stop) {
    auto next = std::chrono::steady_clock::now();          // 单调时钟绝对期限
    while (!stop.stop_requested()) {
        charge_controller.tick(clock.nowMicros());
        next += std::chrono::milliseconds(tick_period_ms);
        std::this_thread::sleep_until(next);
    }
});

// 关闭顺序（硬约束）：
charge_worker.request_stop();
charge_worker.join();      // ① 必须先 join
app.Shutdown();            // ② 再走各模块 shutdown()（LifecycleAdapter -> ChargeController::reset()）
// ③ 之后才允许销毁 charge_controller 及其注入的桩/适配器
```

### 9.3 硬约束

1. **先 `join` 再 `Shutdown()`/析构**：`tick` 会访问注入的 `IMotorCommandSink` / `ILimitSwitchState` / `IStateReader` 等对象；线程未结束就析构业务对象会造成悬垂访问（同类问题见 `GeneralBugs` 的 `ALL-ASM-10`）。
2. **周期取自配置**：`timing.motor_loop_period_ms`，以 `timing.max_control_cycle_ms` 为上界；缺失/越界如实失败，禁止装配层硬编码周期。
3. **单调时钟绝对期限 + 超期监测**：`Design/Config.md` §15；调度为 `SCHED_OTHER`，不构成硬实时保证。
4. **模块侧零线程**：`ChargeController` 头文件不得出现 `std::thread` / `condition_variable` / `atomic` 成员（2026-10-16 已回退为外部 tick）。
5. **每模块一条线程**：ATOM-08 与 ATOM-09/10 各自一条，互不共用。
6. **`tick` 的可见性**：`tick` 属**装配部分**，仅装配层调用；模块**不向任何其他 ATOM 提供接口**，跨模块时序由装配层持有。

### 9.4 与 `Rule/AtomIntegration.md` 的关系

该文件 §3.1(b) 已写明"ATOM-08/09 当前计划采用外部 update/tick，**不自行创建线程**"，本约定与之**一致**；§8.6"生命周期适配器不内置线程业务循环"同样满足（适配器只做配置装载与结果翻译）。

---
