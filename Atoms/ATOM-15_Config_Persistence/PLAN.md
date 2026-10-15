# ATOM-15 Config Persistence 实施方案与报告

## 1. 已核对的规范与实际用法

已只读核查并按以下资料实现：

- `Design/PRD.md`、`Design/PEF.md`、`Design/Interface.md`、`Design/Config.md`；
- `Design/Atomic/ATOM-01_Runtime.md`、`ATOM-02_State_Arbitration.md`、`ATOM-03_SBUS.md`、`ATOM-15_Config_Persistence.md`；
- `Design/LifecycleEvents.md`；
- `Rule/AtomIntegration.md`、`Rule/ConfigSchema.md`、`Rule/ConfigSchema.schema.json`、`Rule/ATOM3ConnectProject.md`；
- `Atoms/Public/IConfigView.hpp`、`ILifecycle.hpp`、`AppInterfaces.hpp`；
- ATOM-01 的 `RuntimeCoordinator` 实现、ATOM-02 的状态接口、ATOM-03 的真实 `IConfigView` 消费代码。

已确认的实际配置消费键：

- `sbus.serial_device`
- `sbus.channel_scale_x`
- `sbus.frame_bytes`
- `sbus.joystick_center`
- `sbus.valid_min`
- `sbus.valid_max`
- `timing.sbus_receive_period_ms`
- `timing.sbus_loss_timeout_ms`

ATOM-03 通过构造注入 `const marshland::config::IConfigView&`，并检查 `ConfigStatus`；配置模块从 `RuntimeContext::configuration_path` 加载文件，不能依赖 `RuntimeContext::values` 向其他模块提供类型安全配置。

## 2. 当前实现

### 2.1 公共实现头

`Inc/atom15_config/config_store.hpp` 提供：

- `ConfigStore`，实现完整的 `marshland::config::IConfigView`；
- `LifecycleAdapter`，实现 `marshland::runtime::ILifecycleModule`；
- `ConfigLoadResult`、`ConfigDiagnostic`、`ConfigError`；
- `ConfigValue` 和运行时更新结果 `PersistResult`；
- `PidConfig`、PID 读写结果；
- 预留变量读写接口；
- 配置变更回调和单调递增版本号。

### 2.2 JSON 与校验

`Src/config_store.cpp` 当前基于 nlohmann/json（vendored 单头）做 JSON 解析、序列化和快照存储（2026-10-15 起，原为无第三方依赖的手写解析器）：

- JSON 对象、数组、字符串、整数、浮点数、布尔值和 `null`；
- 重复对象键、非法 JSON 和文件错误可诊断；
- 必需顶层结构检查：`schema_version`、`baseline`、`mode`、`timing`、`motor`、`can`、`sbus`、`persistence`；
- 电机映射及 PID 字段存在性和类型检查；
- ATOM-03 真实消费键的类型/基础范围检查；
- `null` 必需值拒绝为未实现配置；
- 查询只读内存快照，不在 `IConfigView` 查询方法中执行文件 I/O。

### 2.3 并发与热加载

- 采用 `shared_mutex` 保护快照；
- 读查询支持并发调用；
- 成功加载和运行时更新递增 `configVersion()`；
- 字符串指针在下一次配置版本变化前保持快照内存有效；
- 更新回调在版本变更后调用。

### 2.4 运行时更新与持久化

- 只有 `WEB` 来源且处于 `MANUAL` 或 `DEBUG` 模式才允许更新；
- 类型不匹配、关键配置越界、未知键、未加载配置均拒绝；
- 运行时更新分别返回 `effective` 与 `persisted`；
- 文件损坏不自动修复，不执行版本迁移、钳位或掉电保护；
- PID 支持按电机 ID 读取和整体写入；
- PID 字段包括 `kp`、`ki`、`kd`、`integral_limit`、`output_limit`、`control_period_ms`、`enabled`；
- 预留变量范围为 `reserved_001` 至 `reserved_100`，类型为 `int32`，支持缺失键首次写入。

## 3. CMake 集成

本目录仍不定义 `project()`、`main()` 或可执行目标。当前目标：

- `atom_15_config_persistence`；
- `Marshland::atom_15_config_persistence`；
- 有 `Src` 时生成 STATIC 库；
- 链接 `Marshland::PublicHeaders`；
- 编译标准、ARM 工具链、警告和全局选项由上层 CMake 管理。

## 4. 验证结果

已使用既有 ARM 交叉编译构建目录执行增量构建：

```text
wsl cmake --build cmake-build-ubuntu-arm \
  --target marshland marshland_gateway_tests marshland_public_contracts_check \
  --parallel 4
```

结果：

- `atom_15_config_persistence` 编译通过；
- `marshland` 链接通过；
- `marshland_gateway_tests` 链接通过；
- `marshland_public_contracts_check` 通过；
- 未执行 clean-first 或全量重编译。

## 5. 当前限制与后续风险

1. 首版实现时没有 `Config/dart_control.json` 样例；当前已有初始配置，2026-10-14 已加入 CAN 通信超时键并完成静态检查。本次按用户要求不执行 ARM 程序，真实加载、PID 重启恢复和损坏文件行为不宣称已通过运行测试。
2. ~~当前实现是轻量 JSON 解析器~~（2026-10-15 已解决）：配置校验已切换为编译期嵌入的 `Rule/ConfigSchema.schema.json` + pboettch json-schema-validator，完整执行 `additionalProperties`、枚举、正则、数组与范围约束；仅保留 Schema 无法表达的薄语义层，见 §11。
3. `effective` 更新与普通文件写入发生在同一更新操作中；文件写入失败时内存值仍然生效，并明确返回 `effective=true`、`persisted=false`。未实现自动重试，符合“不额外承诺”的设计边界。
4. `ChangeCallback` 是本模块提供的窄事件回调；当前 `Public` 与 `Design/LifecycleEvents.md` 没有已落地的配置变更事件公共端口，因此没有擅自修改 `Public` 或复用状态事件接口。
5. 其他 Atom 当前只有 ATOM-03 存在真实 C++ `IConfigView` 查询实现；其余模块的配置键消费仍主要停留在规范/计划文档层面。
6. 未新增上层装配对象图注册代码，因为上层 `Entry.cpp` 和 `AppCoordinator` 的最终 ATOM-15 实例注入位置不属于本目录，且修改上层文件不在本次工作范围。

## 6. 文件变更

- 修改：`PLAN.md`；
- 新增：`Inc/atom15_config/config_store.hpp`；
- 新增：`Src/config_store.cpp`；
- 修改：本目录 `CMakeLists.txt`，补充 `Marshland::PublicHeaders` 依赖。

## 7. CAN 通信超时配置（2026-10-14）

### 7.1 已确认契约

- 键：`timing.can_comm_timeout_ms`，JSON `timing` 对象内的整数，单位 ms。
- 初始值：200；范围：1～6000（包含边界）；必须存在。
- 缺失、`null`、类型错误或越界时加载失败，不自动回填 200，不钳位。
- 允许运行时热修改，复用现有 WEB + MANUAL/DEBUG 授权和候选快照校验路径。
- 校验拒绝时活动值、版本、回调与文件均不改变；接受后分别报告 `effective` 与 `persisted`。
- ATOM-15 不实现 CAN 超时判断、安全输出或系统状态切换。

### 7.2 修改范围

| 文件 | 修改 |
|---|---|
| `Config/dart_control.json` | 在 `timing` 中写入 `can_comm_timeout_ms: 200`，保留 6 个电机的映射 |
| `Design/Config.md` | 明确范围、必需性、初始值和职责边界，版本更新为 1.1 |
| `Rule/ConfigSchema.md` | 补齐键契约与热修改授权，版本更新为 1.3 |
| `Rule/ConfigSchema.schema.json` | 补齐 `timing.required`、`maximum: 6000`、`default: 200`；`default` 仅为注释性初始值，不执行缺失回填 |
| `Atoms/ATOM-15_Config_Persistence/Src/config_store.cpp` | 加入必需整数及 1～6000 校验，timing 诊断使用完整点分路径 |
| `Tests/Atom15ConfigTests.cpp` | 补齐有效配置样例，新增 `config.can_comm_timeout`；覆盖加载、查询、更新拒绝、版本/回调、重启恢复及写入失败结果；修正既有数组查询与 reload 测试样例 |
| `Tests/IntegrationTests.cpp` | 为现有有效配置样例补齐新增必需键 |
| 本文件 | 记录契约、接口用法及验证边界 |

本次未修改 Public 接口、CAN 实现或顶层 CMake，也未修改已有的其他工作区变更。

### 7.3 外部使用

消费方只依赖 `IConfigView`，必须检查查询状态：

```cpp
std::int64_t timeout_ms = 0;
const auto status = config.getInt("timing.can_comm_timeout_ms", timeout_ms);
// Use timeout_ms only when status == marshland::config::ConfigStatus::OK.
```

持有 `ConfigStore&` 的配置更新层可调用：

```cpp
const auto result = store.updateRuntime(
    "timing.can_comm_timeout_ms", std::int64_t{500},
    marshland::atom15::UpdateSource::WEB,
    marshland::atom15::UpdateMode::MANUAL);
```

`DEBUG` 同样允许；其他来源/模式拒绝。调用方必须分别检查 `accepted`、`effective`、`persisted`。
消费方通过变更回调或 `configVersion()` 变化重新读取，更新其自身运行参数；仅缓存初始化值的模块不会自动获得新值。

### 7.4 静态验证与限制

- 使用既有 `cmake-build-ubuntu-arm` 增量编译，`atom_15_config_persistence`、`marshland_atom15_tests` 和 `marshland_integration_tests` 均编译/链接通过。
- JSON 语法检查及 timing 子 Schema 的 11 项静态校验通过：1、200、6000 有效；缺失、0、-1、6001、字符串、非整数小数、布尔值、null 被拒绝。
- 测试代码仅编译，未运行任何 ARM 二进制、未进行 ARM 模拟执行；不宣称动态测试通过。新增用例入口为 `marshland_atom15_tests config.can_comm_timeout`，未变更顶层 CTest 用例列表。
- 初始配置仍为 6 个电机。加入超时键时发现完整 Schema 的 `motor[].role` 枚举落后于初始配置；该既有问题在随后获得用户授权后单独修正，见 §8。
- `Config/dart_control.json` 当前被 Git 忽略规则排除；本次已修改本地文件，但它不会出现在普通 `git diff` 中。

## 8. 六电机 Schema 同步（2026-10-14）

- 根据现有 `Config/dart_control.json` 及 `Design/Config.md` §5.1，将 `motor[].role` 更新为 `charge_right`、`charge_left`、`reload_right`、`reload_left`、`yaw`、`force`。
- 同步 `Rule/ConfigSchema.md` 的电机编号说明并升版为 1.4；设计已明确六电机映射，本次不改变设计参数。
- 仅更新角色枚举、说明和本记录；不修改初始 JSON、C++ 代码、PID 参数、CAN 实现或构建文件；不新增固定数组顺序、固定数量或其他校验约束。
- 静态验证完成，21 项检查全部通过：现有初始配置通过完整 JSON Schema 校验；六角色有效，旧的 `charge`、非法角色、错误类型被拒绝；数组重排仍有效，原有 PID 和 CAN 超时约束仍生效。
- 六电机 Schema 同步阶段记录的初始配置 SHA256 为 `049AD5C2994619E8F2BC2DB335A8250163C035AB241BD9EB9BBBC1E3077B02A0`；后续本节新增配置已按当前需求更新该文件。本次不运行 ARM 程序或模拟器，也未编译程序。

## 9. 舵机通道与蓄力速度配置（2026-10-14）

### 9.1 已确认契约

- 新增顶层 `servo.channels[]`，当前写入两项：`FEED` 使用 PCA9685A 通道 4，`FIRE` 使用 PCA9685A 通道 5。
- 保留既有编号语义：FEED 舵机 `id=5`；FIRE 舵机编号仍待定，配置中写 `id=null`；FIRE 待机角度和动作角度也保持 `null`。
- 新增顶层 `charge`：`charge.charge_speed_rpm` 和 `charge.home_speed_rpm`，均为带方向符号的整数，初始/默认值均为 5000 rpm。
- 两个新增顶层对象和字段均纳入加载校验；缺失、类型错误或舵机 FEED/FIRE 通道不符合契约时加载失败。
- ATOM-15 只负责配置解析、校验、查询和持久化，不实现蓄力控制、回零动作或舵机动作逻辑。

### 9.2 修改范围

| 文件 | 修改 |
|---|---|
| `Config/dart_control.json` | 新增 `charge`，写入两个 5000 rpm；新增 `servo.channels[]`，写入 FEED=4、FIRE=5 |
| `Design/Config.md` | 新增蓄力速度参数；将 §9.2 的实际通道更新为 FEED=4、FIRE=5；保留 FIRE 编号和角度待定 |
| `Rule/ConfigSchema.md` | 新增 `charge.*` 键表，更新 `servo.channels[]` 语义和顶层示例，版本升为 1.5 |
| `Rule/ConfigSchema.schema.json` | 将 `charge`、`servo` 加入必需顶层结构；增加两个 rpm 字段约束和 FEED/FIRE 通道约束 |
| `Src/config_store.cpp` | 允许并校验 `charge`、`servo` 顶层键及其字段，拒绝错误通道/缺失资源 |
| `Tests/Atom15ConfigTests.cpp` | 样例增加新配置，并验证两个 rpm 与 FEED/FIRE 通道读取 |
| `Tests/IntegrationTests.cpp` | 样例增加新配置 |
| 本文件 | 记录配置语义和实现边界 |

### 9.3 验证边界

- 本次只进行 JSON/Schema 静态检查和 ARM 交叉编译，不运行 ARM 二进制，不运行模拟器。
- `charge` 的默认值指初始配置和 Schema 默认元数据；配置文件显式写入两个字段，ATOM-15 不对缺失字段静默回填。

## 10. 蓄力/舵机/变力补充配置（2026-10-14）

- P1：`timing.charge_max_action_ms` 从 30 修正为 30000。
- P2：新增必需 `timing.charge_home_max_action_ms = 25000`。
- P3：`sbus.functions[]` 新增 `manual_charge_left -> CH8` 与 `manual_charge_right -> CH9`；既有 `manual_charge -> CH2` 保留。
- P4：左右独立配置键仍未实施。ATOM-08 规划给出两种方案但本轮未收到选择；继续使用共用 `charge_speed_rpm/home_speed_rpm`，左右实时独立性由 CH8/CH9 目标承担。
- 可选项：新增 `charge.target_range = 15000`，用于对称 `-15000..+15000 rpm` 目标范围校验。
- 舵机配置补齐 `servo.pca9685.*`、`servo.model.*`、`servo.strategy.*`、`timing.iic_comm_timeout_ms`，以及 `thread.iic_access/iic_priority`。
- 变力配置新增 `force.center_deadband_rpm = 0` 和 `force.center_action = ZERO_TORQUE`；`ZERO_SPEED` 不作为当前合法值。
- ATOM-15 校验同步覆盖新增必需对象、字段、枚举、数组范围、CH2/CH8/CH9 映射和 Q-13 中位处置。
- 为适配 ATOM-07 的 `getIntAt(..., "", ...)` 标量数组读取，ATOM-15 现在支持对 `array<int>` 直接读取元素值；通道范围和角度范围可由消费者正常读取。

| 版本 | 日期 | 说明 |
|---|---|---|
| 0.4 | 2026-10-11 | 获得 Coding 授权后完成首版 ATOM-15 配置快照、IConfigView、生命周期适配器、JSON 读写、PID/预留变量持久化及 ARM 增量构建验证。 |
| 0.5 | 2026-10-14 | 加入必需 CAN 通信超时配置，初始值 200 ms，范围 1～6000 ms，支持现有授权下的热修改；同步配置规范/Schema/测试样例；仅完成静态验证和 ARM 增量编译。 |
| 0.6 | 2026-10-14 | 同步六电机 Schema 角色枚举和键名规范，保留初始配置与代码；执行完整 Schema 静态校验。 |
| 0.7 | 2026-10-14 | 新增 `servo.channels[]` 的 FEED=4/FIRE=5 通道配置与 `charge` 两个 5000 rpm 速度键；同步 Schema、规则、设计配置、ATOM-15 校验和测试样例。 |
| 0.8 | 2026-10-14 | 落实蓄力 P1/P2/P3、可选目标范围、完整 ATOM-07 舵机/IIC/线程配置，以及变力 Q-13 中位死区与零力矩处置；P4 左右配置键仍待用户选定。 |
| 0.9 | 2026-10-14 | 将 `force` 和 `thread` 加入必需配置结构；完成 Q-13 校验、标量数组查询修复，并通过初始配置与集成样例 Schema 静态校验及 ARM 增量编译。 |
| 1.0 | 2026-10-15 | JSON 解析/序列化切换为 nlohmann/json 3.12.0；配置校验切换为编译期嵌入的 ConfigSchema（pboettch json-schema-validator 2.4.0）+ 薄语义层；修复 getStringAt 标量字符串数组读取；同步测试样例。 |

## 11. JSON 库与 Schema 校验化改造（2026-10-15）

### 11.1 实现结构

- JSON 解析/序列化：nlohmann/json 3.12.0（`ThirdParty/nlohmann/json.hpp`，`Marshland::ThirdPartyNlohmannJson`，PRIVATE）。重复键诊断由解析回调 `DuplicateKeyTracker` 保留；超过 INT64_MAX 的正整数按无符号溢出视为非法整数。
- 配置校验：`Rule/ConfigSchema.schema.json` 经上层 CMake 编译期嵌入为 `marshland::atom15::kConfigSchemaJson`（`Marshland::ConfigSchemaEmbed`），由 pboettch json-schema-validator 2.4.0（`ThirdParty/pboettch`，`Marshland::ThirdPartyJsonSchemaValidator`，PRIVATE）首次加载时编译并缓存；错误经 `SchemaErrorCollector` 逐项转为 `ConfigDiagnostic`（JSON Pointer 路径）。
- 薄语义层（Schema 表达不了且不冲突）：`motor[].id`/`node` 去重、`sbus.valid_min≤valid_max`、`pulse_max>pulse_min`、角度/通道区间有序且含 4/5 通道、PCA9685 prescale 公式。
- `load()` 与 `updateRuntime/writePid/writeReserved` 共用同一两段式 `validate()`；热更新现在受完整 Schema 约束（const/枚举/范围均生效）。
- 以 Schema 为准后删除的旧规则：`isKnownRootKey` 白名单、`frame_bytes==25`、timing 额外必需键、摇杆 0–2047、`estop_other_servos` 仅 HOLD。`sbus.frame_bytes` 的 25 字节约束自此完全由 ATOM-03 消费侧承担。
- `getStringAt(..., "", ...)` 现支持标量字符串数组元素直读（与 `getIntAt` 行为对齐）。

### 11.2 验证边界

- WSL ARM 交叉增量编译通过：`marshland_thirdparty_json_schema_validator`、`atom_15_config_persistence`、`marshland`、`marshland_atom15_tests`、`marshland_integration_tests`、`marshland_gateway_tests`、`marshland_public_contracts_check`。
- 静态核对：`Config/dart_control.json` 与两个测试样例逐节对照嵌入 Schema 通过（必需键、`additionalProperties:false`、`contains+allOf`、枚举、pattern）。
- 未运行任何 ARM 二进制（WSL 无 qemu-aarch64）；pboettch 对本 Schema 用到的 `$defs` 引用、`contains+allOf`、联合类型、`patternProperties` 的运行期行为需在 ARM 主机或 qemu 上执行 `marshland_atom15_tests`/`marshland_integration_tests` 后方可宣称通过。新增手工用例入口 `marshland_atom15_tests config.store_load_invalid_schema`，未变更顶层 CTest 用例列表。
