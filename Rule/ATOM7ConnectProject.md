# ATOM7ConnectProject — ATOM-07 舵机底层接入规则

| 项目 | 内容 |
|---|---|
| 模块 | ATOM-07 PCA9685A/IIC 舵机底层（Marshland） |
| 模块目录 | `Atoms/ATOM-07_Servo_Driver/` |
| 本文档版本 | 1.1 |
| 日期 | 2026-10-14 |
| 设计基线 | PRD 0.10、PEF 1.5、Interface 1.5/1.6、Config 0.8、Atoms 0.11、`Rule/AtomIntegration.md` 1.3、`Rule/ConfigSchema.md` 1.4 |
| 关联文档 | `Rule/AtomIntegration.md`、`Rule/ConfigSchema.md`、`Rule/ATOM10ConnectProject.md`、`Rule/ATOM11ConnectProject.md`、`Atoms/ATOM-07_Servo_Driver/PLAN.md`（v0.7）、`Entry.cpp` |
| 编制说明 | 本文件由 ATOM-07 维护，经用户 2026-10-14 指示写入 `Rule/`（超出本模块工作目录的常规可写范围） |

---

## 1. 接入目标

ATOM-07 把一个逻辑舵机目标（舵机编号或机构 + 角度）映射为一个配置通道的 PCA9685A PWM 输出，并负责：

- 读配置、打开 IIC 设备、初始化 PCA9685A（SWRST + 回读校验）；
- 角度 → 脉宽 → 12 位计数值换算与越界拒绝；
- 单通道写入、独立 IIC 线程串行化、单次事务超时；
- 最近接受目标值查询（目标值语义，**不得**冒充物理反馈）；
- 急停锁存 / IIC 故障锁存期间**由本模块自身**拒绝普通目标。

**不负责**：决定什么时候动舵机（ATOM-10/11/13）、发射授权与仲裁（ATOM-02/11）、安全恢复编排（ATOM-14）、OE 引脚与限位输入（ATOM-06）、故障历史与日志（ATOM-18）。不定义 `main()`、不自行注册生命周期、不解析 JSON。

---

## 2. CMake 接入

```text
Marshland executable
  -> Atoms/Public/AppCoordinator（顶层装配）
  -> Marshland::atom_07_servo_driver
  -> Marshland::PublicHeaders
```

1. 模块目标 `atom_07_servo_driver`，别名 `Marshland::atom_07_servo_driver`；上层一律用别名链接。
2. `Src/` 非空 ⇒ 目标为 **STATIC** 库；不定义 `project()`、不产生可执行文件、无 `main()`。
3. 依赖：文件尾部 `if(TARGET Marshland::PublicHeaders)` 链接公共契约；另需 Linux IIC（`/dev/i2c-N` + `ioctl` / `select`）与 C++ 标准线程库。
4. 上层 `Atoms/CMakeLists.txt` 已 `add_subdirectory(ATOM-07_Servo_Driver)` 并加入 `marshland_atoms` 聚合。
5. 不设置任何编译器 / 架构 / C++ 标准 / 编译选项；不创建独立测试 `main()`。

---

## 3. 对外契约（唯一公开头 `Inc/atom07_servo_driver/servo_driver.hpp`）

对外只有三件事：**生命周期管理 + 装配 + 传参命令**。全部符号如下（8 个可调用符号 + 1 个枚举 + 2 个类）：

| 符号 | 签名 | 说明 |
|---|---|---|
| `ServoMechanism` | `enum class : uint8_t { Unknown, Reload, Feed, Fire }` | 按机构寻址（发射舵机 id 为 `null`，必须能按机构寻址） |
| `ServoCore` | `ServoCore(const config::IConfigView&, const app::IMonotonicClock&, app::IInputReporter* = nullptr, const app::IStateReader* = nullptr)` | 装配入口；配置在构造期注入 |
| | `bool writeTarget(int servo_id, double angle_deg) noexcept` | 按 `servo.channels[].id` 下发角度（度） |
| | `bool writeTarget(ServoMechanism mechanism, double angle_deg) noexcept` | 按机构下发（`Fire` = 发射/扳机舵机） |
| | `std::optional<double> lastAcceptedTarget(ServoMechanism) const noexcept` | 最近成功接受的目标值 |
| | `void resetFault() noexcept` | IIC 故障锁存恢复（根因清除后由上层调用） |
| | `const char* lastError() const noexcept` | 最近一次失败原因（`nullptr` = 无错误） |
| `LifecycleAdapter` | `explicit LifecycleAdapter(ServoCore&) noexcept` | 唯一 `ILifecycleModule` 实现（28 行） |
| | `ModuleInitResult initialize(const RuntimeContext&) override` | 读配置 → 开总线 → 初始化 PCA9685A → 起 IIC 线程 |
| | `void shutdown() noexcept override` | 停线程、关总线（幂等） |
| | `const char* name() const noexcept override` | `"atom07-servo-driver"` |

`ServoCore::start()` / `stop()` 为 **private**，仅 `LifecycleAdapter`（friend）可调用。

以下内容**不属于**对外契约，位于 `Inc/atom07_servo_driver/detail/` 与 `Src/`：
`II2cBus` / `I2cStatus` / `LinuxI2cBus` / `Pca9685Device` / 寄存器常量 / `computePrescale` /
`angleToOffTicks` / `ServoConfig` / `LoadServoConfig` / `ServoStatus` / `WriteResult` / `ServoFault` / `ServoFaultKind`。

---

## 4. API 如何使用

### 4.1 传参命令（唯一业务入口）

```cpp
using marshland::atom07::ServoMechanism;

// 例：Fire（扳机/发射舵机）转到 180°（配置中 FIRE 的 action_deg = 180）
core.writeTarget(ServoMechanism::Fire, 180.0);

// 例：供弹舵机（servo.channels[].id = 5）转到 20°
core.writeTarget(5, 20.0);
```

- 角度单位：度；允许范围取 `servo.model.angle_range_deg`（当前 `[0, 180]`）。
- 通道与舵机映射**只来自 `servo.channels[]`**（当前 FEED → 通道 4、FIRE → 通道 5）；代码内无硬编码，调用方也不需要知道通道号。
- 返回值：`true` = 已成功写入 PCA9685A 通道；`false` = 被拒绝或写入失败。

### 4.2 失败原因

```cpp
if (!core.writeTarget(ServoMechanism::Fire, 200.0)) {
    const char* error = core.lastError();
    // 例：writeTarget(id=0, mechanism=FIRE, channel=5, angle=200.0): ANGLE_OUT_OF_RANGE
    // 例：writeTarget(id=0, mechanism=FIRE, channel=5, angle=180.0): ESTOP_REJECTED
    // 例：writeTarget(id=0, mechanism=FIRE, channel=5, angle=180.0): FAULT_REJECTED
    // 例：writeTarget(servo id 9): not registered (CHANNEL_NOT_WIRED)
}
```

文本中固定携带 `servo_id / mechanism / channel / angle` 与状态字；状态字取值：
`NOT_READY` / `CHANNEL_NOT_WIRED` / `CHANNEL_INVALID` / `ANGLE_OUT_OF_RANGE` /
`ESTOP_REJECTED` / `FAULT_REJECTED` / `IIC_ERROR` / `TIMEOUT`。
成功一次即清空上次错误。

### 4.3 目标值查询与故障恢复

```cpp
// 最近接受的目标值（目标值，不是物理反馈；从未成功返回 nullopt）
const auto target = core.lastAcceptedTarget(ServoMechanism::Fire);

// IIC 故障锁存后的恢复入口：根因（总线/接线/电源）清除后调用
core.resetFault();
```

---

## 5. 装配对象图

```text
IConfigView 实现（ATOM-15 ConfigStore）
IMonotonicClock 实现（装配层，如 SystemMonotonicClock）
AppCoordinator（实现 IInputReporter / IStateReader）           ← 必须在 ATOM-07 构造之前就绪
ServoCore(config_view, clock, &app /*IInputReporter*/, &app /*IStateReader*/)   ← 内核；内部自建 LinuxI2cBus
LifecycleAdapter(ServoCore&)                                    ← 只持引用
```

```cpp
auto servo_core = std::make_unique<marshland::atom07::ServoCore>(
        config_store,                // const config::IConfigView&
        clock,                       // const app::IMonotonicClock&
        &app,                        // app::IInputReporter*（可空）
        &app);                       // const app::IStateReader*（安全闸门，**不应为空**，见 §8）

auto servo_adapter = std::make_unique<marshland::atom07::LifecycleAdapter>(*servo_core);
```

寿命要求：`ServoCore` 必须长于 `LifecycleAdapter`（适配器只持 `ServoCore&`）；`IConfigView`、时钟与两个端口必须长于 `ServoCore`。

---

## 6. 生命周期注册

```cpp
const bool registered = runtime.addModule({
    marshland::runtime::RuntimeStep::InitializeIicPca9685,
    true,                                   // required：舵机为关键硬件，必须显式书写
    std::move(servo_adapter)});
if (!registered) {
    const auto error = runtime.lastRegistrationError();   // 禁止静默忽略
}
```

- 注册步骤固定为 `RuntimeStep::InitializeIicPca9685`（`Atoms/Public/ILifecycle.hpp`）。
- 顺序依赖：**ATOM-07 必须先于 ATOM-10 / ATOM-11**；ATOM-11 的步骤 `InitializeServoOutputs` 晚于本步骤。
- `initialize()` 内部一次完成：配置读取 → 打开 IIC → PCA9685A 初始化（SWRST + 回读 `MODE1` / `PRE_SCALE`）→ 启动独立 IIC 线程。
- `shutdown()` 幂等：停线程、关设备与总线；不支持运行中重启。
- 注册只能在装配层完成；ATOM-07 不自行注册，也不注册其他模块。

---

## 7. 配置接入

配置只经 `marshland::config::IConfigView` 查询，不由 ATOM-07 打开 JSON；键名以 `Rule/ConfigSchema.md` §2.10 为准。任一必需键非 `OK` 或越界 ⇒ `initialize()` 返回 `success = false`，`error_message` 给出可定位键名与 `ConfigStatus`（例如 `config 'servo.pca9685.i2c_device' unusable: MISSING`）。

| 键 | 类型 | 当前值 | 用途 |
|---|---|---|---|
| `servo.pca9685.i2c_device` | str | `/dev/i2c-7` | 打开 IIC 设备 |
| `servo.pca9685.address` | str | `"0x40"` | 器件 7 位地址 |
| `servo.pca9685.channel_count` | int | `16` | 通道上界（必须等于 16） |
| `servo.pca9685.pwm_resolution_bits` | int | `12` | 计数值上限 4096 |
| `servo.pca9685.pwm_frequency_hz` | int | `50` | PWM 频率（须能算出合法预分频） |
| `servo.pca9685.channel_range` | array\<int\>（2 项） | `[0, 15]` | 通道合法性 |
| `servo.pca9685.oe_active_level` | enum str | `LOW` | 仅校验（OE 硬件默认使能，模块不驱动 GPIO） |
| `servo.pca9685.access_mode` | str | `SERIALIZED` | 串行访问要求（非此值 ⇒ 失败） |
| `servo.pca9685.error_handling` | str | `ENTER_FAULT` | IIC 错误进入故障 |
| `servo.model.pulse_min_us` | int | `500` | 脉宽下限 |
| `servo.model.pulse_max_us` | int | `2500` | 脉宽上限 |
| `servo.model.angle_range_deg` | array\<int\>（2 项） | `[0, 180]` | 角度校验唯一来源 |
| `timing.iic_comm_timeout_ms` | int | `200` | 单次事务超时 |
| `servo.channels[]` | 对象数组 | FEED(id=5, ch=4, 20/30)、FIRE(id=null, ch=5, 90/180) | 舵机资源唯一归口 |

约定：

- `channel` 为 `null` 表示"未接入"，模块如实失败（`CHANNEL_NOT_WIRED`），**禁止**推断默认通道。
- 发射舵机 `id` 为 `null`，因此按 `ServoMechanism::Fire` 寻址；供弹可用 id 或 `ServoMechanism::Feed`。
- 标量数组（`channel_range` / `angle_range_deg`）按 `getIntAt(key, index, "")` 读取（ATOM-15 已确认 `""` 与 `nullptr` 均兼容）。
- **本版本无配置热加载**：配置仅在 `initialize()` 读取一次；运行期修改需重启流程。

---

## 8. 线程、时序与安全边界

```text
调用方线程（ATOM-10/11 或手动路径）
  -> writeTarget
     -> 取 StateReadView 租约，读 estop_latched / hardware_ready / run_gate（租约内不做任何 I/O）
     -> 释放租约 -> 角度换算与越界校验 -> 入队 -> 等待结果（预算按队列长度放大）
IIC worker 线程（唯一持有总线）
  -> 取队列 -> 写 PCA9685A 通道 -> 成功/失败 -> 记录故障事实 -> 唤醒调用方
```

1. **串行化**：所有总线事务由唯一 worker 线程执行（`access_mode = SERIALIZED`）；调用方不直接访问 IIC。
2. **超时**：单次事务超时 = `timing.iic_comm_timeout_ms`（当前 200 ms），由 `select()` 实现，超时置 `ETIMEDOUT` 并返回 `TIMEOUT`。
3. **闸门**：`StateReadView` 租约只用于读事实，持租约期间不做 I/O；租约不跨线程存活。构造时 `IStateReader*` 为 `nullptr` 时**闸门判定被跳过**——这是安全性下降，见 §10 差异 6。
4. **急停锁存**：`estop_latched()` 为真 ⇒ 返回 `false`（`ESTOP_REJECTED`），**不下发**、不改变当前角度。
5. **IIC 故障锁存**：`comm_fault_servos = HOLD` ⇒ 返回 `false`（`FAULT_REJECTED`），直至上层调用 `resetFault()`。
6. **越界**：`out_of_range_action = REJECT_AND_WARN` ⇒ 拒绝下发、不改变当前有效目标（`ANGLE_OUT_OF_RANGE`）。
7. 本模块**不提交** `ControlIntent`、不判定发射授权、不包含 `AppCoordinator.hpp` 或其他 ATOM 私有头；故障事实经可空 `IInputReporter::ReportFault(true/false, SEVERE)` 发布，历史记录归 ATOM-18。
8. 换弹机构不使用舵机（双 M3508）：`servo.channels[]` 中若出现 `mechanism = RELOAD`，本模块不产生 PWM。

---

## 9. 接入自检清单

- [ ] `Atoms/CMakeLists.txt` 已包含 `add_subdirectory(ATOM-07_Servo_Driver)` 与聚合链接；
- [ ] `dart_control.json` 含 §7 全部 13 个必需键与 `servo.channels[]`（当前已就绪）；
- [ ] 装配层构造 `ServoCore` 时注入**非空** `IConfigView`、`IMonotonicClock` 与 `IStateReader`；
- [ ] `LifecycleAdapter` 注册到 `InitializeIicPca9685` 且 `required` 显式书写；
- [ ] 注册返回值与 `lastRegistrationError()` 已检查；
- [ ] ATOM-07 的注册顺序早于 ATOM-10 / ATOM-11；
- [ ] 未包含任何 ATOM 私有头；未在模块内注册自己；
- [ ] ARM 增量构建（WSL aarch64 或目标机）通过且 `-Wall -Wextra -Wpedantic` 零警告；
- [ ] §10 的接口差异已按下述选项之一处理完毕。

---

## 10. 接入前置差异（当前主系统需处理，阻塞编译或安全）

以下差异来自对当前 `Entry.cpp` 与 `../Atoms/ATOM-08_Control` 的实际核对。**ATOM-07 侧不改动则主系统无法编译**，需要按下表选择处理方。

| # | 差异 | 当前主系统现状 | ATOM-07 现状 | 处理选项 |
|---|---|---|---|---|
| 1 | 公开头名 | `Entry.cpp` 与 ATOM-11 包含 `atom07_servo_driver/servo_core.hpp` | 已合并为 `atom07_servo_driver/servo_driver.hpp` | A) 消费方改 include；B) ATOM-07 提供 `servo_core.hpp` 兼容转发头 |
| 2 | `LinuxI2cBus` 公开 | `Entry.cpp` 自带 `linux_i2c_bus.hpp` 并构造 `LinuxI2cBus servo_i2c_bus;` | 总线已内化（核心自建），`detail/` 内 | A) 装配层不再构造总线；B) ATOM-07 恢复公开总线口 |
| 3 | `lifecycle_adapter.hpp` | `Entry.cpp` 单独包含该头 | 已合并进单头 | A) 消费方改 include；B) 提供兼容转发头 |
| 4 | `ServoCore` 构造签名 | `(servo_i2c_bus, clock, nullptr, nullptr)` | `(config_view, clock, reporter, reader)` | A) 装配层改为传 `config_store` 与 `&app`；B) ATOM-07 增加兼容构造 |
| 5 | `LifecycleAdapter` 构造 | `(servo_core, config_store)` | `(ServoCore&)` | A) 装配层去掉第二个实参；B) ATOM-07 接受可选 `IConfigView` |
| 6 | **安全闸门端口为空** | `Entry.cpp` 传 `nullptr`，且 `AppCoordinator` 在 ATOM-07 注册**之后**才构造（`Entry.cpp:461`） | 需要非空 `IStateReader*` 才能自身拒绝急停期间的普通目标 | **必须处理**：提前构造 `AppCoordinator`（`ATOM11ConnectProject.md` §2 已给出该路径），或 ATOM-07 增加晚绑定入口（`bindStateReader`，+1 符号） |
| 7 | FEED 路径归属 | `Entry.cpp` 以 `StubFeedActuator(ServoCore&)` 直接持有 ATOM-07；`ATOM10ConnectProject.md` §4 记"供弹硬件已变更，实现方待 Design 修订后确定" | 仍按 `servo.channels[]` 提供 FEED（通道 4）能力 | 与 Design 修订对齐后确定 FEED 是否继续经 ATOM-07 |
| 8 | 基线漂移 | 兄弟文档为 Interface 1.8 / Config 1.4 / ConfigSchema 1.9（2026-10-15） | 本文按 Interface 1.5/1.6、Config 0.8、ConfigSchema 1.4 编写 | 复核 `servo.*` 键表与通道映射是否仍一致，必要时同步本文 |

处理建议（待用户裁决）：若以"零改动接入主系统"为优先，选各行的 **B 选项**（ATOM-07 侧提供兼容头与兼容构造，对外符号增加约 4 个）；若以"最小契约"为优先，选 **A 选项**（改 `Entry.cpp` 约 6 行 + ATOM-11 一行 include，均属装配层/其他模块的写权限）。

---

## 11. 验证责任

ATOM-07 侧已完成的静态验证：

- 注释剥离后括号/圆括号配平（13/13 文件）；
- 对外头不含任何内部类型（`II2cBus` / `I2cStatus` / `WriteResult` / `ServoStatus` / `ServoFault` / `LinuxI2cBus` 均为 0 处）；
- 无 `main()`、无独立可执行目标、无跨 ATOM 私有头、无 JSON 自解析；
- 对外可调用符号 8 个（精简前 45 个）。

尚未执行：ARM 增量编译与 `-Wall -Wextra -Wpedantic` 零警告验证（用户 2026-10-14 裁决本轮不做编译验证）。
运行时验证（实际舵机角度、IIC 时序、急停拒绝、超时判定）由上层集成测试与目标机联调承担。

---

## 12. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.1 | 2026-10-14 | 修复 ATOM-07 内部 P0 缺陷：@servo.pca9685.address@ 形如 @"0x40"@，旧 @parseAddress()@ 把前缀 @x@ 当作非法十六进制字符，导致配置装载必然失败（@initialize()@ 返回 @servo.pca9685.address unusable: OUT_OF_RANGE@）。现按 ConfigSchema 的 @^0x[0-9A-Fa-f]{2}$@ 严格解析前缀；同时清理未使用的 @\<utility\>@ 包含。对外契约不变（仍 8 个可调用符号）。 |
| 1.0 | 2026-10-14 | 首次编制：固化 ATOM-07 最小对外契约（生命周期 + 装配 + 传参）、装配对象图、生命周期注册、配置键、线程与安全边界、接入自检清单，并登记与当前 `Entry.cpp` / ATOM-11 的 8 项接入差异（含安全闸门端口为空的阻塞项）。 |
