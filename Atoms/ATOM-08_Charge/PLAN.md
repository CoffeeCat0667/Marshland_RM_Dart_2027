# ATOM-08_Charge 实现方案（PLAN）

> 版本：2.5
> 更新日期：2026-10-14
> 设计基线：`Design/Atomic/ATOM-08_Charge.md`（同步基线 PRD 0.10 / PEF 1.5 / Interface 1.5 / Config 0.8）
> 接入规范：`Rule/AtomIntegration.md` v1.4（本轮已按授权做一次现实同步）
> 键名规范：`Rule/ConfigSchema.md` v1.4；机器可读契约 `Rule/ConfigSchema.schema.json`
> 接入样例基线：`Atoms/ATOM-03_SBUS`
> 当前实现状态：**已实现**（`Inc/atom08_charge/` 5 个头 + `Src/` 3 个源），ARM 增量编译与 `-Wall -Wextra -Wpedantic` 严格检查均通过（见 §13）；装配层接入与 ATOM-06 联调待顶层装配方。
> 本轮已获授权的改动：`PLAN.md`（本文）、`CMakeLists.txt`（补齐 `Marshland::PublicHeaders` 链接块）、`Rule/AtomIntegration.md`（现实同步 v1.4），以及经用户"直接完整实现"授权的 `Inc/atom08_charge/`（5 个头）与 `Src/`（3 个源）。

---

## 1. 目标与边界

### 1.1 单一职责

控制蓄力机构两个电机（右蓄力电机 1 / node 0x201、左蓄力电机 2 / node 0x202）的

- 正常恒速蓄力、普通停止；
- 双限位回零；
- 蓄力机构状态维护与查询。

### 1.2 覆盖的内部原子操作（`ATOM-08_Charge.md`）

| # | 原子操作 | 本模块落点 |
|---|---|---|
| 1 | 校验蓄力命令的运行模式、控制权和安全条件 | `ChargeController::requestCharge()` |
| 2 | 分别向电机 1、2 提交配置的恒定速度目标 | `ChargeController` → `IMotorCommandSink` |
| 3 | 从本次蓄力开始时刻启动 Config 蓄力时间 X 计时 | `ChargeController::tick(now_us)` |
| 4 | X 秒到期后结束正常蓄力并置动作完成 | 同上（完成判定优先于超时） |
| 5 | 接受普通停止蓄力请求并停止两个电机的普通蓄力目标 | `requestStopCharge()` |
| 6 | 向电机 1 提交回零目标 | `requestHome()` |
| 7 | 向电机 2 提交回零目标 | 同上 |
| 8 | 读取两个限位开关的稳定状态 | 窄端口 `ILimitSwitchState`（装配层注入 ATOM-06 的"双限位是否同时有效且稳定为低"查询）；**本模块不读取 `gpio.*` 配置**（选项 A，见 §4.1） |
| 9 | 仅当双限位均低电平时停止回零并标记回零完成 | `tick()` 回零判定 |
| 10 | 查询蓄力状态、目标、完成状态或最近错误 | 只读快照接口 |
| 11 | 将蓄力超时、电机或限位错误报告给 ATOM-14 | `IFaultReporter` 窄端口 |

### 1.3 明确不做（不属于本模块职责）

- 不实现 `main()`、可执行文件、线程创建与进程启动（ATOM-01）；
- 不实现状态机、控制权仲裁与发射授权（ATOM-02）；
- 不实现 CAN 收发与报文编解码（ATOM-04）、电机 PID 闭环与反馈解析（ATOM-05）；
- 不实现 GPIO 采样/去抖/稳定状态判定（ATOM-06）；本模块只消费其"稳定状态 + 双限位回零判据"；
- 不实现固定四发流程编排（ATOM-13）、故障与安全恢复编排（ATOM-14）；
- 不实现配置解析、校验与持久化（ATOM-15），不自行打开或解析 JSON；
- 不实现 HTTP/WebSocket 路由与推送（ATOM-16/17），不接受网络直连；
- 不实现 ATOM-08 之外任何 `Design/Atomic` 文档描述的功能。

---

## 2. 规范符合性（依据 `Rule/AtomIntegration.md` v1.4）

| 条款 | 本模块方案 |
|---|---|
| §1 依赖方向 `ATOM → Public` | 只包含 `Atoms/Public/` 契约头与 C++ 标准库；**禁止**包含 `AppCoordinator.hpp`、`SystemMonotonicClock.hpp` 或任何 `ATOM-NN` 私有头 |
| §2 内核 + 适配器两层 | 纯逻辑内核 `ChargeController` + 薄生命周期适配器 `LifecycleAdapter`（独立 TU，≤ 50 行，无业务逻辑） |
| §2.3 适配器六条 | `initialize()` 如实反映成败、`error_message` 可定位、`shutdown()` noexcept/幂等、配置只读 `IConfigView`、不判断流程与控制权、不内置线程业务循环 |
| §3 / §3.1(c) `RuntimeStep` 与注册分工 | `RuntimeStep` 无 ATOM-08 对应步骤。本模块**提供** `ILifecycleModule` 适配器作为 **ATOM 内注册接口**；`addModule()` 的步骤归属、`required` 与对象寿命由**顶层装配/设计方**决定，本模块不自注册 |
| §4.1 生命周期面 | 见上；适配器只做"配置检查 → 内核启动 → 结果翻译" |
| §4.2 输入上报面 | 使用 `Atoms/Public/AppInterfaces.hpp` 的 `IInputReporter` / `IStateReader`；不调用 `StateArbitrator` 任何 setter、不调用 `UpdateInputs()`、不注入具体 `AppCoordinator&`/`StateArbitrator&` |
| §4.3 仲裁面 | 受控动作先 `IControlAuthority::SubmitIntent()` 并检查 `IntentDecision::accepted`；`accepted == false` **禁止**当作成功 |
| §4.3.1 安全动作类别 | 普通蓄力/停止 = `CONTROL`；**回零（含急停人工确认后的双电机回零、一般故障恢复回零）= `SAFETY_RECOVERY`**；零力矩/停止出力 = `SAFETY`；`SystemMode == SOFTWARE_ESTOP` 阶段**禁止**提交任何出力动作 |
| §4.3 边界 | 不把安全寄托于调用方查询 `RunGate()`；急停锁定/故障活动期间**本模块自身拒绝普通蓄力命令** |
| §5 配置使用 | 只经 `IConfigView` 按 `Rule/ConfigSchema.md` 键名读取；检查 `ConfigStatus`，非 `OK` 不使用该值；缺失不臆造默认值；不填充 `[待定]` 项；按 `configVersion()` 支持热加载 |
| §6 模块间依赖 | 采用 §6.2 方案 B（接口注入）：本模块自声明 `IMotorCommandSink` / `ILimitSwitchState` / `IFaultReporter` / 速度来源端口，由装配层适配 |
| §7 注册与所有权 | 注册只在装配层；内核与所有被引用对象寿命由装配层保证 |
| §8 时间与线程 | 使用 `IMonotonicClock`（`nowMicros()`）；不创建线程，由上层周期调用 `tick(now_us)`；禁止墙上时钟；控制路径不阻塞网络/日志 |
| §9 构建与验证 | 增量编译；禁止 x86 工具链；仅 WSL aarch64 交叉编译或目标 Ubuntu ARM 主机编译；`-Wall -Wextra -Wpedantic` 零警告；模块内不设标准/架构/选项；不建测试用 `main()` |
| §10 自检清单 | 见 §12 |

---

## 3. 接入系统的方式（对齐 `ATOM-03_SBUS` 基线）

### 3.1 CMake 接入方向

```text
Marshland executable
  -> Atoms/Public/AppCoordinator（顶层装配，仅装配层可见）
  -> Marshland::atom_08_charge
  -> Marshland::PublicHeaders
```

- 目标名 `atom_08_charge`，别名 `Marshland::atom_08_charge`；
- 不定义 `project()`、不产生可执行文件、不设置架构/标准/编译选项；
- 有 `Src/*.cpp` 时建 `STATIC` 库，否则 `INTERFACE` 占位；
- **本轮已补齐**（授权一次）与 `ATOM-03_SBUS` 一致的公共契约链接块：

```cmake
if(TARGET Marshland::PublicHeaders)
    target_link_libraries(${ATOM_MODULE_TARGET} ${ATOM_MODULE_SCOPE}
        Marshland::PublicHeaders)
endif()
```

- 上层 `add_subdirectory(Atoms/ATOM-08_Charge)` 与顶层 CMake 由顶层设计/装配方负责，本模块不修改。

### 3.2 装配对象图（由顶层装配方创建并持有）

```text
IConfigView 实现（ATOM-15）
IMonotonicClock 实现（装配层绑定）
IMotorCommandSink 适配器（包装 ATOM-05：按电机 ID 提交速度目标/停止）
ILimitSwitchState 适配器（包装 ATOM-06：双蓄力限位稳定状态 + 回零条件）
ISbusSnapshotReader（ATOM-03，Public 契约 SbusInterfaces.hpp）或等价窄端口
IControlAuthority / IStateReader（ATOM-02，经 AppCoordinator 绑定）
IInputReporter（故障与状态上报，经 AppCoordinator 绑定）
ChargeController(上述引用)
LifecycleAdapter(ChargeController&, IConfigView&)
```

要求：

1. `ChargeController` 与 `LifecycleAdapter` 只持引用，装配层保证被引用对象寿命更长；
2. 不注入具体 `AppCoordinator&` / `StateArbitrator&`；
3. 不横向包含其他 ATOM 的头；
4. `LifecycleAdapter` 由装配层注册，注册点检查 `addModule()` 返回值与 `lastRegistrationError()`（由装配层执行）。

### 3.3 顺序依赖（仅执行序，不建立链接依赖）

```text
ATOM-04 (CAN)        必须先于 ATOM-05        （电机走 CAN）
ATOM-05 (Motor/PID)  必须先于 ATOM-08        （速度目标提交与反馈/错误就绪）
ATOM-06 (GPIO/限位)  必须先于 ATOM-08        （双限位稳定状态与回零判据）
ATOM-15 (配置)       必须先于 ATOM-08        （IConfigView 可查询）
ATOM-02 (仲裁)       必须先于 ATOM-08 的受控动作（SubmitIntent / 状态读取）
ATOM-03 (SBUS)       必须先于 ATOM-08 的 SBUS 手动速度读取（快照可用）
```

以上顺序**不**授予 ATOM-08 调用其他 ATOM 符号的能力。

### 3.4 时间与调用模型

- 本模块不创建线程、不注册固定周期任务；
- 装配层按控制周期调用 `tick(now_us)` 推进计时、超时与限位判定；
- 周期约束遵循 `Config.md` §4/§15（系统最长控制周期 100 ms；电机/CAN/GPIO 20 ms）；不假设回调准时到达；
- 全部计时使用单调时钟。

### 3.5 命令入口与速度来源（依用户裁决：先定控制权，再按来源取值）

1. **先确定控制权归属**：经 `IStateReader::ReadState()`（或等价 Public 状态读取）取 `ArbitrationContext.control_source`（`SBUS` / `WEB` / `AUTO` / `NONE`）；受控动作再经 `IControlAuthority::SubmitIntent()` 仲裁。
2. **再按来源读取速度目标**：

| 控制源 | 速度来源 | 通道/字段 |
|---|---|---|
| `SBUS` | `ISbusSnapshotReader` 快照 | **CH2（整体）与 CH8 左 / CH9 右都要**；**CH2 非零时优先级更高，覆盖 CH8/CH9 指令**；通道映射取自 `sbus.functions[]`（当前仅登记 `manual_charge→CH2`，CH8/CH9 待补，见 §4.2-P3） |
| `WEB` | 命令自带目标（`charge_start` 动作/参数下发） | 由装配层解析后传入 |
| `AUTO` | ATOM-13 自动流程下发的目标；未下发时使用配置恒定速度 | `charge.charge_speed_rpm`（已落地 = 5000） |
| `NONE` | 无控制权 | 拒绝普通蓄力 |

> **左右独立（R2 已裁决）**：模块内目标模型按**左右独立**设计（电机 1、2 各持目标、各判成败），不假设两电机同值；SBUS 路径天然独立（CH8/CH9），WEB 路径可分别下发，AUTO 路径当前配置仅提供单一恒定速度（见 §4.2-P4）。

3. **命令入口**：`charge_start` / `charge_stop` / `charge_home`（`Config.md` §11.4，仅手动/调试模式；受 SBUS 优先级、急停、故障限制）。ATOM-16 只解析动作名并转交装配层；ATOM-08 不实现 HTTP，只在被注入窄端口时读取 SBUS 快照。

### 3.6 ATOM 内注册接口（对外契约）

```cpp
// Inc/atom08_charge/lifecycle_adapter.hpp
class LifecycleAdapter final : public marshland::runtime::ILifecycleModule {
public:
    LifecycleAdapter(ChargeController& core,
                     const marshland::config::IConfigView& config) noexcept;
    marshland::runtime::ModuleInitResult initialize(
        const marshland::runtime::RuntimeContext& context) override;  // 只做配置检查与内核启动
    void shutdown() noexcept override;                                // noexcept 且幂等
    const char* name() const noexcept override;                       // "atom08-charge"
private:
    ChargeController& core_;
    const marshland::config::IConfigView& config_;
};
```

- 该适配器是模块唯一实现 `ILifecycleModule` 的类，且是独立翻译单元；
- **外部注册（`addModule()`、步骤选择、`required`）由顶层装配/设计方完成**，本模块不注册；
- `RuntimeContext::configuration_path` 由配置模块消费，本模块不使用。

---

## 4. 配置接入

### 4.1 本模块读取的配置键

| 查询键 | 类型/单位 | 本模块用途 |
|---|---|---|
| `timing.charge_time_s` | int / s（30） | 正常蓄力完成判据 X |
| `timing.charge_max_action_ms` | int / ms（30000） | 蓄力最大动作时间；完成判定优先于超时 |
| `timing.charge_home_max_action_ms` | int / ms（25000） | 回零最大动作时间（R3） |
| `charge.charge_speed_rpm` | int / rpm（5000） | 正常蓄力恒定速度（含方向符号） |
| `charge.home_speed_rpm` | int / rpm（5000） | 回零速度（含方向符号） |
| `charge.target_range` | int / rpm（15000，对称 ±） | 目标越界拒绝依据（`W-CMD-001`） |
| `motor[]`（`id`/`role`） | int / str | 按 `role` 解析 `charge_left` / `charge_right` 的电机 ID；**禁止硬编码电机 ID** |
| `sbus.functions[]`（`name`/`channel`） | str | 解析 `manual_charge`（整体）/ `manual_charge_left` / `manual_charge_right` 的通道下标；`channel` 为 null 或不匹配视为未映射；**禁止硬编码 CH 编号** |

**不属于本模块读取的键（选项 A 裁决，2026-10-14）**

| 键 | 归属 | 说明 |
|---|---|---|
| `gpio.*`（`charge_home_criterion`、`limit_switch[]`、`active_level`、`default_level`、`debounce_ms`、`sample_period_ms`、`read_failure_handling`、`limit_trigger_action`、`report_on_change`、`report_max_delay_ms`） | **ATOM-06** | 限位采样、去抖、有效电平与"双限位均低"判据由 ATOM-06 负责；ATOM-08 只调用窄端口 `ILimitSwitchState`，**不读这些键**，也不因它们缺失而失败 |

> 该裁决意味着 `gpio` 段缺失（见 `Atoms/Public/GeneralBugs/2026-10-14-ATOM-08.md` BUG-20261014-02）**不阻塞 ATOM-08**，只影响 ATOM-06 自身的实现与联调。

### 4.2 配置落地核对与待补项（2026-10-14 核对 `Config/dart_control.json`）

**已核对落地**

| 键 | 现值 | 状态 |
|---|---|---|
| `charge.charge_speed_rpm` | 5000 | 已落地（`Design/Config.md` §5.5、`ConfigSchema.md` §2.5、`schema.json` 的 `charge`） |
| `charge.home_speed_rpm` | 5000 | 已落地 |
| `timing.charge_time_s` | 30 | 已落地 |
| `charge.target_range` | 15000 | 已落地（对称 ±） |
| `gpio.*`（10 键） | **config 缺失** | **归 ATOM-06；本模块不读取（选项 A）**，不阻塞 ATOM-08 |

**曾列待补项（2026-10-14 已全部闭环，保留记录）**

| # | 问题 | 需要配置维护方做的动作 |
|---|---|---|
| P1 | `timing.charge_max_action_ms` 现值为 **30**（ms），与已裁决的 30000 ms 不符；30 ms 小于 20 ms 控制周期，会导致蓄力立刻判超时 | 改为 `30000`，并确认 `Design/Config.md` §4 的"30 s"与键名 ms 的换算已一致 |
| P2 | 回零最大动作时间键不存在（R3 已裁决新增，25 s） | 新增 `timing.charge_home_max_action_ms = 25000`，登记 `ConfigSchema.md` §2.3、`schema.json` 的 `timing` 块、`Design/Config.md` §4 |
| P3 | `sbus.functions[]` 只登记 `manual_charge → CH2`，CH8/CH9 无功能映射；R1 要求 CH2 与 CH8/CH9 都要 | 在 `sbus.functions[]` 增加左右蓄力速度项（如 `manual_charge_left → CH8`、`manual_charge_right → CH9`），并同步 `ConfigSchema.md` §2.8.1 与 `schema.json` 中 `functions[].name` 的封闭枚举；若坚持固定使用 CH8/CH9 而不入表，请在 `Design/Config.md` §7.1 明确写死，我按固定通道读取 |
| P4 | R2 裁决"需要左右独立"，但配置只有单一一对 `charge_speed_rpm` / `home_speed_rpm` | 二选一：**(a)** 自动/无人下发路径两电机共用这对值，左右独立性只体现在 SBUS（CH8/CH9）与 WEB 下发目标；**(b)** 确需配置内独立，则补 `charge.charge_speed_left_rpm`/`_right_rpm` 与 `charge.home_speed_left_rpm`/`_right_rpm`（现有两键可作默认值），并同步三处文档 |

**可选（未落地，不阻塞）**

- `charge.target_range`（对称 ±，int / rpm）：若需按 `W-CMD-001` 拒绝越界目标则补此键；不加则本模块只做类型、有限性与 int 可表示性校验，**不臆造范围**。
- `charge.control_mode` / `charge.target_unit` / `charge.completion_criterion` / `charge.has_dedicated_fault_handling`：与 `force.*` 对齐的元数据，非必需；本模块不读取未登记的键。

**核对结论（2026-10-14，读 `Config/dart_control.json` + `Rule/ConfigSchema.schema.json`）**

- ✅ P1 `timing.charge_max_action_ms = 30000`
- ✅ P2 `timing.charge_home_max_action_ms = 25000`（config 与 schema 均已登记）
- ✅ P3 `manual_charge_left → CH8`、`manual_charge_right → CH9` 已加入配置，且已登记进 `functions[].name` 枚举；未发现任何配置键超出 schema 声明
- ✅ P4 采用**选项 (a)**：`charge` 段只有 `charge_speed_rpm` / `home_speed_rpm`，左右独立性由 SBUS CH8/CH9 与 WEB/ATOM-13 下发目标体现
- ✅ 另加可选键 `charge.target_range = 15000`（对称 ±，schema 默认值同值）
- ✅ `reserved_001` 由 schema 的 `patternProperties ^reserved_[0-9]{3}$` 覆盖，非未登记键
- ⚠️ **`gpio` 段整体缺失**（10 键）：schema 未把 `gpio` 列入 `required`，校验通过但运行期查询返回 `MISSING`。**按选项 A 裁决，此缺口归 ATOM-06，不阻塞 ATOM-08**——ATOM-08 不读 `gpio.*`，回零判据由 ATOM-06 的窄端口给出。已登记为 `GeneralBugs/2026-10-14-ATOM-08.md` BUG-20261014-02；用户已决定本阶段不补该段。
- ⚠️ `sbus.rules[]` 缺失（`ConfigSchema.md` §2.8.2 要求入 JSON）：非本模块直接消费，但 §3.5 第 1 步的"控制权归属判定"依赖它，属 ATOM-02 的配置缺口。
- （非本模块）`fault` 段亦缺失（`fault.table` / `fault.rules`），影响 ATOM-14/18。
### 4.3 使用纪律

1. 只经 `Atoms/Public/IConfigView.hpp` 查询，禁止自解析 JSON；
2. 必须区分 `ConfigStatus`，非 `OK` 不使用 `out` 值；
3. 缺失/非法必需项如实失败，禁止隐含默认值；
4. `null`（`[待定]`）项保持未实现并登记；
5. 通过 `configVersion()` 处理热加载。

---

## 5. 代码结构（已实现，2026-10-14）

```text
ATOM-08_Charge/
├── CMakeLists.txt                      # STATIC 目标 atom_08_charge + Marshland::atom_08_charge；链接 Marshland::PublicHeaders
├── PLAN.md
├── Inc/atom08_charge/
│   ├── charge_types.hpp                # 状态/动作/目标来源/请求结果/故障类别/请求/只读快照
│   ├── charge_interfaces.hpp           # IMotorCommandSink / ILimitSwitchState / IFaultReporter
│   ├── charge_config.hpp               # ChargeConfig + LoadChargeConfig(IConfigView&)
│   ├── charge_controller.hpp           # 纯逻辑内核 ChargeController（configure/reset/request*/tick/status）
│   └── lifecycle_adapter.hpp           # ATOM 内注册接口（唯一 ILifecycleModule 实现，见 §3.6）
└── Src/
    ├── charge_config.cpp               # 配置读取与校验（含 motor[]、sbus.functions[] 解析）
    ├── charge_controller.cpp           # 校验 → 仲裁 → 提交 → 计时 → 限位判定 → 故障上报
    └── lifecycle_adapter.cpp           # 初始化编排与结果翻译（31 行）
```

- 命名空间 `marshland::atom08`；头文件只在 `Inc/atom08_charge/`；内核只依赖 `Atoms/Public/` 与标准库。
- 窄接口语义：`IMotorCommandSink`（装配层适配 ATOM-05 的速度目标提交/撤销）、`ILimitSwitchState::readChargeLimit(motor_id, sample)`（装配层适配 ATOM-06，本模块不读 `gpio.*`）、`IFaultReporter::reportChargeFault(code, severity)`（装配层委托 `IInputReporter::ReportFault`）。

---

## 6. 状态机与判定

### 6.1 状态

| 状态 | 含义 |
|---|---|
| `Uninitialized` | 未初始化/配置不可用 |
| `Idle` | 空闲，可接受蓄力或回零请求 |
| `Charging` | 双电机正常恒速蓄力并累计 X 秒 |
| `ChargeCompleted` | 正常蓄力 X 秒到期完成 |
| `Returning` | 双电机执行回零目标 |
| `ReturnCompleted` | 双限位同时稳定为低电平 |
| `Stopped` | 普通停止后空闲（**不等价于**回零完成） |
| `Fault` | 故障，保留最近错误码并停止相关动作 |

### 6.2 不变式

1. 初始化失败、急停锁定、控制授权不通过、模式不允许 → 拒绝普通蓄力启动；
2. X 秒未到期不得报告完成；`timing.charge_max_action_ms`（30000 ms）到期进入故障处理，且完成判定优先于超时；
3. 双限位未同时稳定为低电平时不得报告回零完成；读取失败/输入无效不得视为低电平；回零超过 `timing.charge_home_max_action_ms`（25000 ms）未完成时进入故障处理并上报，不得无限运行；
4. 回零请求必须显式覆盖电机 1 与电机 2；
5. 普通停止只停止正常蓄力目标，不得被判定为回零完成；
6. 任一电机提交失败 → 不得声称动作已开始或已完成，按联动故障处理并上报；
7. 重复命令按幂等或明确拒绝处理；完成后重复回零不产生第二次动作；
8. 非法命令（缺字段、非有限值、越界）→ 拒绝且不改变当前有效目标（`W-CMD-001`）。

### 6.3 验收判定表

| 场景 | 预期结果 |
|---|---|
| 授权失败 / 急停锁定 / 模式不允许 | 拒绝启动，不向任何电机提交目标 |
| 配置键缺失或非 OK | 拒绝，如实失败，不使用未校验值 |
| 正常启动 | 电机 1、2 均提交目标成功后才开始计时 |
| 单侧提交失败 | 停止另一侧，按故障处理，不上报完成 |
| X 秒前 / X 秒到期 | 不完成 / 完成 |
| 到期与最大动作时间同时到达 | 判完成，不判超时 |
| 达最大动作时间仍未完成 | 进入故障处理并上报 |
| 普通停止 | 双电机普通目标停止，状态为停止而非回零完成 |
| 回零请求 | 电机 1、2 均收到回零目标 |
| 单限位低 / 单限位无效 | 不回零完成 |
| 双限位稳定低 | 停止回零目标并置回零完成 |
| 重复命令 / 完成后重复回零 | 幂等或明确拒绝 |
| 查询 | 返回状态、目标、完成状态、最近错误，且不阻塞 |

---

## 7. 安全与仲裁接入

1. 普通蓄力/停止：`SubmitIntent(ControlIntent{kind = CONTROL, source = 控制源})` → 检查 `accepted`；再自行复核急停/故障/模式/停止发射等前置条件。
2. 回零：`SubmitIntent(kind = SAFETY_RECOVERY)`；`SystemMode == SOFTWARE_ESTOP` 阶段**禁止**提交；仅人工确认后的恢复阶段或对应一般恢复阶段允许。
3. 零力矩/停止出力类动作（如必要）：`IntentKind::SAFETY`。
4. `accepted == false` 不得当作成功，也不得据此改变业务状态。
5. 急停锁定/故障活动期间本模块自身拒绝普通命令（`RunGate()` 只是第二道防线）。
6. 每次外部触发只产生一次蓄力/回零动作；四发顺序与发次由 ATOM-13 编排。

---

## 8. 故障与上报

| 故障码 | 来源/触发 | 本模块动作 |
|---|---|---|
| `F-MOTOR-001` / `F-MOTOR-002` / `F-MOTOR-003` | ATOM-05 | 停止相关动作、保留错误、转报 |
| `F-ENC-001` | ATOM-05 | 同上 |
| `F-GPIO-001` | ATOM-06（限位读取失败/输入无效） | 停止双电机回零、保留错误、转报 |
| `F-LIMIT-001` | 非回零动作期间限位与运动方向冲突 | 停止相关电机、上报 |
| `W-CMD-001` | 本模块命令校验失败 | 拒绝命令、不改变有效目标 |
| `E-ESTOP-001` | 安全链路 | 只拒绝普通命令，不参与急停编排 |

上报经窄接口 `IFaultReporter`，由装配层委托 `IInputReporter::ReportFault(bool, FaultSeverity)`；禁止新增 `Config.md` §14.2 之外的故障码。

---

## 9. 遥测与查询面

- 只读快照：`state`、`target`（电机 1/2 目标速度）、`feedback`（可获得时）、`error_code`；
- 满足 `Config.md` §12.3 `mechanism_state.charge` 字段需求（ATOM-17 取用）；
- 限位/GPIO 状态由 ATOM-06 发布（`sensor_state`），本模块不重复发布；
- 查询接口无阻塞，可在控制线程调用。

---

## 10. CMake 现状

| 项 | 状态 |
|---|---|
| 目标 `atom_08_charge` + 别名 `Marshland::atom_08_charge` | 已有 |
| 无 `project()` / 无 `main()` / 无可执行文件 / 不设标准与选项 | 已有 |
| 有 `Src` 时 `STATIC`，否则 `INTERFACE`；`CONFIGURE_DEPENDS` | 已有 |
| 包含目录（本目录 / `Inc` / `../Public`） | 已有 |
| `Marshland::PublicHeaders` 链接块 | **本轮已补齐**（授权一次） |

上层 `NewVersion/CMakeLists.txt`、`Atoms/CMakeLists.txt` 未读取、未修改；子目录接入与注册由顶层装配方负责。

---

## 11. 裁决结果与剩余待裁决项

### 11.1 已裁决（原 §11 冲突项闭环）

| # | 原事项 | 裁决与落实 |
|---|---|---|
| 1 | 蓄力恒定速度配置键缺失 | **已落地**：`charge.charge_speed_rpm` = `charge.home_speed_rpm` = 5000；核对与待补见 §4.2 |
| 2 | `timing.charge_max_action_ms` 单位 | 裁决 30000 ms；**配置文件现值仍为 30**，见 §4.2-P1 |
| 3 | `gpio.limit_switch[].pin = null` | **暂不接入**；接口保留，`pin` 保持 `null`，禁止推断 |
| 4 | ATOM-08 无 `RuntimeStep` / 是否注册 | 外部注册由顶层设计 Agent 负责；本模块**提供 `ILifecycleModule` 适配器作为 ATOM 内注册接口**（§3.6） |
| 5 | `AtomIntegration.md` 与 Public 契约不一致 | **已按授权同步为 v1.4**（§0/§4.2/§4.3/§8/§11#7/表头/版本记录） |
| 6 | 本目录 CMake 缺 `PublicHeaders` 链接块 | **已按授权补齐**（§10） |
| 7 | 手动蓄力速度来源 | 先在 Public 内确定控制权归属（`IStateReader` → `control_source`），再按来源取速度：SBUS 经 `ISbusSnapshotReader`、WEB 经命令目标、AUTO 经 ATOM-13 目标或配置恒定速度 |
| 7b | SBUS 通道选择（R1） | **CH2 与 CH8/CH9 都要；CH2 非零时优先覆盖 CH8/CH9**；通道映射待补（§4.2-P3） |
| 8 | 根/本目录 `AGENTS.md` 职责表述不一致 | 以**本工作目录 `AGENTS.md`** 规定为准；不读取其他目录的 `AGENTS.md` |

### 11.2 R1～R4 裁决结果

| # | 事项 | 裁决 |
|---|---|---|
| R1 | SBUS 手动蓄力通道 | **两个都要**：CH2（整体）与 CH8（左）/ CH9（右）均使用；**CH2 非零时优先级更高，覆盖 CH8/CH9 指令** |
| R2 | 左右是否独立 | **需要**：模块内目标模型按左右独立设计；配置键是否拆分待定，见 §4.2-P4 |
| R3 | 回零超时 | **新增**，25 s = 25000 ms，**经 config 读取**（`timing.charge_home_max_action_ms`），见 §4.2-P2 |
| R4 | 回零判据键 | **合并**：只读 `gpio.charge_home_criterion`（`BOTH_LIMITS_LOW`），不新增 `charge.home_completion_criterion` |

**当前阻塞开工的待补配置**：§4.2 的 P1、P2、P3、P4（P4 为二选一确认）。

---

## 12. 验收与自检清单（实施后逐项确认）

- [ ] `Inc/atom08_charge/` 内不含任何 `ATOM-NN` 私有头，未包含 `AppCoordinator.hpp` / `SystemMonotonicClock.hpp`
- [ ] 未创建 `main()`、未创建可执行目标
- [ ] `LifecycleAdapter` 为唯一 `ILifecycleModule` 实现，独立 TU、≤ 50 行、无业务逻辑、`shutdown()` noexcept 且幂等
- [ ] `initialize()` 失败时如实返回 `success = false`，`error_message` 可定位
- [ ] 配置经 `IConfigView` 按 `Rule/ConfigSchema.md` 键名读取，检查了 `ConfigStatus`
- [ ] **未读取 `gpio.*`**：回零判据与限位状态全部来自注入的 `ILimitSwitchState`（选项 A）
- [ ] 未自解析 JSON；缺失项如实失败；`[待定]` 项保持未实现；无臆造默认值
- [ ] 未调用 `StateArbitrator` 任何 setter / `UpdateInputs()`；未注入具体 `AppCoordinator&`/`StateArbitrator&`
- [ ] 安全动作按类别提交：普通蓄力 `CONTROL`、回零 `SAFETY_RECOVERY`、零力矩 `SAFETY`
- [ ] 急停锁定阶段未用 `SAFETY_RECOVERY` 提交出力动作
- [ ] `accepted == false` 未被当作成功
- [ ] 急停锁定期间本模块自身拒绝普通蓄力命令（不依赖 `RunGate()`）
- [ ] 计时全部使用单调时钟；周期符合 `Config.md` §4
- [ ] 模块 CMake 提供 `atom_08_charge` + `Marshland::atom_08_charge` 与 `Marshland::PublicHeaders` 链接，未设全局选项
- [ ] 本模块 `PLAN.md` 已更新：接入点、所需配置键、`[待定]` 项、顺序依赖
- [ ] 仅在 WSL aarch64 交叉编译或目标 Ubuntu ARM 主机增量编译，`-Wall -Wextra -Wpedantic` 零警告

---

## 13. 实现与验证结果（2026-10-14）

| 项 | 结果 |
|---|---|
| 增量 ARM 交叉编译 | ✅ `cmake --build cmake-build-ubuntu-arm --target atom_08_charge`：3 个 `.cpp` 编译并归档为 `libatom_08_charge.a`（未全量 configure、未 clean） |
| 整体链接 | ✅ `--target marshland` 增量构建通过，`atom_08_charge` 参与构建，可执行文件链接成功 |
| 严格警告 | ✅ `aarch64-linux-gnu-g++ -std=c++23 -Wall -Wextra -Wpedantic -fsyntax-only` 单独编译 3 个 `.cpp`：**零警告** |
| 静态自检 | ✅ 无 `main()`、无 `AppCoordinator.hpp`/`SystemMonotonicClock.hpp`、无跨 ATOM 私有头、无 `gpio.*` 读取、无硬编码 CH 编号/电机 ID、无 JSON 解析 |
| 适配器规模 | ✅ `lifecycle_adapter.cpp` 独立翻译单元、31 行、无业务逻辑、`shutdown()` noexcept 且幂等 |
| 安全门控 | ✅ 普通蓄力/停止 = `CONTROL`、回零 = `SAFETY_RECOVERY`；`SystemMode == SOFTWARE_ESTOP` 或 `FAULT` 时自身拒绝并撤销出力；`accepted == false` 一律不当作成功 |
| 死锁防护 | ✅ `IStateReader::ReadState()` 的状态租约取快照后立即销毁，之后才调用 `SubmitIntent`/电机/限位端口 |

**自审后修复（2026-10-14，仅本模块内部缺陷）**

| 编号 | 缺陷 | 修法 |
|---|---|---|
| D-1 | `requester == NONE` 时"解析用控制源、提交用 NONE"不一致 → 必然被仲裁拒绝 | 引入 `effective_source`（requester 优先，NONE 回落当前控制源），解析与 `SubmitIntent` 使用同一值；`NONE` 直接返回 `RejectedNoControlAuthority` |
| D-3 | `tick` 取了 `fault_active` 却不用 → 故障已激活但 mode 未转 FAULT 时不撤销出力 | 改为 `must_revoke = SOFTWARE_ESTOP ‖ FAULT ‖ (fault_active && mode != RECOVERY)`；恢复阶段不打断 ATOM-14 的蓄力回零 |
| NB-1 | SBUS 三通道全 0 时"空转 X 秒后误报完成" | 解析结果为 (0, 0) 时返回 `RejectedTargetUnavailable`（蓄力与回零一致处理） |
| NB-3 | `ChargeStatusSnapshot::action` 易被误读为"当前动作" | 重命名为 `last_action`，并在类型注释中说明"最近一次动作" |
| NB-4 | `tick` 的调用周期未写明 | 头文件注明"期望由装配层按 20 ms 控制周期调用" |

重新验证：ARM 增量编译通过（`libatom_08_charge.a` 重建，仅重编 2 个改动单元）；`-Wall -Wextra -Wpedantic` 严格编译**零警告**。

**待外部裁决/接线（非本模块可修，已登记 `GeneralBugs/2026-10-14-ATOM-08.md`）**

- **HB-1（致命）**：`tick()` 无调用方 → 蓄力只启动不收敛；装配层必须指定 20 ms 周期调用者。
- HB-2：`clearVelocityTarget` 映射语义（0 rpm 保持 vs 零力矩）待裁决。
- HB-3：`ConfigReloadFailed` / `LimitInputInvalid` 是否走故障通道待裁决。
- HB-4：若注册生命周期适配器，步骤必须晚于 `LoadConfiguration` / `ValidateConfiguration`。
- D-2：`F-LIMIT-001`（非回零期间限位冲突）的检测与"方向冲突"语义归 ATOM-06 / ATOM-14；本模块已在 D-3 修复后对"故障激活且非恢复阶段"撤销出力。

**实现期新发现（已登记 `Atoms/Public/GeneralBugs/2026-10-14-ATOM-08.md`）**

- `Design/Config.md` §14.2 故障表**没有**"蓄力超时/回零超时"条目，而 `ATOM-08_Charge.md` 要求把蓄力超时报告给 ATOM-14；本实现因此只上报自定义类别枚举 `ChargeFaultCode`，交装配层/ATOM-14 映射，未新增表外故障码。
- 顶层 CMake 生成的编译命令**不含** `-Wall -Wextra -Wpedantic`（与 `Rule/AtomIntegration.md` §9.3 不符）；本轮以独立严格编译补验。

**未完成/受限项**

- ATOM-06 未实现：`ILimitSwitchState` 需装配层提供实现或测试替身才能联调。
- 装配层（`Entry.cpp`）未接入：对象图、注册步骤与 `required` 由顶层装配方负责（§3.2、§3.6）。

---

## 14. 本轮修改范围

| 文件 | 操作 |
|---|---|
| `Atoms/ATOM-08_Charge/PLAN.md` | 更新（本文） |
| `Atoms/ATOM-08_Charge/CMakeLists.txt` | 补齐 `Marshland::PublicHeaders` 链接块（授权一次） |
| `Rule/AtomIntegration.md` | 现实同步至 v1.4（授权一次） |
| `Inc/atom08_charge/*.hpp`（5 个） | 新建（本轮实现） |
| `Src/charge_config.cpp`、`Src/charge_controller.cpp`、`Src/lifecycle_adapter.cpp` | 新建（本轮实现） |
| 其他 ATOM、`Design/`、`Atoms/Public/`（除 GeneralBugs 文档）、`Config/`、上层 CMake | **未修改** |

---

## 15. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | — | 初版（基于 ATOM-08 规划文件与旧版公共接口假设）。 |
| 2.0 | 2026-10-14 | 依据 `AtomIntegration.md` v1.3、`ConfigSchema.md` v1.4、`Design/Config.md` 1.1、`Design/Interface.md`、`Atoms/Public` 契约与 `ATOM-03_SBUS` 样例重写，并登记 8 项待确认/冲突。 |
| 2.1 | 2026-10-14 | 落实用户 8 项裁决：新增 §3.5（先定控制权再按来源取速度）、§3.6（ATOM 内注册接口）、§4.2（配置项需求清单，提交配置维护方）、§11（裁决闭环与剩余 R1–R4）；`timing.charge_max_action_ms` 定为 30000 ms；限位暂不接入；CMake 补齐 `PublicHeaders`；`AtomIntegration.md` 同步 v1.4。**本轮未创建或修改任何实现代码。** |
| 2.2 | 2026-10-14 | 核对已落地的 `charge` 配置并落实 R1～R4：§3.5 增列 CH2 优先覆盖 CH8/CH9 的读取规则与左右独立设计；§4.1 换为真实键与现值；§4.2 改为"配置落地核对与待补项"（P1 超时值 30→30000、P2 新增回零超时 25000、P3 sbus.functions 补 CH8/CH9 映射、P4 左右独立键二选一、target_range 可选）；§6.2 增补回零超时判据；§11.2 记录 R1～R4 裁决。**仍未创建或修改任何实现代码。** |
| 2.4 | 2026-10-14 | 完成实现：新增 `Inc/atom08_charge/` 5 个头与 `Src/` 3 个源（领域类型、窄接口、配置读取、纯逻辑内核、ATOM 内注册适配器）；`sbus.functions[]` 通道映射改为配置驱动（§4.1）；新增 §13 实现与验证结果（ARM 增量编译、严格警告零警告、静态自检）；§5 记录实际文件结构。**代码已实现，未修改其他模块。** |
| 2.5 | 2026-10-14 | 自审并修复本模块内部缺陷：D-1（effective_source 一致性）、D-3（故障激活即撤销出力，恢复阶段除外）、NB-1（全零目标不再误报完成）、NB-3（`last_action` 重命名）、NB-4（tick 周期注释）；§13 记录修复与重新验证结果、外部待裁决项（HB-1～HB-4、D-2）。**未修改其他模块。** |
| 2.3 | 2026-10-14 | 按用户选项 A 定稿：§1.2/§4.1 明确 ATOM-08 **不读取 `gpio.*`**，限位与回零判据全部经窄端口 `ILimitSwitchState` 由 ATOM-06 提供；`gpio` 段缺失不再视为 ATOM-08 阻塞；§4.2 更新核对结论与已落地键表；§12 自检清单增设"未读取 `gpio.*`"项。**仍未创建或修改任何实现代码。** |