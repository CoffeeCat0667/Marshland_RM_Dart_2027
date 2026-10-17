# ATOM-07_Servo_Driver 实现方案（PLAN）

> 状态：**已实施代码，未编译**。本目录现有 `CMakeLists.txt`、`PLAN.md`、`AGENTS.md`、`Lib/`（供应商库参考副本）、`Inc/atom07_servo_driver/`（9 个头文件）、`Src/`（6 个源文件）。
> 本轮（2026-10-14）：按用户授权实施实现代码，并修正本目录 `CMakeLists.txt`（补 `Marshland::PublicHeaders` 链接）。**未执行任何编译**（见 §13.4）。
> 同步基线：PRD 0.10、PEF 1.5、Interface 1.5/1.6、Config 0.8、Atoms 0.11、`Rule/AtomIntegration.md` 1.3、`Rule/ConfigSchema.md` 1.4。
> 验证方式：静态检查与形式化判定；本阶段不编译。

---

## 0. 本轮变化

### 0.1 规则变化落点（第一轮）

| 规则变化 | 对本模块的影响 | 本文落点 |
|---|---|---|
| 配置读取统一走 `marshland::config::IConfigView`（`Atoms/Public/IConfigView.hpp`），装配层构造期注入，必须检查 `ConfigStatus`，键名以 `Rule/ConfigSchema.md` 为唯一来源，热加载看 `configVersion()` | 原计划"Config 提供设备/地址/频率/通道/角度"改为逐键查询并逐项判错 | §4 |
| 模块结构固定为"纯逻辑内核 + 生命周期适配器"两层；适配器是唯一 `ILifecycleModule` 实现、独立翻译单元、≤ 50 行、无业务逻辑 | 原计划未显式分层，现固定文件划分与职责 | §3、§5 |
| 注册步骤固定为 `RuntimeStep::InitializeIicPca9685`；顺序依赖 ATOM-07 → ATOM-10/11 | 原计划未写明注册步骤 | §2.3 |
| `Atoms/Public` 新增 `IConfigView.hpp`、`IMonotonicClock.hpp`、`AppInterfaces.hpp`（`IInputReporter` / `IControlAuthority` / `ISafetyStateControl` / `IStateReader`） | 时间与安全状态无需依赖 `AppCoordinator` 私有头 | §2.4、§2.5 |
| CMake 契约：目标 + `Marshland::` 别名、不定义 `project()`、应链接 `Marshland::PublicHeaders` | 原文件缺 `PublicHeaders` 链接块 | §2.2（本轮已修复） |

### 0.2 本轮用户裁决（已确认，直接落库）

| # | 裁决 | 落点 |
|---|---|---|
| 1 | 授权修正本目录 `CMakeLists.txt` | §2.2（**已执行**） |
| 2 | `Rule/ATOM7ConnectProject.md` 暂时不编写 | §11 #11 |
| 3 | 授权参考供应商库调库方式；`PCA9685A` 库已复制到本目录 `Lib/` | §5.3（**已阅读评估**） |
| 4 | OE 默认使能（硬件固定拉低），本模块不管 OE | §4.1 |
| 5 | 舵机参数归口 `servo.channels[]`；供弹舵机 = 通道 **4**，发射舵机 = 通道 **5** | §4.2、§4.5 |
| 6 | 授权读实际代码确认安全端口是否可用 | §2.5（**已确认可用**） |
| 7 | 不新增 `Atoms/Public` 舵机类型；由 ATOM-10/11 声明窄接口、装配层适配 | §2.4、§3 |

---

## 1. 目标与边界

### 1.1 单一职责（依据 `Design/Atomic/ATOM-07_Servo_Driver.md`）

把一个逻辑舵机目标映射为一个配置通道输出，并处理 PCA9685A 初始化、就绪查询和 IIC 错误。本模块**不决定哪一流程应移动舵机**。

### 1.2 内部原子操作（10 项，与规格逐条对应）

1. 通过配置 IIC 设备和 PCA9685A 地址打开设备。
2. 调用驱动初始化并读取驱动就绪状态。
3. 设置 Config 指定的 PWM 频率。
4. 校验一个舵机目标是否落在该舵机配置范围内。
5. 将一个角度转换为一个 PWM/脉宽输出目标。
6. 向单个 PCA9685A 通道写入一个目标。
7. 查询一个舵机最近接受的目标值；**不得**将目标值冒充物理反馈。
8. 对一次 IIC 错误发布故障结果。
9. 急停/通信异常要求保持时不下发新的普通角度目标。
10. 软件急停拒绝 FireController 的目标命令；换弹为双 M3508 电机，不通过本 Atom 执行抬起/放下。

### 1.3 明确不负责

- 不决定舵机运动流程、四发流程、控制权仲裁与发射授权（属 ATOM-02/13）。
- 不实现供弹动作序列（ATOM-10）、发射动作序列（ATOM-11）、安全恢复编排（ATOM-14）。
- 不管理 OE 引脚（本轮裁决：OE 默认使能、硬件固定）；不实现限位/GPIO（属 ATOM-06）。
- 不定义 `main()`，不创建可执行文件，不自行注册生命周期。
- 不解析 JSON 配置，不臆造 `[待定]` 值。
- 不向 `Atoms/Public` 新增舵机公共类型（本轮裁决 #7）。

### 1.4 与架构设计的一致性检查

- `Design/Atoms.md` ATOM-07 范围与本文一致。
- `Design/Interface.md` §12.1～§12.3 与本文一致；`FeedOnce()` / `FireOnce()` / `HoldCurrentServoAngles()` / `RejectFireServoCommand()` 由 ATOM-10/11 编排，本模块只提供"目标角度 → PWM 通道"能力。
- `Design/Config.md` §9 与本文 §4 键表一致（通道 4/5 为本轮用户裁决值，见 §4.5 落库要求）。
- **未发现超出设计范围的需求**；剩余冲突/待确认项统一登记在 §11，本模块不自行裁决。

---

## 2. 依赖方向与接入契约（`Rule/AtomIntegration.md`）

### 2.1 依赖方向（不可协商）

```text
ATOM-07_Servo_Driver  →  Atoms/Public（IConfigView.hpp / ILifecycle.hpp / IMonotonicClock.hpp /
                          AppInterfaces.hpp / MarshlandTypes.hpp）+ C++ 标准库 + 目标平台 Linux IIC
```

- **禁止**包含 `AppCoordinator.hpp` 或任何其他 ATOM 的 `Inc/` 私有头。
- **禁止**调用 `StateArbitrator` 的字段 setter 或 `UpdateInputs()`。
- **禁止**在模块内定义 `main()`、创建可执行文件、设置架构/编译器/C++ 标准/编译选项。

### 2.2 CMake 接入

```text
marshland executable
  -> Atoms/Public/AppCoordinator（顶层装配）
  -> Marshland::atom_07_servo_driver
  -> Marshland::PublicHeaders
```

1. 目标名 `atom_07_servo_driver`，别名 `Marshland::atom_07_servo_driver`（已满足）。
2. 不定义 `project()`，不设全局编译选项（已满足）。
3. 有 `Src/` 源码时建 `STATIC` 库，否则 `INTERFACE`（已按此分支编写）。
4. 链接 `Marshland::PublicHeaders`（**本轮已补齐**，与 ATOM-03 一致）。

### 2.3 生命周期注册

- 注册步骤：`RuntimeStep::InitializeIicPca9685`。
- 顺序依赖：**ATOM-07 必须先于 ATOM-10 / ATOM-11**。
- 注册只能在装配层完成：显式 `required`、检查 `addModule()` 返回值、失败读取 `lastRegistrationError()`。
- 内核对象寿命**必须**长于其生命周期适配器（适配器只持引用）。

### 2.4 契约面

| 契约面 | 本模块角色 | 依据 |
|---|---|---|
| 生命周期面 | 实现 `runtime::ILifecycleModule`（仅 `LifecycleAdapter`） | AtomIntegration §2、§4.1 |
| 输入上报面 | 不上报外部输入；IIC 故障的 `ReportFault` 最终由装配层委托（见 §11 #4） | §4.2 |
| 仲裁面 | **不提交** `ControlIntent`；普通/安全动作意图由 ATOM-10/11/14 提交 | §4.3 |
| 执行器拒绝面 | **本模块自身必须拒绝**普通角度目标：急停锁存、通信异常、未就绪时不下发 | §4.2 "重要边界" |
| 对外能力面 | 只导出本模块内核 API（`ServoCore` 等）；ATOM-10/11 各自声明窄接口，装配层适配 | 裁决 #7、AtomIntegration §6.2 |

### 2.5 安全状态端口（本轮已读实际代码确认）

已确认 `Atoms/Public/AppInterfaces.hpp` 中：

- `app::IStateReader` 为**公共**抽象，`AppCoordinator` 实现它（`AppCoordinator.hpp` 第 52 行 `StateReadView ReadState()`）。
- `StateReadView` 暴露 `run_gate()` / `hardware_ready()` / `estop_latched()` / `context()` / `inputs()`，并以 `unique_lock` 持有读租约。
- `app::IMonotonicClock` 为公共时钟契约（`IMonotonicClock.hpp`）。

因此拒绝面**可以**落地为：装配层注入 `const app::IStateReader&` 与 `app::IMonotonicClock&`；本模块在**访问 IIC 之前**短时取得租约、读取 `estop_latched()` / `run_gate()` / `hardware_ready()`，随即**释放租约**再访问总线。

使用约束（来自 `AppInterfaces.hpp` 注释与 `AtomIntegration` §4.2、§8）：

- 持租约期间**禁止**任何 I/O 或端口调用（含 IIC 与日志）。
- 租约不得超出本次判定存活；不得跨线程保存 `StateReadView` 或 `AppCoordinator` 引用。
- 仍**禁止**包含 `AppCoordinator.hpp`、`RuntimeCoordinator.hpp`、`StateArbitration.hpp` 私有头。
- `Rule/AtomIntegration.md` §8"公共时钟头尚未提取"与 §11 #7"端口尚未落地"的表述已滞后，按实际公共契约实现并登记（§11 #9）。

---

## 3. 目录结构（计划）

```text
ATOM-07_Servo_Driver/
├── CMakeLists.txt
├── PLAN.md
├── Lib/                             # 供应商库副本：仅作参考，不参与编译、不加入模块目标
│   ├── PCA9685A.hpp
│   └── PCA9685A.cpp
├── Inc/
│   └── atom07_servo_driver/
│       ├── servo_types.hpp          # 舵机编号、通道、角度、WriteResult、ServoFault
│       ├── servo_config.hpp         # IConfigView → 本模块配置视图（逐键校验）
│       ├── i2c_bus.hpp              # II2cBus 窄接口（带超时、返回状态、可测试替身）
│       ├── pca9685_registers.hpp    # 寄存器常量与位定义
│       ├── pca9685_device.hpp       # 初始化/预分频/通道写/回读（内核）
│       ├── angle_to_pulse.hpp       # 角度 → 脉宽 → 计数值（纯函数）
│       ├── servo_core.hpp           # 组合：配置、设备、通道映射、目标缓存、故障、串行化、拒绝面
│       └── lifecycle_adapter.hpp    # 唯一 ILifecycleModule 实现声明
└── Src/
    ├── servo_config.cpp
    ├── pca9685_device.cpp
    ├── angle_to_pulse.cpp
    ├── servo_core.cpp
    ├── linux_i2c_bus.cpp            # II2cBus 的 Linux 实现（/dev/i2c-N）
    └── lifecycle_adapter.cpp        # ≤ 50 行，只做编排与结果翻译
```

- 内核只依赖 `Atoms/Public` 与标准库；设备访问在 `II2cBus` 之后。
- `Lib/` **不进入**模块目标：CMake 只从 `Src/` 收集源文件，`Lib/PCA9685A.cpp` 不会被编译（理由见 §5.3）。
- 不新建本目录 `AGENTS.md`，不读取任何子目录 `AGENTS.md`。

---

## 4. 配置接入（`Rule/ConfigSchema.md` 1.4 §2.10、`Design/Config.md` §9）

查询方式：`IConfigView::getInt/getDouble/getBool/getString/getArraySize/getIntAt/getDoubleAt`，逐键检查 `ConfigStatus`；非 `OK` 一律不得继续使用该值（`out` 在非 OK 时保持原值，禁止当默认值用）。热加载：比对 `configVersion()`，变化时重新读取；读取失败保留旧有效值并记录。

### 4.1 必需键（本模块只读）

| 键 | 类型（初值） | 用途 | 失败处理 |
|---|---|---|---|
| `servo.pca9685.i2c_device` | str（`/dev/i2c-7`） | 打开 IIC 设备 | 缺失/非法 → 初始化失败 |
| `servo.pca9685.address` | str（`"0x40"`） | 器件地址（解析为 7 位） | 解析失败 → 初始化失败 |
| `servo.pca9685.pwm_frequency_hz` | int（50） | 设置 PWM 频率 | 非 OK → 初始化失败 |
| `servo.pca9685.channel_count` | int（16） | 通道数上界 | 与 `channel_range` 不一致 → 初始化失败 |
| `servo.pca9685.pwm_resolution_bits` | int（12） | 计数值上限 4096、脉宽换算 | 非 OK → 初始化失败 |
| `servo.pca9685.channel_range` | array of int（`[0,15]`） | 通道合法性 | 非 OK → 初始化失败 |
| `servo.pca9685.oe_active_level` | enum str（`LOW`） | **仅校验**；本模块不驱动 OE（裁决 #4：OE 硬件默认使能） | 非 OK → 初始化失败；不产生 GPIO 访问 |
| `servo.pca9685.access_mode` | str（`SERIALIZED`） | 访问串行化要求 | 非 `SERIALIZED` → 初始化失败（不降级） |
| `servo.pca9685.error_handling` | str（`ENTER_FAULT`） | IIC 错误 → 进入 `IIC_FAULT` 并发布故障 | 非 OK → 初始化失败 |
| `servo.model.pulse_min_us` | int（500） | 脉宽下限 | 非 OK 或 ≥ 上限 → 初始化失败 |
| `servo.model.pulse_max_us` | int（2500） | 脉宽上限 | 非 OK → 初始化失败 |
| `servo.model.angle_range_deg` | array of int（`[0,180]`） | **角度校验唯一来源**（元素无 per-servo 范围字段） | 非 OK → 初始化失败 |
| `timing.iic_comm_timeout_ms` | int（200） | IIC 事务超时判定（单调时钟） | 非 OK → 初始化失败 |

### 4.2 舵机资源数组 `servo.channels[]`（唯一归口，裁决 #5）

元素键：`id`（int）、`mechanism`（enum `RELOAD` / `FEED` / `FIRE`）、`channel`（int 或 null）、`standby_deg`（int 或 null）、`action_deg`（int 或 null）。

已裁决映射：

| 机构 | mechanism | 通道 |
|---|---|---|
| 供弹舵机 | `FEED` | **4** |
| 发射舵机（x 号） | `FIRE` | **5** |
| 换弹 | `RELOAD` | 不使用舵机（双 M3508，ATOM-05/04） |

规则：

- 映射**必须**从 `servo.channels[]` 读取（按 `mechanism` 定位、以 `id` 标识），**禁止**把 4/5 硬编码为逻辑常量；4/5 只作为配置数据的预期值用于判定表与验证。
- `channel` 为 `null` 表示"未接入"：**禁止**推断，初始化或写目标必须如实失败（`ChannelNotWired`）。
- `standby_deg` / `action_deg` 属**动作编排参数**，由 ATOM-10/11 使用；ATOM-07 只接受调用方给出的目标角度。
- 若出现 `mechanism = RELOAD` 元素：本模块不为其产生 PWM 输出（登记为遗留字段，不报错、不接受写目标）。

### 4.3 策略键（本模块只做"拒绝/保持"判定，不做流程编排）

| 键 | 取值 | 本模块行为 |
|---|---|---|
| `servo.strategy.estop_fire_servo` | `REJECT_ALL` | 软件急停期间拒绝发射舵机一切命令 |
| `servo.strategy.estop_other_servos` | `HOLD` | 急停期间其他舵机保持当前角度 |
| `servo.strategy.comm_fault_servos` | `HOLD` | 通信异常期间保持当前角度，不主动改变 |
| `servo.strategy.recovery_other_servos` | `HOLD` | 自动恢复三次发射期间非发射舵机角度不变 |
| `servo.strategy.fire_servo_safe_hold` | `HOLD_CURRENT` | 发射舵机安全保持 = 保持当前角度 |
| `servo.strategy.out_of_range_action` | `REJECT_AND_WARN` | 越界：拒绝下发、不改当前有效目标、记录 `W-CMD-001` |
| `servo.strategy.action_failure_action` | `FAIL_AND_BLOCK_FIRE` | 失败：本次判失败并阻止后续发射，记录 `F-SERVO-001` |

### 4.4 显式不读取的键

`servo.pca9685.task_priority`、`thread.iic_access`、`thread.iic_priority` 属调度配置，由上层/装配层落实；本模块只保证"串行化访问"这一事实。

### 4.5 配置落库要求（阻塞 Coding 的数据项）

`Rule/ConfigSchema.md` §2.10.3 目前仍写"`channel` 为 `null`（`[待定]`）"。本轮裁决已给出通道 4/5，需由配置维护方同步：`Design/Config.md` §9.2、`Rule/ConfigSchema.md`（含 `ConfigSchema.schema.json`）与运行时 `Config/dart_control.json`。**本模块不修改这些文件**；数据未落库时初始化会如实失败（`ChannelNotWired`），不会退化为猜测值。

---

## 5. 内核与适配器设计

### 5.1 纯逻辑内核

- `Pca9685Registers`：寄存器地址与位定义常量（`MODE1` / `MODE2` / `PRE_SCALE` / `LED0_ON_L` / `ALL_LED`）。
- `II2cBus`：写寄存器、读寄存器、连续写四字节的窄接口，全部返回显式状态并受单调时钟超时约束；由 Linux 实现（`/dev/i2c-N` + `ioctl(I2C_SLAVE)`）或测试替身提供。
- `Pca9685Device`：`initialize()`（写 `MODE1` / `MODE2` / `PRE_SCALE`，必要时回读校验）、`setPwmFrequency(hz)`、`writeChannelPulse(channel, off_ticks)`、`isReady()`。所有步骤返回成功/失败，**禁止**静默忽略写失败。
- `angleToPulse(angle, pulse_min_us, pulse_max_us, angle_range, resolution_bits, frequency_hz)` → `ticks`：纯函数；越界返回错误（**不**隐式钳位，以符合 `REJECT_AND_WARN`）。
- `ServoCore`：持有配置视图、设备、通道表（`mechanism → channel`）、最近成功目标缓存、最近 IIC 故障、拒绝面输入。对外 API：
  - `bool initialize()` / `bool isReady() const`
  - `WriteResult writeTarget(servo_id, angle_deg)`
  - `std::optional<double> lastAcceptedTarget(servo_id) const`（目标值语义，非反馈）
  - `ServoFault lastIicFault() const` / `void stop() noexcept`
- 串行化：`std::mutex` 覆盖"总线事务 + 目标缓存更新"；同一总线只允许经本类访问。
- 拒绝面：写目标前先取 `IStateReader` 租约读取 `estop_latched()` / `run_gate()` / `hardware_ready()`，释放租约后再做 IIC；不放行则返回 `EstopRejected` / `NotReady` 且**不产生 IIC 写**。

### 5.2 生命周期适配器（薄壳，独立翻译单元，≤ 50 行）

```cpp
// Inc/atom07_servo_driver/lifecycle_adapter.hpp
class LifecycleAdapter final : public runtime::ILifecycleModule {
public:
    LifecycleAdapter(ServoCore& core, const config::IConfigView& config) noexcept;
    runtime::ModuleInitResult initialize(const runtime::RuntimeContext& context) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override { return "atom07-servo-driver"; }
private:
    ServoCore& core_;
    const config::IConfigView& config_;
};
```

六条约束：① 如实反映成败；② `error_message` 可定位到键名/设备/调用；③ `shutdown()` `noexcept`、幂等、对未初始化安全；④ 只经 `IConfigView` 按 ConfigSchema 键名读取，缺失/非法如实失败；⑤ 不判断机构流程、控制权、发射授权；⑥ 不内置线程业务循环。

### 5.3 驱动实现路线与供应商库评估（本轮已读 `Lib/PCA9685A` 与原始 main.cpp）

供应商库**参考价值**：IIC 打开与 `ioctl(I2C_SLAVE)` 方式；`MODE1` 自动增量；`MODE2` 推挽；`PRE_SCALE` 标准写时序（进入 SLEEP → 写 prescale → 清 SLEEP → 500 us → RESTART）；`LEDn_ON_L` 四寄存器布局；`ALL_LED` 统一写；SWRST 通用呼叫写 0x06。

**不能直接满足本模块规格的差距**：

| 差距 | 供应商库现状 | 本模块要求 |
|---|---|---|
| 错误上报 | 公开写接口全返回 `void`，私有 `writeReg()` 的 `bool` 被丢弃，无"最后错误"查询 | 第 6/8 项要求返回成功/失败、IIC 错误可查询 |
| 越界处理 | `setServoAngle()` 静默钳位 0～180°，写死 0.5～2.5 ms | `out_of_range_action = REJECT_AND_WARN`：拒绝下发、不改当前目标 |
| 角度/脉宽来源 | 硬编码 500～2500 us、0～180° | 必须用 `servo.model.pulse_min_us` / `pulse_max_us` / `angle_range_deg` |
| 超时 | 阻塞 read/write，无超时 | `timing.iic_comm_timeout_ms` = 200 ms 判定 |
| 输出副作用 | 直接向 stdout 打印（频率、寄存器转储） | 控制路径不得同步阻塞/污染日志 |
| 复位范围 | `reset()` 切到 0x00 通用呼叫，会复位同总线所有 PCA9685 | 需明确是否允许（§11 #7） |
| 串行化 | 无锁 | `access_mode = SERIALIZED` |

**结论**：不链接、不包装该库作为实现；改为自研 `Pca9685Device` + `II2cBus`（带状态与超时），只复用其寄存器序列与时序知识。`Lib/` 保持参考副本，不进模块目标。

---

## 6. 状态与错误处理

| 状态 | 含义 | 退出条件 |
|---|---|---|
| `UNINITIALIZED` | 尚未初始化或初始化失败 | 成功 `initialize()` |
| `READY` | 设备就绪，可接受普通目标 | IIC 故障 / 急停锁存 |
| `IIC_FAULT` | 读写失败或超过 `timing.iic_comm_timeout_ms`（`error_handling = ENTER_FAULT`） | 上层故障恢复流程明确复位；故障事实可查询 |
| `ESTOP_LOCKED` | 软件急停锁存（拒绝面） | 仅上层安全流程解除 |

错误枚举：`ConfigMissing` / `ConfigInvalid` / `ChannelNotWired` / `ChannelInvalid` / `AngleOutOfRange` / `NotReady` / `EstopRejected` / `IicError` / `Timeout`。**禁止**用异常吞掉硬件错误。故障信息含：错误类别、舵机/通道、底层 errno 或状态、发生时刻（单调时钟）。

---

## 7. 实施步骤

1. ~~确认公共契约边界~~（已完成：`IConfigView` / `IMonotonicClock` / `IStateReader` 均确认可用；不新增 Public 类型）。
2. 冻结 `servo_types.hpp` / `servo_config.hpp` 与逐键校验规则（含 `null` 未接入、`mechanism → channel` 映射）。
3. 实现 `angle_to_pulse` 纯函数 + `Pca9685Registers` 常量（先做形式化边界判定）。
4. 实现 `Pca9685Device`（经 `II2cBus`，全部返回状态）+ Linux `II2cBus` 后端（超时处理）。
5. 实现 `ServoCore`：串行化、目标缓存、故障结果、拒绝面（租约内不 I/O）。
6. 实现 `LifecycleAdapter`（≤ 50 行）。
7. 静态自检（§10）与形式化判定（§8）；在获准环境按 §9 增量编译。
8. 与 ATOM-10/11 的窄接口对齐。

---

## 8. 验收判定表

| 场景 | 预期结果 |
|---|---|
| 必需键缺失 / 类型错误 / 越界 | `initialize()` 返回 `success = false`，`error_message` 指明键名与原因 |
| `FEED` 舵机 `channel` = 4 | 初始化成功并登记通道 4；写目标作用于通道 4 |
| `FIRE` 舵机 `channel` = 5 | 初始化成功并登记通道 5；写目标作用于通道 5 |
| `channel` 为 `null` | `ChannelNotWired`；不推断、不产生 IIC 写 |
| 合法角度（全局范围 0～180 内） | 成功；计数值落在 `pulse_min_us`～`pulse_max_us` 映射区间；最近目标更新 |
| 角度越界 | `AngleOutOfRange`；记录 `W-CMD-001`；当前有效目标**不变**；不产生 IIC 写 |
| 通道号不在 `channel_range` | `ChannelInvalid`；不产生 IIC 写 |
| IIC 写失败（errno / 短写） | `IicError`；故障可查询；按 `ENTER_FAULT` 进入 `IIC_FAULT` |
| IIC 事务超时 | `Timeout`；故障可查询；不静默成功 |
| 急停锁存，写发射舵机 | `EstopRejected`；**不下发** |
| 急停锁存，写其他舵机 | `EstopRejected`；保持当前角度，**不主动**改变 |
| 通信异常期间 | 不主动改变角度；不接受新的普通目标 |
| 自动恢复三次发射期间，非发射舵机 | 角度保持不变 |
| 并发写多通道 | 串行化：事务不交错；缓存一致 |
| 查询"最近目标值" | 目标值语义，**不**声称物理反馈 |
| 未初始化即写目标 | `NotReady`；不访问 IIC |
| `shutdown()` 重复调用 / 从未 `initialize()` | 安全、幂等、不崩溃 |
| 持 `StateReadView` 租约期间 | 无 IIC、无日志、无其他端口调用 |
| 静态检查 | 无 `main()`；头文件在 `Inc/`；实现在 `Src/`；无跨 ATOM 私有头；无 `AppCoordinator.hpp`；`Lib/` 未被编译 |

---

## 9. 构建与验证方式（`Rule/AtomIntegration.md` §9）

- 目标平台 Ubuntu ARM；**禁止** x86 编译链；只允许 WSL aarch64 交叉编译或目标机编译。
- **必须**增量编译；**禁止**删除构建目录、**禁止** `--clean-first`。
- 日常命令：`cmake --build cmake-build-ubuntu-arm --target marshland -j`。
- **必须**在 `-Wall -Wextra -Wpedantic` 下零警告。
- 模块不独立编译，**不得**为本模块创建测试用 `main()`；行为验证依赖 §8 判定表与上层集成验证。
- 本轮（截至 2026-10-14）仍未编译。

---

## 10. 编码前置自检清单

- [ ] `Inc/` 仅包含 `Atoms/Public` 与标准库；`Src/` 实现独立翻译单元；
- [ ] 未包含 `AppCoordinator.hpp` / `RuntimeCoordinator.hpp` / `StateArbitration.hpp` / 其他 ATOM 私有头；
- [ ] `ILifecycleModule` 由独立翻译单元实现，该文件 ≤ 50 行、无业务逻辑；
- [ ] `initialize()` 失败如实返回并给出可定位 `error_message`；
- [ ] `shutdown()` `noexcept`、幂等、对未初始化安全；
- [ ] 配置经 `IConfigView` 按 `ConfigSchema.md` 键名读取，逐项检查 `ConfigStatus`；
- [ ] 通道映射来自 `servo.channels[]`，代码内无 4/5 硬编码；
- [ ] 无自解析 JSON、无臆造默认值、`null` 项未被填值；
- [ ] 未注入具体 `AppCoordinator` / `StateArbitrator`；安全事实仅经 `IStateReader` 租约读取且租约内无 I/O；
- [ ] 急停锁存与通信异常期间，本模块自身拒绝普通角度目标；
- [ ] 目标值与物理反馈在类型/命名上严格区分；
- [ ] 单调时钟；IIC 超时按 `timing.iic_comm_timeout_ms`；
- [ ] IIC 访问串行化；所有总线调用返回状态，无静默失败；
- [ ] `Lib/` 未加入模块目标（`Lib/PCA9685A.cpp` 未编译）；
- [ ] `CMakeLists.txt` 提供目标 + `Marshland::` 别名、无 `project()`、无全局编译选项、链接 `Marshland::PublicHeaders`；
- [ ] aarch64 增量编译通过且 `-Wall -Wextra -Wpedantic` 零警告；
- [ ] `PLAN.md` 已更新接入点、配置键、未接入项与顺序依赖。

---

## 11. 待确认事项（不自行裁决）

| # | 事项 | 影响 | 需要谁决定 |
|---|---|---|---|
| 1 | **配置数据缺口（阻塞启动）**：`Config/dart_control.json` 已含 `servo.channels[]`（FEED id=5 channel=4；FIRE id=null channel=5），但**缺少** `servo.pca9685.*`、`servo.model.*` 与 `timing.iic_comm_timeout_ms` 三组键（全仓检索 0 命中）→ 本模块会如实返回初始化失败，不会猜测默认值 | 能否启动 | 配置维护方（本模块不修改 `Config/`、`Design/`、`Rule/`） |
| 2 | **逐舵机角度范围**：元素无范围字段，只能统一用 `servo.model.angle_range_deg`（`[0,180]`） | 越界判定口径 | 设计/配置维护方 |
| 3 | **IIC 超时语义**：200 ms 是"单次事务超时"还是"连续无响应判定"？是否 N 次失败才 `ENTER_FAULT` | `Timeout` 与 `IIC_FAULT` 触发条件 | 设计/安全负责人 |
| 4 | **故障发布路径**：ATOM-07 是否注入 `app::IInputReporter` 直接 `ReportFault`，还是只提供可查询故障、由装配层转发 | `AppInterfaces.hpp` 使用方式 | 装配层/规则维护方（`AtomIntegration` §4.2） |
| 5 | **故障码归属**：`W-CMD-001` / `F-SERVO-001` 由谁记录（ATOM-18？）。ATOM-07 是否需注入诊断/日志端口 | 是否新增依赖 | 安全/诊断负责人 |
| 6 | **线程模型**：`Config.md` §15 提到"IIC 任务优先级 中高；安全动作队列高于普通舵机队列"。ATOM-07 是"调用方在控制周期内同步调用"，还是需要独立 IIC 线程/队列（谁创建、注册到哪个 `RuntimeStep`） | 内核是否含线程 | 装配层 / ATOM-01 负责人 |
| 7 | **SWRST 与回读校验**：初始化是否允许 SWRST（会复位同总线所有 PCA9685）？是否要求 `MODE1` / `PRE_SCALE` 回读校验 | 初始化流程 | 硬件/设计负责人 |
| 8 | ~~编译验证范围~~ **已裁决（用户 2026-10-14）**：本轮不做编译验证，接入就绪性由静态检查与接口核对确认 | — | 已关闭 |
| 9 | 规则文档滞后项：`AtomIntegration.md` §8（时钟头已提取）、§11 #7（端口状态） | 文档一致性 | 规则维护方 |
| 10 | **与 ATOM-10/11 的窄接口形态**：他们声明什么能力（写目标/查就绪/查故障），本模块内核 API 需与之对齐 | 接口冻结 | ATOM-10/11 负责人 + 装配层 |
| 11 | `Rule/ATOM7ConnectProject.md` 本轮暂不编写（裁决 #2） | 接入说明缺失 | 用户后续决定 |
| 12 | ~~`array<int>` 读取约定~~ **已解决（ATOM-15 2026-10-14 回复）**：按 `getIntAt(key, index, "")` 读取即可，`""` 与 `nullptr` 均兼容，ATOM-15 侧无需改动 | — | 已关闭 |
| 13 | **故障发布代码冻结**（用户 2026-10-14）：暂不继续编写故障发布相关代码，已有 `ReportFault` 调用保留、不扩展 | 后续增量范围 | 用户后续解冻 |

---

## 12. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.1 | 2026-10-10 | 首版：目标与边界、目录与构建、接口设计初稿、状态与错误处理、实现步骤与验收清单。 |
| 0.2 | 2026-10-14 | 按最新规范重写：`AtomIntegration` 1.3（`IConfigView`、内核+适配器、`InitializeIicPca9685`、窄端口与安全动作分类、单调时钟、构建与自检）；`ConfigSchema` 1.4 §2.10 键表；验收判定表；一致性检查；未决事项。仅更新文档。 |
| 0.3 | 2026-10-14 | 落入本轮用户裁决：修正 `CMakeLists.txt`（补 `Marshland::PublicHeaders` 链接）；读实际代码确认 `app::IStateReader` + `StateReadView` 可用并写明租约约束；评估 `Lib/PCA9685A` 后决定自研寄存器层、不链接供应商库；OE 默认使能不管；舵机归口 `servo.channels[]` 且供弹=通道 4、发射=通道 5；不新增 Public 类型。新增 §0.2 裁决表、§4.5 落库要求、§5.3 库评估、§11 待确认事项（11 项）。**除 `CMakeLists.txt` 外未修改任何代码。** |
| 0.4 | 2026-10-14 | 本轮 Coding：按裁决实施 `Inc/` + `Src/`（类型、寄存器常量与预分频、角度→计数值纯函数、`II2cBus` 与 Linux 后端、PCA9685A 设备层含 SWRST/回读校验、`ServoConfig` 逐键加载、`ServoCore` 含独立 IIC 线程与拒绝面、生命周期适配器 ≤ 50 行）；故障发布改为镜像 ATOM-03 的可空公共端口模式（见 §13.2）；登记配置数据缺口与 `array<int>` 约定两项阻塞项。**未编译。** |

---

## 13. 实施记录（2026-10-14 本轮 Coding）

### 13.1 新增文件清单

| 文件 | 行数 | 职责 |
|---|---|---|
| `Inc/atom07_servo_driver/servo_types.hpp` | 116 | 模块内部类型：机构枚举、`ServoStatus`、`WriteResult`、`ServoFault`；目标值与反馈在类型上分离 |
| `Inc/atom07_servo_driver/pca9685_registers.hpp` | 70 | 寄存器/位常量、`computePrescale()`、`actualFrequency()`（header-only 纯逻辑） |
| `Inc/atom07_servo_driver/angle_to_pulse.hpp` | 41 | 角度 → 脉宽 → 计数值的纯函数声明 |
| `Src/angle_to_pulse.cpp` | 56 | 线性映射实现；越界/非法参数返回失败（不隐式钳位） |
| `Inc/atom07_servo_driver/i2c_bus.hpp` | 48 | `II2cBus` 窄接口（write / read / writeRaw / isOpen / close，全部返回 `I2cStatus`） |
| `Inc/atom07_servo_driver/linux_i2c_bus.hpp` | 59 | Linux `/dev/i2c-N` 后端声明（单次事务超时） |
| `Src/linux_i2c_bus.cpp` | 191 | `open` / `ioctl(I2C_SLAVE)`、`select()` 超时（超时置 `ETIMEDOUT`）、短写判错 |
| `Inc/atom07_servo_driver/pca9685_device.hpp` | 55 | 设备层声明：初始化、频率、单通道写、故障查询 |
| `Src/pca9685_device.cpp` | 188 | SWRST → 唤醒+AI → MODE2 推挽 → PRE_SCALE 时序 → 回读校验 → 全通道关闭 |
| `Inc/atom07_servo_driver/servo_config.hpp` | 71 | 配置视图（`ServoConfig` / `ServoSlotConfig`）与加载结果 |
| `Src/servo_config.cpp` | 325 | 逐键读取并检查 `ConfigStatus`；`servo.channels[]` 解析；null → 未接入 |
| `Inc/atom07_servo_driver/servo_core.hpp` | 128 | 内核声明：启动/停止、按 id 或 mechanism 写目标、目标缓存、故障查询 |
| `Src/servo_core.cpp` | 367 | 独立 IIC 线程 + 请求队列 + 拒绝面（急停/未就绪/故障锁存）+ 故障发布 |
| `Inc/atom07_servo_driver/lifecycle_adapter.hpp` | 33 | 唯一 `ILifecycleModule` 实现声明 |
| `Src/lifecycle_adapter.cpp` | 36 | 配置加载 → 内核启动；可定位错误消息；`shutdown()` → `core_.stop()` |

### 13.2 故障发布实现（裁决 #3 报告结论 + 落法）

**ATOM-03 现状（已读源码核实）**：ATOM-03 全模块 **0 处** `Fault`/`fault` 命中，即它**不发布故障**；异常统一用快照字段 `online=false`（失联时把控制源降级为 `WEB`）表示，不新增异常状态表。它对外事实上报靠：`SbusCore` 构造期接收**可空**公共窄端口 `marshland::app::IInputReporter*`（默认 `nullptr`，调用前判空），由装配层注入 `AppCoordinator`；只调用 `ReportSbus(...)` 与 `ReportSoftwareEstop(...)`，**从不调用** `ReportFault`。仲裁同模式：可空 `IControlAuthority*`，并检查 `SubmitIntent` / `AuthorizeFire` 的返回值。头文件只包含 `AppInterfaces.hpp` / `IMonotonicClock.hpp` 等公共契约。

**ATOM-07 落法**：

1. 故障事实保存在内核：`ServoFault{kind, servo_id, channel, system_error, timestamp_us}`，经 `lastIicFault()` 查询（对应 ATOM-07 规格第 8 项，IIC 错误可查询）。
2. 事实发布镜像 ATOM-03：构造期接收可空 `app::IInputReporter*`，状态变化时调用 `ReportFault(true, FaultSeverity::SEVERE)`；`resetFault()` 时调用 `ReportFault(false, SEVERE)`。
3. 等级依据 `Design/Config.md` §14.2：`F-IIC-001`（IIC 打开/读写/初始化失败）与 `F-SERVO-001`（写入失败）均为**严重**；`W-CMD-001`（角度越界，警告）只作为 `WriteResult::AngleOutOfRange` 返回调用方，**不**置位锁存故障。
4. 故障码记录与历史由 ATOM-18 负责（裁决 #4）；ATOM-07 不写日志、不做历史、不解析故障表。
5. 故障锁存期间按 `servo.strategy.comm_fault_servos = HOLD` 拒绝新的普通目标（返回 `ServoStatus::FaultRejected`），直到上层调用 `resetFault()`。

> **冻结说明（用户 2026-10-14）**：**暂不继续编写发布故障相关代码**。已在 `Src/servo_core.cpp` 落地的 `ReportFault(true/false, FaultSeverity::SEVERE)` 调用按用户要求保留，不再扩展（不新增故障码表、不做历史、不加新端口）。

### 13.3 本轮裁决落点

| 裁决 | 落点 |
|---|---|
| 200 ms = **单次事务超时** | `timing.iic_comm_timeout_ms` → `LinuxI2cBus` 的 `select()` 超时；超时置 `ETIMEDOUT` → `ServoStatus::Timeout` / `ServoFaultKind::Timeout` |
| **独立 IIC 线程** | `ServoCore` 持有唯一工作线程与请求队列；调用方只入队并等待结果；总线事务全部由工作线程执行（天然串行化） |
| **允许 SWRST + 回读校验** | `Pca9685Device::initialize()`：SWRST(0x00←0x06) → 回读 `MODE1` → 唤醒+AI 与 `MODE2` 推挽 → 写 `PRE_SCALE` 并回读比对 → 回读 `MODE1` 校验 AI 置位、SLEEP 清零 → 全通道关闭 |
| **全局角度范围** | 校验只用 `servo.model.angle_range_deg`（`[0,180]`）；数组元素无 per-servo 范围字段 |
| **故障记录归 ATOM-18** | 本模块只提供事实与可空 `ReportFault`，不写日志/历史 |
| **通道来自配置** | `FEED` → 通道 4、`FIRE` → 通道 5 全部从 `servo.channels[]` 读取；代码内无 4/5 逻辑常量；发射舵机 `id = null` 时用 `writeTarget(ServoMechanism::Fire, ...)` 寻址 |

### 13.4 静态验证结果（本轮未编译）

- [x] 无 `main()` 入口（仅 `CMakeLists.txt` 注释提及）；无独立可执行目标。
- [x] 头文件全部位于 `Inc/atom07_servo_driver/`，实现全部位于 `Src/`。
- [x] 无 `AppCoordinator.hpp` / `RuntimeCoordinator.hpp` / `StateArbitration.hpp` 或任何 ATOM 私有头包含；只包含 `Atoms/Public` 契约、本模块头、C++ 标准库与 Linux 头。
- [x] `ILifecycleModule` 仅由 `lifecycle_adapter.cpp`（36 行）实现，≤ 50 行且无业务逻辑。
- [x] 所有配置读取逐键检查 `ConfigStatus`；`null`（未接入）如实失败，不填默认值。
- [x] 所有总线调用返回显式状态；无静默失败、无隐式角度钳位。
- [x] 目标值（`lastAcceptedTarget` / `WriteResult`）与物理反馈在类型与命名上分离。
- [x] `Lib/PCA9685A.cpp` 不在 `Src/` 下，不参与模块编译（仅作参考副本）。
- [ ] **未执行 ARM 增量编译**；`-Wall -Wextra -Wpedantic` 零警告尚未验证（见 §11 #8）。

### 13.5 阻塞项

1. `Config/dart_control.json` 缺少 `servo.pca9685.*`、`servo.model.*`、`timing.iic_comm_timeout_ms`（§11 #1）→ 真实启动会如实失败。
2. `array<int>` 的 `IConfigView` 读取约定待与 ATOM-15 确认（§11 #12）。
3. ATOM-10/11 的窄接口形态未冻结（§11 #10）；本模块内核 API 已提供 `writeTarget(int)` 与 `writeTarget(ServoMechanism)` 两种寻址及对应查询，便于装配层适配。

---

## 14. 接入就绪状态（Integration Readiness，2026-10-14）

### 14.1 结论

ATOM-07 已具备接入主系统的全部前提：CMake 目标与别名、公共契约依赖、注册步骤、配置键、对象图与生命周期接口齐备；装配层**无需解析配置、无需预打开设备**。
本轮按用户裁决**不做编译验证**，因此“就绪”是接口层/结构层/配置层的结论（见 §14.4 与 §14.5）。

> **2026-10-14 档 A 精简后更新**：对外契约已收敛为单头 `servo_driver.hpp`，装配接线改为 `MakeLinuxI2cBus()` + `ServoCore` + `LifecycleAdapter`。**以 §15 为准**；§14.2 / §14.3 保留为精简前快照。

### 14.2 装配层（Entry.cpp）需要做的事

```cpp
// 1) 对象图：bus -> core -> adapter；三者寿命由装配层保管（适配器与内核只持引用）
auto bus  = std::make_unique<marshland::atom07::LinuxI2cBus>();   // 默认可构造
auto core = std::make_unique<marshland::atom07::ServoCore>(
                *bus, monotonic_clock,
                &app_coordinator,   // app::IInputReporter*（可空）
                &app_coordinator);  // app::IStateReader*（可空）
auto servo = std::make_unique<marshland::atom07::LifecycleAdapter>(*core, config_view);

// 2) 注册：RuntimeStep::InitializeIicPca9685（在 ATOM-10/11 的 InitializeServoOutputs 之前）
if (!runtime.addModule(runtime::ModuleRegistration{
        runtime::RuntimeStep::InitializeIicPca9685, true, std::move(servo)})) {
    // 必须读取 runtime.lastRegistrationError()，禁止静默忽略
}
```

- `LinuxI2cBus` 无参构造；设备路径 / 地址 / 超时由内核在 `initialize()` 内按配置打开（镜像 ATOM-03 的 `configure()` 模式）。
- `IInputReporter*` 与 `IStateReader*` 均可空：不注入则不发布故障事实、不做闸门判定（仅保留可查询故障）。

### 14.3 装配层依赖的 ATOM-07 接口清单

| 符号 | 位置 | 用途 |
|---|---|---|
| `marshland::atom07::LinuxI2cBus` | `Inc/atom07_servo_driver/linux_i2c_bus.hpp` | `II2cBus` 生产实现（默认可构造，`open()` 由内核调用） |
| `marshland::atom07::ServoCore` | `Inc/atom07_servo_driver/servo_core.hpp` | 内核：`start` / `stop` / `isReady` / `writeTarget(int \| ServoMechanism, deg)` / `lastAcceptedTarget` / `lastIicFault` / `resetFault` |
| `marshland::atom07::LifecycleAdapter` | `Inc/atom07_servo_driver/lifecycle_adapter.hpp` | 唯一 `ILifecycleModule` 实现 |
| `marshland::atom07::ServoConfig` / `LoadServoConfig` | `Inc/atom07_servo_driver/servo_config.hpp` | 配置加载（适配器内部使用；装配层一般不需要） |
| `Marshland::atom_07_servo_driver` | `CMakeLists.txt` | 模块目标与别名 |

### 14.4 就绪检查表

- [x] CMake：目标 `atom_07_servo_driver` + 别名 `Marshland::atom_07_servo_driver`，链接 `Marshland::PublicHeaders`；`Atoms/CMakeLists.txt` 已 `add_subdirectory` 并加入聚合链接。
- [x] 注册步骤 `RuntimeStep::InitializeIicPca9685` 存在于 `Atoms/Public/ILifecycle.hpp`，且先于 `InitializeServoOutputs`。
- [x] 依赖仅为 Public 契约（`AppInterfaces.hpp` / `ILifecycle.hpp` / `IMonotonicClock.hpp` / `IConfigView.hpp` / `MarshlandTypes.hpp`）。
- [x] 配置就绪：13 项必需键全部存在且合法（实测 `FEED` → 通道 4、`FIRE` → 通道 5）。
- [x] ATOM-15 确认标量数组读取约定（`getIntAt(key, index, "")`，`""` 与 `nullptr` 兼容）。
- [x] 无 `main()`、无独立可执行文件、无自行注册、无 JSON 自解析。
- [x] 独立 IIC 线程在 `initialize()` 内创建、`shutdown()` 内 `join`；无运行中重启路径。
- [x] 闸门：急停锁存 / 未就绪 / 故障锁存时本模块自身拒绝普通目标；持 `StateReadView` 租约期间无 I/O。
- [ ] 编译验证（用户裁决本轮不做）。
- [ ] 与 ATOM-10/11 的窄接口适配（由它们声明、装配层适配）。

### 14.5 已知限制与待决策（不影响接入前提，但需知晓）

1. **未编译**：本轮仅完成静态检查（结构、依赖、键表、括号配平），无编译器验证。
2. **无配置热加载**：`ServoConfig` 只在 `initialize()` 读取一次（与 ATOM-03 一致）；若要求运行期热更新脉宽/角度范围，需追加 `configVersion()` 重读逻辑。
3. **`resetFault()` 调用路径**：当前由持有 `ServoCore` 的装配层直接调用；若要求 ATOM-14 经窄端口恢复，需要新增端口（跨模块裁决）。
4. **未提供 `enable()` / `disable()` / `getState()`**：`Design/Interface.md` §5.3 的机构控制器共同约束适用于 ATOM-10/11；本驱动暴露 `isReady()` / `lastIicFault()` / `lastAcceptedTarget()` / `config()`。
5. **故障发布冻结**：现有 `ReportFault(true/false, SEVERE)` 调用保留，不再扩展（用户裁决）。
6. `Lib/PCA9685A.hpp` 会被模块头文件 glob 收录（不编译、不链接）；如需彻底排除可在 `CMakeLists.txt` 中加 `EXCLUDE`。

| 0.5 | 2026-10-14 | 接入就绪轮：`II2cBus` 增加 `open(device_path, address, timeout_ms)`，`LinuxI2cBus` 改为默认可构造，内核在 `start()` 内按配置打开总线（装配层无需解析配置）；关闭 §11 #8（不做编译验证）与 #12（ATOM-15 确认 `getIntAt(key, index, "")`）；新增 §14 接入就绪状态（装配对象图、接口清单、检查表、已知限制）。 |

---

## 15. 档 A 精简结果与对外契约清单（2026-10-14）

> **已被 §16 取代**：用户要求进一步收敛到「生命周期 + 装配 + 传参」，对外可调用符号 22 → 8。本节保留为档 A 快照。

### 15.1 精简后的文件布局

```text
Inc/atom07_servo_driver/
├── servo_driver.hpp          ← 唯一对外契约头（201 行）
└── detail/                   ← 实现细节（不属于对外契约）
    ├── servo_config.hpp      （ServoConfig / LoadServoConfig）
    ├── pca9685_device.hpp    （寄存器常量 + computePrescale + Pca9685Device）
    ├── angle_to_pulse.hpp    （角度 → 脉宽 → 计数值）
    └── linux_i2c_bus.hpp     （Linux /dev/i2c-N 实现）
Src/
├── servo_core.cpp            （PIMPL：配置读取 / 总线打开 / 设备初始化 / IIC 线程）
├── lifecycle_adapter.cpp     （28 行，唯一 ILifecycleModule 实现）
├── servo_config.cpp          （逐键配置读取）
├── pca9685_device.cpp        （SWRST + 回读校验 + 通道写）
├── angle_to_pulse.cpp        （纯函数）
└── linux_i2c_bus.cpp         （Linux I2C + MakeLinuxI2cBus 工厂）
```

### 15.2 装配层接线（精简后，替代 §14.2）

```cpp
auto bus   = marshland::atom07::MakeLinuxI2cBus();          // 生产实现（分配失败返回 nullptr）
auto core  = std::make_unique<marshland::atom07::ServoCore>(
                 *bus, config_view, monotonic_clock,
                 &app_coordinator,    // app::IInputReporter*（可空）
                 &app_coordinator);   // app::IStateReader*（可空）
auto servo = std::make_unique<marshland::atom07::LifecycleAdapter>(*core);

if (!runtime.addModule(runtime::ModuleRegistration{
        runtime::RuntimeStep::InitializeIicPca9685, true, std::move(servo)})) {
    // 读取 runtime.lastRegistrationError()
}
```

要点：配置视图改由 `ServoCore` 构造期持有；`LifecycleAdapter` 只持 `ServoCore&`；
`start()` 内部完成配置读取 → 打开总线 → PCA9685A 初始化（含回读校验）→ 启动独立 IIC 线程。

### 15.3 对外契约清单（单头 `servo_driver.hpp`）

| 类别 | 符号 | 形式 |
|---|---|---|
| 工厂 | `MakeLinuxI2cBus()` | `std::unique_ptr<II2cBus>` |
| 抽象 | `II2cBus` | 6 个纯虚方法：`open` / `write` / `read` / `writeRaw` / `isOpen` / `close` |
| 内核 | `ServoCore` | 10 个方法：`start` / `stop` / `isReady` / `writeTarget(int,deg)` / `writeTarget(ServoMechanism,deg)` / `lastAcceptedTarget(int)` / `lastAcceptedTarget(ServoMechanism)` / `lastIicFault` / `resetFault` / `lastError` |
| 生命周期 | `LifecycleAdapter` | 3 个：`initialize` / `shutdown` / `name` |
| 枚举 | `ServoMechanism`(4) / `ServoStatus`(9) / `ServoFaultKind`(4) | — |
| 结构体 | `I2cStatus{ok, system_error}` / `WriteResult{...}` / `ServoFault{...}` | `ok()` / `active()` 为 inline 判定 |

合计：**1 个对外头、3 个类、1 个工厂函数、3 个结构体、3 个枚举**；
可调用公开符号 **22 个**（II2cBus 6 + 工厂 1 + ServoCore 10 + LifecycleAdapter 3 + `ok()` / `active()` 2）。
精简前同一口径为 45 个（34 个类方法 + 2 个 inline 判定 + 6 个自由函数 + 3 个 `ToString`），即 **−51%**。

### 15.4 移出对外契约的内容（现为 detail 内部）

- `LinuxI2cBus`（含其 8 个公开成员）→ 只经 `MakeLinuxI2cBus()` 暴露；
- `Pca9685Device` + 18 个寄存器/位常量 + `computePrescale`；
- `angleToOffTicks` + `PulseConversion`；
- `ServoConfig` / `ServoSlotConfig` / `ServoConfigLoadResult` / `LoadServoConfig`；
- 3 个 `ToString`（无调用，直接删除）；`FindSlotById` / `FindSlotByMechanism`（内核未用，删除）；
- `ServoStatus::InvalidArgument`、`pca9685::actualFrequency`、`LinuxI2cBus::transactionTimeoutMs`（死代码，删除）。

### 15.5 体量说明（如实记录）

| 指标 | 精简前 | 精简后 |
|---|---|---|
| 对外头文件 | 9 个 / 626 行 | **1 个 / 201 行** |
| 对外可调用符号 | 45 | **22** |
| 全部文件（头+源） | 15 个 / 1809 行 | **11 个 / 1744 行** |
| 代码行（非空非注释） | 1403 | 1398 |

**结论**：本轮削减的是**对外契约**（−47% 符号、−89% 对外头行数），不是代码总量；
总行数基本持平（PIMPL 引入 Impl 定义、对外头新增使用说明注释）。
若还要减少实现体量，需执行此前登记的「重复分支合并 / 配置校验样板抽取」两项，属另一轮工作。

| 0.6 | 2026-10-14 | **档 A 精简**：对外契约收敛为单头 `Inc/atom07_servo_driver/servo_driver.hpp`；`LinuxI2cBus` / `Pca9685Device` / 寄存器常量 / 角度换算 / `ServoConfig` 全部移入 `detail/`（内部）；新增 `MakeLinuxI2cBus()` 工厂；`ServoCore` 改为 PIMPL、构造期注入 `IConfigView`、`start()` 无参并内置配置读取、新增 `lastError()`；删除死代码与未使用的查找函数。对外可调用符号 45 → 22。 |

---

## 16. 最小对外契约（2026-10-14 第三轮）

### 16.1 外部只需要三件事

1. **生命周期管理**：`LifecycleAdapter`（唯一 `ILifecycleModule` 实现，28 行）；
2. **装配**：`ServoCore(config_view, clock, 可选输入端口, 可选状态端口)`；
3. **传参命令**：`writeTarget(舵机 id 或机构, 角度)` —— 例 `writeTarget(ServoMechanism::Fire, 180.0)`。

### 16.2 完整对外契约（单头 `servo_driver.hpp`，103 行）

```cpp
namespace marshland::atom07 {

enum class ServoMechanism : uint8_t { Unknown, Reload, Feed, Fire };

class ServoCore final {
    ServoCore(const config::IConfigView&, const app::IMonotonicClock&,
              app::IInputReporter* = nullptr, const app::IStateReader* = nullptr);
    ~ServoCore() noexcept;
    bool writeTarget(int servo_id, double angle_deg) noexcept;
    bool writeTarget(ServoMechanism mechanism, double angle_deg) noexcept;
    std::optional<double> lastAcceptedTarget(ServoMechanism mechanism) const noexcept;
    void resetFault() noexcept;
    const char* lastError() const noexcept;
private:
    friend class LifecycleAdapter;
    bool start() noexcept;   // 生命周期内部入口（读配置/开总线/初始化/起线程）
    void stop() noexcept;
    struct Impl; std::unique_ptr<Impl> impl_;
};

class LifecycleAdapter final : public runtime::ILifecycleModule {
    explicit LifecycleAdapter(ServoCore&) noexcept;
    ModuleInitResult initialize(const RuntimeContext&) override;
    void shutdown() noexcept override;
    const char* name() const noexcept override;
};
}
```

**合计：2 个类、1 个枚举（4 值）、0 个结构体、0 个自由函数；可调用公开符号 8 个**
（`writeTarget`×2 + `lastAcceptedTarget` + `resetFault` + `lastError` + `initialize` / `shutdown` / `name`）。

| 口径 | 原始 | 档 A | 本轮最小面 |
|---|---|---|---|
| 对外头文件 | 9 个 / 626 行 | 1 个 / 201 行 | **1 个 / 103 行** |
| 对外类 | 5 | 3 | **2** |
| 对外结构体 | 7 | 3 | **0** |
| 对外自由函数 | 6 | 1 | **0** |
| 对外可调用符号 | 45 | 22 | **8**（−82%） |

### 16.3 本次移除的内容与其代价

| 移除项 | 代价 / 补偿 |
|---|---|
| `WriteResult` / `ServoStatus` / `ServoFault` / `ServoFaultKind` | 调用方只拿到 `bool`；失败原因经 `lastError()` 文本，例如 `writeTarget(id=5, mechanism=FEED, channel=4, angle=200.0): ANGLE_OUT_OF_RANGE`。若 ATOM-10/11 需要机器可判的错误分类，需加回 1 个枚举返回 |
| `II2cBus` / `I2cStatus` / `MakeLinuxI2cBus()` | 总线由内核内部创建（`detail/linux_i2c_bus.hpp`）。测试替身注入不再属于契约；`Rule/AtomIntegration.md` §2.1 的「便于上层以测试替身验证」被弱化，如需恢复可在 `detail/` 增加注入工厂 |
| `isReady()` | 未就绪时 `writeTarget` 返回 `false` 且 `lastError()` = `NOT_READY` |
| `lastAcceptedTarget(int)`（id 重载） | 保留按机构的单一查询即可覆盖 FEED / FIRE |
| `config()` 及配置访问器 | 下游若需通道映射须自行经 `IConfigView` 读取，或由装配层适配 |

### 16.4 仍保留但可再删的两个符号（等你裁决）

1. `resetFault()`：IIC 故障锁存后若无清除入口，驱动会永久拒绝普通目标；若确认由 ATOM-14 走其它恢复路径，可删（−1）。
2. `lastAcceptedTarget()`：`Design/Atomic/ATOM-07_Servo_Driver.md` 内部原子操作第 7 项要求「查询最近接受的目标值且不得冒充物理反馈」；删掉后该项无法从外部验证，可删（−1）。

两者都删则对外可调用符号降到 **6 个**（`writeTarget`×2 + `lastError` + 适配器 3），为本模块理论下限。

| 0.7 | 2026-10-14 | **最小对外契约**：按用户要求收敛为「生命周期 + 装配 + 传参」。`ServoCore` 对外只剩 `writeTarget`×2 / `lastAcceptedTarget` / `resetFault` / `lastError`，`start()` / `stop()` 转为 private + `friend LifecycleAdapter`；`II2cBus`、`I2cStatus`、`MakeLinuxI2cBus()`、`WriteResult`、`ServoStatus`、`ServoFault`、`ServoFaultKind` 全部移入 `detail/`，总线改由内核内部创建。对外可调用符号 22 → 8（原始 45 → 8，−82%）。 |
| 0.8 | 2026-10-14 | 内部缺陷修复（P0）：@servo.pca9685.address@ 的 @"0x40"@ 前缀被旧 @parseAddress()@ 拒绝，导致 @initialize()@ 必然失败；改为按 schema @^0x[0-9A-Fa-f]{2}$@ 严格解析，并用镜像脚本复验整条 @LoadServoConfig@ 链与角度换算（FEED 20°/30° → 148/171 ticks，FIRE 90°/180° → 307/512 ticks，181° 拒绝）。顺带移除 @linux_i2c_bus.cpp@ 中未使用的 @\<utility\>@。对外契约不变。 |
