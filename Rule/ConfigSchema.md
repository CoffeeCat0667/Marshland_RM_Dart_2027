# dart_control.json 配置键名规范（Config Schema）

## 0. 文档定位

| 项目 | 内容 |
|---|---|
| 文档名称 | 运行时配置键名规范与全量键表 |
| 配置文件 | `./Config/dart_control.json` |
| 参数唯一来源 | `Design/Config.md` —— **数值与语义以该文档为准，本文件只规定键名、类型、单位与结构** |
| 机器可读形式 | `Rule/ConfigSchema.schema.json`（JSON Schema 2020-12）—— **形式化契约，供 `ATOM-15` 直接校验**。本文件是它的中文可读说明；两者不一致时以 schema 文件为准并修订本文件 |
| 消费者 | `ATOM-15`（解析、校验、提供查询）与 `ATOM-03`～`ATOM-19`（经 `IConfigView` 取值） |
| 关联文档 | `Rule/AtomIntegration.md`（接入规范）、`Atoms/ATOM-15_Config_Persistence/PLAN.md`（实现方案） |
| 版本 | 1.11 |
| 编制日期 | 2026-09-27 |
| 更新日期 | 2026-10-16 |
| 状态 | **正式**。修改本文件必须同步 `Design/Config.md` 与 `ConfigSchema.schema.json`；冲突时以 `Design/Config.md` 的数值为准并修订本文件 |

**为什么需要本文件**：`Design/Config.md` 定义了约 200 项参数的**数值**，但没有规定 JSON 键名。
键名未定时，各模块只能各自猜测（`sbus.port` 与 `sbus_device` 并存），必然产生互不兼容的配置——
这正是必须避免的"第二真相"。本文件是键名的**唯一来源**。

---

## 1. 命名规则（规范性）

### 1.1 键名格式

1. **必须**使用小写 `snake_case` 的**点分层**结构：`<域>.<对象>.<属性>`。
2. **禁止**使用大写字母、连字符、空格或中文。
3. 层级深度**应**不超过 4 层。
4. 同一概念在全文件**必须**使用同一名称，**禁止**出现同义异名。

### 1.2 域（第一层）的封闭集合

| 域 | 覆盖范围 | 对应 `Config.md` |
|---|---|---|
| `baseline` | 产品与运行基线（总发数、控制优先级、时间基准等） | §2 |
| `mode` | 功能模式启用开关与初始状态 | §3 |
| `timing` | 运行时序、周期、超时 | §4 |
| `motor` | 电机映射、控制、PID、编码器 | §5.1–5.4 |
| `charge` | 蓄力机构速度参数 | §5.5 |
| `force` | 变力机构 | §5.6 |
| `reload` | 换弹机构 | §5.7 |
| `feed` | 供弹机构 | §5.7 |
| `fire` | 发射机构 | §5.7 |
| `can` | SocketCAN 与 CAN 报文 | §6 |
| `sbus` | SBUS 输入与通道映射 | §7 |
| `gpio` | GPIO、限位开关、压力传感器预留 | §8 |
| `servo` | PCA9685A、IIC、舵机资源与策略 | §9 |
| `vision` | 视觉系统 | §10 |
| `http` | HTTP 服务、路由、错误处理 | §11 |
| `websocket` | WebSocket 连接与消息 | §12 |
| `persistence` | 持久化与配置加载 | §13 |
| `fault` | 故障等级、恢复与安全规则 | §14 |
| `thread` | 线程、并发与调度 | §15 |
| `log` | 日志与诊断 | §16 |

**新增域必须**先修订本文件。

### 1.3 数组与元素

1. 重复性实体（电机、CAN 报文、舵机、SBUS 通道）**必须**表示为**对象数组**。
2. 元素内**禁止**重复数组索引作为键；索引由数组位置给出。
3. 固定编号的实体**必须**显式携带其编号字段（如 `motor[].id`、`servo[].id`），
   以使数组顺序不影响语义。
4. 引用其它元素的**必须**用该实体的 `id`，**禁止**用数组下标。

```jsonc
"motor": [
  { "id": 1, "node": "0x201", "bus": "can0", "role": "charge_right", "target_unit": "rpm" },
  { "id": 2, "node": "0x202", "bus": "can0", "role": "charge_left",  "target_unit": "rpm" },
  { "id": 3, "node": "0x203", "bus": "can0", "role": "reload_right", "target_unit": "rpm" },
  { "id": 4, "node": "0x204", "bus": "can0", "role": "reload_left",  "target_unit": "rpm" },
  { "id": 5, "node": "0x205", "bus": "can0", "role": "yaw",          "target_unit": "rpm" },
  { "id": 6, "node": "0x206", "bus": "can0", "role": "force",        "target_unit": "rpm" }
]
```

### 1.4 值类型与写法

| 类型 | 写法 | 说明 |
|---|---|---|
| `int` | JSON number（无小数点） | 计数、ID、周期毫秒数 |
| `uint` | JSON number，≥ 0 | 同上但语义非负 |
| `bool` | `true` / `false` | **禁止**用 0/1 或字符串 |
| `str` | JSON string | 枚举值、设备名、路径 |
| `float` | JSON number | 带小数的物理量 |
| `enum` | JSON string，取值见 §1.5 | 大小写敏感，**必须**与表中列出值完全一致 |
| `array<T>` | JSON array | 见 §1.3 |

### 1.5 枚举值的统一写法

**必须**与 `Design/Config.md` / `Design/Interface.md` 中已定义的枚举字面量一致，
**禁止**另造别名或改变大小写：

| 枚举族 | 合法字面量（来源） |
|---|---|
| 控制模式 | `DISABLED` `TORQUE` `VELOCITY` `POSITION` `CURRENT`（`Config.md` §5.2） |
| 控制源 | `NONE` `SBUS` `WEB` `AUTO`（`Interface.md` §6.1） |
| 系统模式 | `UNINITIALIZED` `INITIALIZING` `INIT_FAILED` `STANDBY` `MANUAL` `AUTO_FIRE` `DEBUG` `CALIBRATION` `RECOVERY` `SOFTWARE_ESTOP` `FAULT`（`Interface.md` §6.2） |
| 故障等级 | `INFO` `WARNING` `SEVERE` `ESTOP`（`Config.md` §14.2） |
| 方向/符号 | 用 `int` 正负号表达，**禁止**造 `"CW"`/`"CCW"` 之类字符串 |
| 舵机动作组 | `lift` `lower`（`Config.md` §9.3） |

### 1.6 单位与量纲

1. **必须**在键名或所属对象中明确单位，且与 `Config.md` 的"单位"列一致。
2. 单位后缀规则：`_ms`（毫秒）、`_us`（微秒）、`_s`（秒）、`_rpm`（转每分）、
   `_ma`（毫安）、`_c`（摄氏度）、`_px`（像素）、`_deg`（度）、`_count`（个数）。
3. **禁止**混用单位（例如同时存在 `_ms` 与裸数字毫秒）。
4. `Config.md` 中用 `±` 表达的双向限幅，**必须**写成单个带符号 int（如 `15000` 表示 ±15000），
   并在文档注明其为对称限幅；**禁止**拆成 `_min`/`_max` 两键，除非 `Config.md` 明确非对称。
5. 位率/采样点等**必须**与 `Config.md` 保持原始表达（如 `"1Mbps"`、`87.5`）。

### 1.7 与本工程既有约定的几条禁令

1. **禁止**为本文件未列出的键取名；新增键必须**先**修订本文件与 `Design/Config.md`。
2. **禁止**为 `Design/Config.md` 中标记 `[待定]` 的项臆造数值或键名。
   `[待定]` 项**必须**登记为 `null` 或整段省略，并由 `ATOM-15` 在加载时报告为"未接入"。
3. **禁止**让同一语义存在两个键（例如 `timing.can_rx_period_ms` 与 `can.rx_period_ms`）。
4. 本文件的键名对 `ATOM-15` 的 `IConfigView` 查询**即是**公开 API：
   改名属于**破坏性变更**，必须同步全部消费方与本文件版本号。

---

## 2. 全量键表

> 约定：`ID` 列为 `IConfigView` 的查询键（点分路径）。
> 数组元素用 `[]` 表示，其内部键相对于元素根。
> "来源"列指向 `Design/Config.md` 的章节。**数值一律以该章节为准，本表不重复抄录。**

### 2.1 `baseline.*` —— 产品与运行基线（`Config.md` §2）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `baseline.auto_total_shots` | int | 发 | 自动流程总发数，固定 4 |
| `baseline.continuous_extra_interval_ms` | int | ms | 连续发射额外固定间隔，0 |
| `baseline.repeat_charge_per_shot` | bool | — | 每发是否重复蓄力 |
| `baseline.repeat_force_per_shot` | bool | — | 每发是否重复变力 |
| `baseline.fire_priority_order` | array&lt;str&gt; | — | 实际发射控制优先级，`["SBUS","WEB","AUTO"]` |
| `baseline.estop_overrides_all_sources` | bool | — | 软件急停高于所有普通控制源 |
| `baseline.estop_trigger_sources` | array&lt;str&gt; | — | 急停触发入口，`["WEB","SBUS"]` |
| `baseline.hardware_estop_added` | bool | — | 是否新增硬件急停，固定 `false` |
| `baseline.estop_confirm_requires_reinit` | bool | — | 急停恢复是否重新初始化，固定 `false` |
| `baseline.stop_fire_default` | bool | — | 停止发射标志默认值，`true` |
| `baseline.stop_fire_setter` | str | — | 置位方，`WEB` |
| `baseline.stop_fire_clear_paths` | array&lt;str&gt; | — | 清除方集合，`["VISION","MAIN_ON_TIMEOUT","WEB"]` |
| `baseline.stop_fire_scan_state` | str | — | 扫描时机，`FIRE_READY` |
| `baseline.initial_control_source` | enum | — | 初始控制源，见 §1.5 |
| `baseline.yaw_arrived_type` | str | — | YAW 到位信号类型，`bool` |
| `baseline.yaw_arrived_initial` | bool | — | YAW 到位信号初值，`false` |
| `baseline.yaw_arrived_clearer` | str | — | 清除方，`MAIN_CONTROLLER` |
| `baseline.yaw_arrived_clear_moment` | str | — | 清除时机，`AFTER_FIRE_ACTION` |
| `baseline.pressure_sensor_enabled` | bool | — | 压力传感器本版本启用，`false` |
| `baseline.time_base` | str | — | 时间基准，`MONOTONIC` |

### 2.2 `mode.*` —— 运行模式与初始状态（`Config.md` §3）

`mode.enabled.*`（bool，全部表示该模式是否启用）：

| ID | 说明 |
|---|---|
| `mode.enabled.initialization` | 初始化 |
| `mode.enabled.standby` | 待机 |
| `mode.enabled.auto_fire` | 自动发射 |
| `mode.enabled.manual` | 手动控制 |
| `mode.enabled.debug` | 调试 |
| `mode.enabled.calibration` | 校准 |
| `mode.enabled.auto_recovery` | 一般故障自动恢复 |
| `mode.enabled.init_failed` | 初始化失败 |
| `mode.enabled.fault` | 故障 |
| `mode.enabled.software_estop` | 软件急停 |

`mode.initial.*`（系统初始状态）：

| ID | 类型 | 说明 |
|---|---|---|
| `mode.initial.system_mode` | enum | 系统模式初值，`UNINITIALIZED` |
| `mode.initial.control_source` | enum | 控制源初值，`AUTO` |
| `mode.initial.current_shot` | int | 自动流程当前发次初值，0 |
| `mode.initial.total_shots` | int | 自动流程总发次，4 |
| `mode.initial.recovery_fire_count` | int | 自动恢复发射动作次数，3 |
| `mode.initial.recovery_final_mode` | enum | 恢复完成后状态，`MANUAL` |
| `mode.initial.auto_fire_state` | enum | 自动流程状态初值，`IDLE` |
| `mode.initial.estop_active` | bool | 软件急停状态初值，`false` |
| `mode.initial.fault_active` | bool | 故障状态初值，`false` |
| `mode.initial.stop_fire` | bool | 停止发射标志初值，`true` |
| `mode.initial.yaw_arrived` | bool | YAW 到位信号初值，`false` |

### 2.3 `timing.*` —— 运行时序（`Config.md` §4）

所有周期/超时键名**必须**带 `_ms` 或 `_s` 后缀。

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `timing.max_control_cycle_ms` | int | ms | 系统最长控制周期（100） |
| `timing.motor_loop_period_ms` | int | ms | 电机闭环周期 |
| `timing.pid_period_ms` | int | ms | PID 控制周期 |
| `timing.can_io_period_ms` | int | ms | CAN 接收/发送周期 |
| `timing.gpio_sample_period_ms` | int | ms | GPIO 采样周期 |
| `timing.sbus_receive_period_ms` | int | ms | SBUS 接收周期 |
| `timing.estop_response_max_ms` | int | ms | 软件急停响应上限 |
| `timing.zero_torque_send_period_ms` | int | ms | 零力矩发送周期 |
| `timing.reload_lift_run_s` | int | s | 换弹抬起**运行时间**（3）。左右电机抬起转速目标均成功提交后计时；到期将两台目标转速设为 0，均成功提交后判定抬起到位 |
| `timing.reload_lower_run_s` | int | s | 换弹放下**运行时间**（3）。判据同上 |
| `timing.feed_angle2_hold_ms` | int | ms | ~~供弹角度二保持时间（100）~~ **v1.9 起作废**：供弹机构不再用角度控制，保持时间改由 `feed.hold_ms` 承载 |
| `timing.feed_to_reload_lower_delay_s` | int | s | 供弹后换弹放下等待时间（2） |
| `timing.fire_angle_hold_ms` | int | ms | 发射角度保持时间（100） |
| `timing.sbus_loss_timeout_ms` | int | ms | SBUS 失联判定时间（200） |
| `timing.charge_time_s` | int | s | 蓄力时间 X（30） |
| `timing.charge_max_action_ms` | int | ms | 蓄力最大动作时间（30000） |
| `timing.charge_home_max_action_ms` | int | ms | 蓄力回零最大动作时间（25000） |
| `timing.force_time_s` | int | s | 变力时间 X（30） |
| `timing.feed_max_action_ms` | int | ms | 供弹机构最大动作时间 |
| `timing.force_max_action_ms` | int | ms | 变力机构最大动作时间 |
| `timing.yaw_adjust_max_action_ms` | int | ms | YAW 调整最大动作时间（2000） |
| `timing.fire_max_action_ms` | int | ms | 发射机构最大动作时间 |
| `timing.can_comm_timeout_ms` | int | ms | 必需；初始值 200；范围 1～6000；允许热修改，WEB 来源且仅 MANUAL/DEBUG 模式；缺失时加载失败；ATOM-15 不实现超时后的业务处理 |
| `timing.iic_comm_timeout_ms` | int | ms | IIC 通信超时（200） |
| `timing.websocket_io_timeout_ms` | int | ms | WebSocket 单次读写/消息处理超时 |
| `timing.vision_status_update_timeout_ms` | int | ms | 视觉系统状态更新超时（200） |
| `timing.vision_no_response_timeout_ms` | int | ms | 视觉系统无响应超时（30000） |
| `timing.max_position_error_px` | int | px | 最大位置误差（10，视觉内部判据） |
| `timing.max_current_ma` | int | mA | 最大电流或力矩（对称，±15000） |

**说明**：`Config.md` §4 的"蓄力最大动作时间"与"蓄力时间 X"数值相同（30 s），
但语义不同（完成判据 vs 超时判据），**必须**保留两个独立键。

### 2.4 `motor[].*` —— 电机、编码器与 PID（`Config.md` §5.1–5.4）

#### 2.4.1 映射（`motor[]`，`Config.md` §5.1）

| 元素键 | 类型 | 单位 | 说明 |
|---|---|---|---|
| `id` | int | — | 电机标识（1 右蓄力、2 左蓄力、3 右换弹、4 左换弹、5 YAW、6 变力）；映射见 `Config.md` §5.1 |
| `role` | enum str | — | `charge_right` / `charge_left` / `reload_right` / `reload_left` / `yaw` / `force`；与 `Config.md` §5.1 的六电机机构对应 |
| `bus` | str | — | SocketCAN 接口名，`CAN0` |
| `node` | str | — | CAN 节点 ID，十六进制字符串 `"0x201"` 形式 |
| `control_mode` | str | — | 控制方式，`CLOSED_LOOP_PID` |
| `encoder_source` | str | — | 编码器来源，`CAN` |
| `target_unit` | str | — | 目标单位，`rpm` |

`Config.md` §5.1 的"其他电机"行为 `[待定]` → **整段省略**，不建占位元素。

#### 2.4.2 控制配置（`motor.control.*`，`Config.md` §5.2）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `motor.control.supported_modes` | array&lt;enum&gt; | — | 支持的控制模式（§1.5） |
| `motor.control.start_stop_strategy` | str | — | 电机启停策略，`PID` |
| `motor.control.output_limit` | int | — | 输出限幅（对称 ±15000） |
| `motor.control.current_limit_ma` | int | mA | 电流/力矩限幅（对称 ±15000） |
| `motor.control.target_slew_limit` | int | — | 目标值变化限速（对称 ±15000） |
| `motor.control.offline_timeout_ms` | int | ms | 电机离线判定（200） |
| `motor.control.feedback_timeout_ms` | int | ms | 电机反馈超时（200） |
| `motor.control.error_code_map` | object | — | 错误码映射，按 `Docs` C620 资料 |
| `motor.control.temperature_limit_c` | int | ℃ | 电机温度阈值（75） |

**注意**：`Config.md` §5.2 的"目标值单位"为"按电机和机构分别配置"，
故单位记在各 `motor[].target_unit`，**不**在此处设全局键。

#### 2.4.3 PID（`motor[].pid.*`，`Config.md` §5.3）

| 元素键 | 类型 | 单位 | 说明 |
|---|---|---|---|
| `pid.kp` | int | — | Kp |
| `pid.ki` | int | — | Ki |
| `pid.kd` | int | — | Kd |
| `pid.integral_limit` | int | — | 积分限幅 |
| `pid.output_limit` | int | — | 输出限幅 |
| `pid.control_period_ms` | int | ms | 控制周期（20） |
| `pid.enabled` | bool | — | 启用状态 |

PID 参数属 `Config.md` §13.3 的**必须持久化内容**：运行期可经 WEB 修改并写回 JSON。

#### 2.4.4 编码器（`motor.encoder.*`，`Config.md` §5.4）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `motor.encoder.interface` | str | — | 接口，`CAN` |
| `motor.encoder.output_quantity` | str | — | 输出量，`rpm` |
| `motor.encoder.direction_sign` | str | — | 方向表示，`SIGNED`（正负号） |
| `motor.encoder.feedback_period_ms` | int | ms | 反馈周期（20） |
| `motor.encoder.feedback_timeout_ms` | int | ms | 反馈超时（200） |
| `motor.encoder.abnormal_abs_limit_rpm` | int | rpm | 异常值范围（绝对值上限，**7500**） |

> **数值权威**：`Config.md` §5.4 记「绝对值大于 **7500** rpm」判为异常，这是唯一权威值。
> 本文件 v1.0 曾声称此处与 `ATOM-02` BugLists 存在 2500 的冲突，**该说法是错误的、已撤回**：
> `Atoms/` 全域检索不到任何 2500 rpm 的编码器阈值，2500 仅出现在舵机脉宽（500~2500 µs）。
> 不存在待裁决的冲突。

### 2.5 `charge.*` —— 蓄力机构速度（`Config.md` §5.5）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `charge.charge_speed_rpm` | int | rpm | 正常蓄力恒定速度，含方向符号；默认值 5000 |
| `charge.home_speed_rpm` | int | rpm | 回零速度，含方向符号；默认值 5000 |
| `charge.target_range` | int | rpm | 蓄力目标对称范围（15000 表示 ±15000），可选 |

两个键均为带符号整数。默认值是初始配置值；配置文件中应显式写入，ATOM-15 不因缺失而静默创建隐含运行参数。

### 2.6 `force.*` —— 变力机构（`Config.md` §5.6）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `force.control_mode` | str | — | 控制方式，`CLOSED_LOOP_PID` |
| `force.target_default_source` | str | — | 目标值来源，`SBUS_CH12`（CH12 为变力专用映射：`(raw - 1024) * 2000 / 660`，`-2000`～`+2000` rpm，**不使用**通用 `x`；WEB 经 `force_set`） |
| `force.target_unit` | str | — | 目标单位，`rpm` |
| `force.target_range` | int | rpm | 允许范围（对称 ±15000） |
| `force.completion_criterion` | str | — | 完成判据，`TIMED`（执行 `timing.force_time_s`） |
| `force.calibration_method` | str | — | 校准方式，`MANUAL` |
| `force.has_dedicated_fault_handling` | bool | — | 是否有本机构专属异常处理，`false` |
| `force.center_deadband_rpm` | int | rpm | CH12 中位判定死区（≥0），默认 0 |
| `force.center_action` | enum str | — | 当前固定 `ZERO_TORQUE`；`ZERO_SPEED` 仅作未来扩展保留 |

### 2.7 换弹机构（双 M3508）与 `feed.*` / `fire.*` 舵机（`Config.md` §5.7）

`Config.md` §5.7 是换弹/供弹/发射机构参数表，与 §9.2 的舵机资源表配套。**以 §5.7 为准**。

#### 2.7.1 换弹机构（左右两台 M3508，**非舵机**）

> **更正（2026-10-14，Q-24）**：`Config.md` §5.7 已明确换弹机构为**左右两台 M3508**（右侧 id:3 / node `0x203`、左侧 id:4 / node `0x204`，见 §5.1），控制模式 `VELOCITY`；抬起左 +2000 / 右 −2000 rpm，放下左 −2000 / 右 +2000 rpm；按 `timing.reload_lift_run_s` / `timing.reload_lower_run_s`（各 3 s）运行后提交两台 0 rpm 并保持速度闭环（零转速目标 ≠ 零力矩）。
>
> **本文件 v1.6 及以前的「`reload[]` 四个舵机」键结构作废**：换弹不接入舵机；换弹不设置动作超时（3 s 为正常运行计时，不是故障超时）；原引用的 `timing.reload_*_settle_ms` 不存在。
>
> `Rule/ConfigSchema.schema.json` 的 `servo_mechanisms.reload[]` 与初始 JSON 仍为旧结构，其键结构迁移属 ATOM-10 / ATOM-15 接入范围，**待用户确认后另行实施**，不属本次文档更正。
>
> **键结构裁决（2026-10-15，用户确认）**：速度目标采用**单个幅值键** `reload.speed_rpm`，方向为消费模块内约定（抬起左 +幅值 / 右 −幅值，放下左 −幅值 / 右 +幅值），不再设左右分立键。

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `reload.speed_rpm` | int | rpm | 换弹速度目标**幅值**（2000，必须 >0）。方向约定：抬起左正右负、放下左负右正；数值权威为 `Config.md` §5.7 |

> 左右电机映射（id 3/4、node `0x203`/`0x204`）由 `motor[]` 数组持有（`role` = `reload_right` / `reload_left`），`reload.*` 不重复登记。
> `Rule/ConfigSchema.schema.json` 与初始 `Config/dart_control.json` 的迁移（旧 `servo_mechanisms.reload[]` 移除、`reload.speed_rpm` 入库）属 ATOM-15 接入范围，**待实施**，不属本次文档修订。

#### 2.7.2 供弹机构（`feed.*`）

> **更正（2026-10-15，用户裁决）**：供弹机构**硬件本身已变更，不再使用角度控制**。
> 原 `feed.angle1_deg` / `feed.angle2_deg` 键**作废**，角度也不再由 `servo.channels[]` 的
> `standby_deg` / `action_deg` 兜底；`Config.md` §5.7/§9.2 及 `Design/` 各文档中的供弹角度表述
> 同步失效，新机构动作参数待 `Design/` 修订后另行增补。供弹时序（换弹位保持时间、最大动作时间、
> 供弹后 2 s 放下等待）不变。

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `feed.servo_id` | int | — | 供弹执行器编号 5（新机构的执行器类型与通道归属待 `Design/` 修订确认） |
| `feed.hold_ms` | int | ms | 换弹位保持时间（100）；取代已作废的 `timing.feed_angle2_hold_ms` |

#### 2.7.3 发射机构（`fire.*`）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `fire.servo_id` | int | — | `x` 号舵机编号（`[待定]` → 省略） |
| `fire.standby_deg` | int | deg | 待机角度（`[待定]` → 省略） |
| `fire.fire_deg` | int | deg | 发射角度（`[待定]` → 省略） |
| `fire.hold_ms` | int | ms | 发射角度保持时间 |

### 2.8 `can.*` —— SocketCAN 与报文（`Config.md` §6）

#### 2.7.1 基础配置（`can.*`，`Config.md` §6.1）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `can.interface` | str | — | Linux 接口名，`CAN0` |
| `can.bitrate` | str | — | 波特率，`"1Mbps"` |
| `can.data_bits` | int | — | 数据位（8） |
| `can.sample_point_percent` | float | % | 采样点（87.5） |
| `can.sjw` | int | — | SJW（1） |
| `can.start_mode` | str | — | 总线启动方式，`CLASSIC_CAN_FD_OFF` |
| `can.rx_filter` | str | — | 接收过滤规则，`NONE` |
| `can.error_frame_handling` | str | — | 错误帧处理，`DISCARD` |
| `can.rx_task` | str | — | 接收任务，`DEDICATED_THREAD` |
| `can.tx_task` | str | — | 发送任务，`DEDICATED_THREAD` |
| `can.task_priority_order` | array&lt;str&gt; | — | 任务优先级，`["SAFETY","MANUAL","AUTO"]` |
| `can.task_sched_policy` | str | — | 调度策略，`SCHED_OTHER` |
| `can.protocol_source` | str | — | 协议来源说明（C620 资料） |

#### 2.7.2 报文表（`can.frames[]`，`Config.md` §6.2）

| 元素键 | 类型 | 单位 | 说明 |
|---|---|---|---|
| `id` | str | — | 报文类别标识，如 `motor_control_1`、`zero_torque_1` |
| `direction` | enum str | — | `TX`（主程序→电机）/ `RX`（电机→主程序） |
| `can_id` | str | — | CAN ID，十六进制字符串 `"0x200"` |
| `can_id_range` | array&lt;str&gt; | — | 反馈帧的 ID 范围，如 `["0x201","0x208"]` |
| `dlc` | int | — | 数据长度（8） |
| `fields` | array&lt;object&gt; | — | 字段列表，见下 |
| `byte_order` | str | — | 字节序，固定 `BIG_ENDIAN` |
| `unit` | str | — | 单位（`mA` / `rpm` / `deg` / `c`） |
| `period_ms` | int | ms | 周期 |
| `trigger` | str | — | 触发方式，`PERIODIC` / `ON_DEVICE_REPORT` |
| `timeout_ms` | int | ms | 超时阈值 |
| `on_failure` | str | — | 失败处理策略标识 |

`fields[]` 元素：

| 键 | 类型 | 说明 |
|---|---|---|
| `name` | str | 字段名（`rotor_angle` / `rotor_speed` / `torque_current` / `temperature` / `error_code`） |
| `type` | str | `int16` / `raw` |
| `slot` | int | 帧内槽位序号（0 基），仅控制帧使用 |

**字节序规则必须显式存在**：`byte_order` 固定 `BIG_ENDIAN`（`Config.md` §6.2 规则）。

#### 2.7.3 零力矩与安全发送（`can.zero_torque.*`，`Config.md` §6.3）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `can.zero_torque.send_mode` | str | — | 发送方式，`OPEN_LOOP` |
| `can.zero_torque.period_ms` | int | ms | 持续发送周期（20） |
| `can.zero_torque.frames` | array&lt;str&gt; | — | 使用的帧 `id`，`["zero_torque_1","zero_torque_2"]` |
| `can.zero_torque.motor_on_comm_fault` | str | — | 通信异常时电机行为，`ZERO_TORQUE` |
| `can.zero_torque.servo_on_comm_fault` | str | — | 通信异常时舵机行为，`HOLD` |
| `can.zero_torque.fire_servo_on_comm_fault` | str | — | 通信异常时发射舵机，`REJECT_CHANGE` |
| `can.silence_trigger` | str | — | 全机静默触发条件，`ZERO_TORQUE_SEND_FAILED` |

### 2.9 `sbus.*` —— SBUS（`Config.md` §7）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `sbus.receiver` | str | — | 接收设备，`ET16S` |
| `sbus.protocol` | str | — | 输入协议，`SBUS` |
| `sbus.serial_device` | str | — | 实际串口设备路径，例如 `/dev/ttyS10`；设备名由 `RuntimeContext` 传入 |
| `sbus.channel_count` | int | — | 通道总数（16） |
| sbus.channel_scale_x | int | — | 连续通道统一缩放变量，范围 1～10；所有连续 SBUS 通道共用 |
| `sbus.inverted` | bool | — | 是否反相，`true`（已经过硬件反向电路） |
| `sbus.receive_period_ms` | int | ms | 接收周期（20） |
| `sbus.loss_timeout_ms` | int | ms | 失联判定时间（200） |
| `sbus.frame_bytes` | int | — | 判定一帧的字节数（25） |
| `sbus.validate_frame_tail` | bool | — | 是否校验帧尾，`false` |
| `sbus.joystick_center` | int | — | 摇杆量中心（1024） |
| `sbus.joystick_range` | int | — | 摇杆量半幅（660，有效 364–1684） |
| `sbus.switch_values` | array&lt;int&gt; | — | 开关量取值 `[364,1024,1684]` |
| `sbus.valid_min` | int | — | 有效值下限（364） |
| `sbus.valid_max` | int | — | 有效值上限（1684） |
| `sbus.loss_handling` | str | — | 失联处理，`DEGRADE_TO_WEB` |
| `sbus.priority` | str | — | 控制优先级，`SBUS_GT_WEB_GT_AUTO` |

#### 2.8.1 功能映射（`sbus.functions[]`，`Config.md` §7.1）

| 元素键 | 类型 | 说明 |
|---|---|---|
| `name` | str | 功能名，如 `manual_mode_toggle`、`auto_mode`、`estop_trigger`、`estop_confirm`、`stop_fire`、`auto_start`、`manual_charge`、`manual_charge_left`、`manual_charge_right`、`manual_reload`、`manual_feed`、`manual_force`、`manual_yaw`、`manual_fire` |
| `channel` | str | 通道，如 `"CH6"`；`[待定]` 用 `null` |
| `signal_type` | enum str | `SWITCH` / `JOYSTICK` |
| `valid_range` | array&lt;int&gt; | 有效范围 |
| `neutral` | int | 中立/默认值 |
| `loss_behavior` | str | 失联行为标识 |

通道绑定与缩放约定（本次替代旧 CH7 确认规则和 CH13 保留约定）：

| 功能名 | 通道 | 公共数组下标 | 数值映射 |
|---|---|---|---|
| `manual_fire` | CH7 | `scaled_channels[6]` | `(raw - center) * x` |
| `manual_feed` | CH13 | `scaled_channels[12]` | `(raw - center) * x`，与 CH7 相同 |

- CH7/CH13 共用原始值有效范围和同一个 x，独立解析、独立输出；不进行持续帧数确认，也不直接产生发射/供弹动作。
- 推荐标定 364/1024/1684 对应 `-660*x`/0/`+660*x`；输出不是舵机角度或 PWM。
- `signal_type` 的 SWITCH/JOYSTICK 是输入标签，不改变 CH7/CH13 的通用缩放，不引入确认计数；不新增必填功能条目或配置键。
- `sbus.functions[]` 中一旦出现 `manual_fire` 或 `manual_feed`，其 channel 分别必须是 CH7、CH13，不能写成其他通道或 null。机器 Schema 同步约束这些绑定。
- CH12 仍使用专用 rpm 映射，只有 CH14～CH16 保留、不映射为动作。

**`[待定]` 通道必须写为 `null`**（当前：`sbus.estop_confirm`、`sbus.auto_start` 的通道待定）。
`ATOM-03` 遇到 `null` 时**必须**保持该功能未映射，**禁止**推断。

#### 2.8.2 控制源选择规则（`sbus.rules[]`，`Config.md` §7.2）

**规则表必须入 JSON**，以"条件 → 控制源"的结构化映射表达：

| 元素键 | 类型 | 说明 |
|---|---|---|
| `id` | enum str | 规则标识，取值见下 |
| `when` | object | 条件：`web_online` / `sbus_online` / `sbus_auto_mode` / `software_estop`，每项 `bool` 或 `null`（表示不关心） |
| `then_source` | enum str | 结果控制源（`NONE`/`SBUS`/`WEB`/`AUTO`），或 `OVERRIDE_ALL`（急停覆盖全部普通控制源） |
| `never_changes_fire_priority` | bool | 固定 `true`：控制源选择结果**不**改变实际发射授权优先级 |

`id` 的封闭取值（对应 `Config.md` §7.2 五行 + 急停覆盖）：

| id | when | then_source |
|---|---|---|
| `startup_no_web` | `web_online=false` | `AUTO` |
| `web_connected` | `web_online=true` | `WEB` |
| `sbus_online_auto_mode_true` | `sbus_online=true, sbus_auto_mode=true` | `AUTO` |
| `sbus_online_auto_mode_false` | `sbus_online=true, sbus_auto_mode=false` | `SBUS` |
| `sbus_lost` | `sbus_online=false` | `WEB` |
| `software_estop` | `software_estop=true` | `OVERRIDE_ALL` |

规则按数组顺序求值。`ATOM-02` 的 `StateArbitrator::SelectControlSource` 实现该判定；
**禁止**在代码中另行硬编码条件分支。

> **不变式**：`never_changes_fire_priority` 必须为 `true`。
> 控制源选择结果与实际发射授权优先级（`SBUS > WEB > 自动流程`）是**两个独立的量**，
> 混用会导致自动流程被误拒或 SBUS 检查被绕过（历史缺陷 `ATOM02-BUG-002`）。

### 2.10 `gpio.*` —— GPIO 与传感器（`Config.md` §8）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `gpio.limit_switch[]` | array | — | 限位开关，见下 |
| `gpio.sample_period_ms` | int | ms | 采样周期（20） |
| `gpio.default_level` | enum str | — | 未触发默认电平，`HIGH` |
| `gpio.active_level` | enum str | — | 输入有效电平，`LOW` |
| `gpio.charge_home_criterion` | str | — | 蓄力回零判据，`BOTH_LIMITS_LOW` |
| `gpio.debounce_ms` | int | ms | 去抖时间（20） |
| `gpio.report_on_change` | bool | — | 稳定跳变时上报 `sensor_state` |
| `gpio.report_max_delay_ms` | int | ms | 上报延迟上限（20） |
| `gpio.read_failure_handling` | str | — | 读取失败处理，`ENTER_FAULT` |
| `gpio.limit_trigger_action` | str | — | 限位触发动作，`STOP_RELATED_MOTOR` |

`gpio.limit_switch[]` 元素：

| 键 | 类型 | 说明 |
|---|---|---|
| `id` | int | 限位编号 1、2（对应蓄力电机 1、2） |
| `pin` | int? | GPIO 编号；`[待定]` → `null` |
| `motor_id` | int | 关联电机 ID |

压力传感器（`Config.md` §8.2）：本版本**不接入**，整段省略；仅保留 `baseline.pressure_sensor_enabled = false`。

### 2.11 `servo.*` —— PCA9685A、IIC 与舵机（`Config.md` §9）

#### 2.10.1 PCA9685A（`servo.pca9685.*`，`Config.md` §9.1）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `servo.pca9685.i2c_device` | str | — | IIC 设备，`/dev/i2c-7` |
| `servo.pca9685.address` | str | — | 器件地址，`"0x40"` |
| `servo.pca9685.channel_count` | int | — | 通道数（16） |
| `servo.pca9685.pwm_resolution_bits` | int | — | PWM 分辨率（12） |
| `servo.pca9685.pwm_frequency_hz` | int | Hz | PWM 频率（50） |
| `servo.pca9685.channel_range` | array&lt;int&gt; | — | 通道范围 `[0,15]` |
| `servo.pca9685.oe_active_level` | enum str | — | OE 电平，`LOW` |
| `servo.pca9685.access_mode` | str | — | 访问方式，`SERIALIZED` |
| `servo.pca9685.task_priority` | str | — | 任务优先级，`MEDIUM_HIGH` |
| `servo.pca9685.error_handling` | str | — | 错误处理，`ENTER_FAULT` |

#### 2.10.2 舵机型号与脉宽（`servo.model.*`）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `servo.model.name` | str | — | `MG996R` |
| `servo.model.type` | str | — | `180_DEGREE` |
| `servo.model.pulse_min_us` | int | us | 脉宽下限（500） |
| `servo.model.pulse_max_us` | int | us | 脉宽上限（2500） |
| `servo.model.angle_range_deg` | array&lt;int&gt; | deg | 角度控制范围 `[0,180]` |

#### 2.10.3 舵机资源（`servo.channels[]`，`Config.md` §9.2）

| 元素键 | 类型 | 单位 | 说明 |
|---|---|---|---|
| `id` | int? | — | FEED 舵机编号为 5；FIRE 舵机编号仍待定时写 `null` |
| `mechanism` | enum str | — | `FEED` / `FIRE`；本版本换弹使用双 M3508，不占用舵机通道 |
| `channel` | int | — | PCA9685A 通道；FEED 固定为 4，FIRE 固定为 5 |
| `standby_deg` | int? | deg | 待机/基准角度；`[待定]` → `null` |
| `action_deg` | int? | deg | 动作角度；`[待定]` → `null` |

当前已确定 PCA9685A 通道映射：`FEED=4`、`FIRE=5`。FIRE 的舵机编号、待机角度和发射角度仍为待定，继续写 `null`；ATOM-07/10/11 不得推断这些仍未确定的字段。

> **v1.9 注**：供弹机构硬件已变更、不再使用角度控制（见 §2.7.2 更正）。`servo.channels[]` 中
> `FEED` 行的 `standby_deg` / `action_deg` 固定为 `null`；供弹是否仍占用 PCA9685A 通道 4，
> 待 `Design/` 对新机构的修订确认。

#### 2.10.4 动作策略（`servo.strategy.*`，`Config.md` §9.3）

> **v1.9 删除**：`servo.strategy.reload_lift_group` / `servo.strategy.reload_lower_group` ——
> 换弹机构为双 M3508、不接入舵机（见 §2.7.1），旧四舵机模型的策略键作废移除。

| ID | 类型 | 说明 |
|---|---|---|
| `servo.strategy.feed_sequence` | str | `STANDBY_TO_ACTION_TO_STANDBY`（v1.9 起；原 `ANGLE1_TO_ANGLE2_TO_ANGLE1` 随供弹角度模型作废） |
| `servo.strategy.fire_sequence` | str | `STANDBY_TO_FIRE_TO_STANDBY` |
| `servo.strategy.estop_fire_servo` | str | `REJECT_ALL` |
| `servo.strategy.estop_other_servos` | str | `HOLD` |
| `servo.strategy.comm_fault_servos` | str | `HOLD` |
| `servo.strategy.recovery_other_servos` | str | `HOLD` |
| `servo.strategy.fire_servo_safe_hold` | str | `HOLD_CURRENT` |
| `servo.strategy.out_of_range_action` | str | `REJECT_AND_WARN`（`W-CMD-001`） |
| `servo.strategy.action_failure_action` | str | `FAIL_AND_BLOCK_FIRE`（`F-SERVO-001`） |

### 2.12 `vision.*` —— 视觉系统（`Config.md` §10）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `vision.transport` | str | — | 通信承载，`TCP` |
| `vision.listen_address` | str | — | 地址，`0.0.0.0` |
| `vision.port` | int | — | 端口（10001） |
| `vision.target_field` | str | — | YAW 目标字段名，`yaw_speed` |
| `vision.target_unit` | str | — | 目标单位，`rpm` |
| `vision.target_range` | int | rpm | 目标范围（对称 ±16000） |
| `vision.feedback_field` | str | — | 反馈字段名，`yaw_callback` |
| `vision.feedback_unit` | str | — | 反馈单位，`rpm` |
| `vision.arrived_field` | str | — | 到位字段名 |
| `vision.arrived_type` | str | — | 到位字段类型，`bool` |
| `vision.arrived_validity` | str | — | 有效性规则标识，`CURRENT_SHOT_ONLY` |
| `vision.status_update_timeout_ms` | int | ms | 状态更新超时（200） |
| `vision.no_response_timeout_ms` | int | ms | 无响应超时（30000） |
| `vision.yaw_adjust_timeout_ms` | int | ms | YAW 调整时间节点（2000，**不终止等待**） |
| `vision.error_codes` | object | — | 错误码映射（`F-VISION-001` / `W-VISION-002`） |
| `vision.message_has_sequence` | bool | — | 消息是否带序号，`false` |
| `vision.shot_scoped` | bool | — | 目标与到位是否以发次为作用域，`true` |

### 2.13 `http.*` —— HTTP（`Config.md` §11）

#### 2.12.1 服务（`http.*`，`Config.md` §11.1）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `http.listen_address` | str | — | 监听地址，`0.0.0.0` |
| `http.port` | int | — | 端口（10000） |
| `http.static_resource_provider` | str | — | 静态资源提供方，`MAIN_PROGRAM` |
| `http.charset` | str | — | 字符编码，`UTF-8` |
| `http.request_body_limit_bytes` | int | B | 请求体上限（64 KiB） |
| `http.response_body_limit_bytes` | int | B | 响应体上限（1 MiB） |
| `http.request_timeout_ms` | int | ms | 请求处理超时（200）；HTTP 请求超时的唯一配置键（原 `timing.http_request_timeout_ms` 已合并删除，见 §6 v1.10） |
| `http.auth_required` | bool | — | 用户登录，`false` |
| `http.permission_levels_required` | bool | — | 权限等级，`false` |
| `http.confirm_for_normal_ops` | bool | — | 普通操作二次确认，`false` |
| `http.estop_recovery_requires_confirm` | bool | — | 急停恢复必须人工确认，`true` |

#### 2.12.2 路由（`http.routes[]`，`Config.md` §11.2）

| 元素键 | 类型 | 说明 |
|---|---|---|
| `method` | str | HTTP 方法；`GET/POST` 用数组 `["GET","POST"]` |
| `path` | str | 路径，如 `/api/state` |
| `purpose` | str | 用途标识 |
| `control_source` | enum | 控制源，见 §1.5 |
| `status_restriction` | str | 状态限制标识 |

**路由的字段名与错误结构**（`Config.md` §11.2/§11.3）**必须**以结构化字段入 JSON：
`http.routes[]` 携带 `request_fields` / `response_fields` / `status_restriction` / `is_emergency_path`，
`http.error_response_template` 与 `http.error_codes` / `http.status_mapping` 分别表达统一错误结构与状态码映射。
`ATOM-16` 只消费这些字段，**禁止**在代码中另行硬编码路由表或错误码表。

#### 2.12.3 动作集（`http.manual_actions[]`，`Config.md` §11.4）

| 元素键 | 类型 | 说明 |
|---|---|---|
| `action` | str | 动作名，如 `charge_start` |
| `requires_target` | bool | 是否需要 `target` |
| `target_unit` | str? | 目标单位（如 `rpm`、`deg`） |
| `allowed_modes` | array&lt;enum&gt; | 允许模式 |

`action` 取值集合是 `Config.md` §11.4 的**唯一来源**，新增动作必须同步该表与 `Interface.md` §9.2。

### 2.14 `websocket.*` —— WebSocket（`Config.md` §12）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `websocket.listen_address` | str | — | 监听地址，`0.0.0.0` |
| `websocket.port` | int | — | 端口（10002）；独立于 HTTP（10000）与视觉（10001） |
| `websocket.message_version` | str | — | 消息版本，`"1.0"` |
| `websocket.heartbeat_period_ms` | int | ms | 心跳周期（1000） |
| `websocket.connection_timeout_ms` | int | ms | 连接超时（3000） |
| `websocket.reconnect_backoff_s` | array&lt;int&gt; | s | 退避序列 `[1,2,4,8,16]` |
| `websocket.reconnect_backoff_max_s` | int | s | 退避上限（30） |
| `websocket.reconnect_jitter` | bool | — | 是否加入随机抖动，`true` |
| `websocket.control_over_ws_allowed` | bool | — | 是否允许经 WS 提交控制请求，`false` |
| `websocket.multi_client_mode` | str | — | 多客户端仲裁，`READ_ONLY_SUBSCRIBE` |
| `websocket.push_period_ms` | int | ms | 状态推送周期（20）；推送周期的唯一配置键（原 `timing.websocket_push_period_ms` 已合并删除，见 §6 v1.10） |
| `websocket.envelope_fields` | array&lt;str&gt; | — | 信封字段名顺序 |
| `websocket.message_types` | array&lt;str&gt; | — | 服务端消息类型集合 |

**消息类型的 `payload` 字段定义**（`Config.md` §12.3）**必须**入 JSON：
`websocket.message_types[]` 逐类型给出 `payload_fields`；
`websocket.heartbeat_message` 给出心跳信封结构；`websocket.envelope_fields` 给出信封字段顺序。
`ATOM-17` **禁止**在代码中另行硬编码消息类型表或 payload 字段。

### 2.15 `persistence.*` —— 持久化（`Config.md` §13）

| ID | 类型 | 说明 |
|---|---|---|
| `persistence.file_format` | str | `JSON` |
| `persistence.directory` | str | `./Config` |
| `persistence.file_name` | str | `dart_control.json` |
| `persistence.write_mode` | str | `PLAIN_FILE_WRITE` |
| `persistence.power_loss_protection` | bool | `false` |
| `persistence.corruption_recovery` | bool | `false` |
| `persistence.missing_param_recovery` | str | `USE_BUILTIN_DEFAULT` |
| `persistence.version_migration` | bool | `false` |
| `persistence.out_of_range_handling` | str | `NONE` |
| `persistence.effective_timing` | str | `HOT_RELOAD_IMMEDIATE` |
| `persistence.pid_edit_allowed_modes` | array&lt;enum&gt; | `["MANUAL","DEBUG"]` |
| `persistence.pid_edit_control_source` | enum | `WEB` |
| `persistence.pid_abnormal_criterion` | str | PID 异常判定标识 |
| `persistence.pid_abnormal_action` | str | PID 异常处理标识 |
| `persistence.load_failure_action` | str | 配置加载失败处理 |
| `persistence.load_timing` | str | 加载时机，`INITIALIZATION` |
| `persistence.reserved_variable_count` | int | 预留变量数量（100） |
| `persistence.reserved_variable_pattern` | str | 命名模式，`reserved_{index:03d}` |
| `persistence.reserved_variable_type` | str | 类型，`int32` |

**必须持久化的内容**（`Config.md` §13.3）：电机 PID、蓄力时间、变力时间、发射角度参数、
YAW 目标参数、变力参数、校准参数、100 个预留变量。
其中 PID 与预留变量的**值**存于其自身键（`motor[].pid.*`、`reserved_001`…`reserved_100`），
不在此处重复。

### 2.16 `fault.*` —— 故障与安全（`Config.md` §14）

| ID | 类型 | 说明 |
|---|---|---|
| `fault.table[]` | array | 故障表，见下 |
| `fault.safety_rules` | object | 已确定安全规则（`Config.md` §14.1），**建议代码实现** |

`fault.table[]` 元素：

| 键 | 类型 | 说明 |
|---|---|---|
| `name` | str | 故障名（如 `can_bus_error`） |
| `code` | str | 故障码，**必须**取自 `Config.md` §14.2（如 `F-CAN-001`） |
| `trigger` | str | 触发条件标识 |
| `severity` | enum | 等级，见 §1.5 |
| `auto_recovery` | bool | 是否自动恢复 |
| `recovery_action` | str | 恢复动作标识 |
| `requires_confirm` | bool | 是否需人工确认 |

> **禁止**新增 `Config.md` §14.2 之外的故障码。`W-VISION-002` 属正常回退路径，
> `auto_recovery` 必须为 `false`。

### 2.17 `thread.*` —— 线程与调度（`Config.md` §15）

| ID | 类型 | 说明 |
|---|---|---|
| `thread.motor_loop_task` | str | `DEDICATED_THREAD` |
| `thread.can_rx_task` | str | `DEDICATED_TASK` |
| `thread.can_tx_task` | str | `DEDICATED_TASK` |
| `thread.iic_access` | str | `SERIALIZED` |
| `thread.state_sync` | str | `ZERO_COPY` |
| `thread.control_change_action` | str | 控制权变化处理标识 |
| `thread.feed_wait_behavior` | str | 供弹等待期间行为标识 |
| `thread.estop_variable_type` | str | `BOOL` |
| `thread.estop_check_before_action` | bool | 执行器普通动作前急停检查，`true` |
| `thread.safety_action_exceptions` | array&lt;str&gt; | 安全动作例外清单 |
| `thread.motor_loop_priority` | str | `HIGH` |
| `thread.state_machine_priority` | str | `HIGH` |
| `thread.iic_priority` | str | `MEDIUM_HIGH` |
| `thread.network_priority` | str | `LOW` |
| `thread.sched_policy` | str | `SCHED_OTHER` |
| `thread.stack_size_bytes` | int | 线程栈大小（1 MiB） |

### 2.18 `log.*` —— 日志与诊断（`Config.md` §16）

| ID | 类型 | 单位 | 说明 |
|---|---|---|---|
| `log.level` | enum | — | 日志等级，`INFO` |
| `log.output_directory` | str | — | `./Logs/` |
| `log.stdout_echo` | str | — | 标准输出同步策略，`CRITICAL_ONLY` |
| `log.format` | str | — | `JSON_LINES` |
| `log.timestamp_format` | str | — | `UTC_ISO8601_MS` |
| `log.rotation_enabled` | bool | — | 是否轮转，`true` |
| `log.max_files` | int | — | 最多保留文件数（10） |
| `log.max_file_size_bytes` | int | B | 单文件上限（10 MiB） |
| `log.max_total_size_bytes` | int | B | 最大存储量（100 MiB） |
| `log.fault_history_limit` | int | — | 历史故障保留数量（1000） |
| `log.fault_history_policy` | str | — | `RING_LATEST_FIRST` |
| `log.flush_policy` | str | — | 刷新策略标识 |
| `log.required_events` | array&lt;str&gt; | — | 必须记录的事件清单 |
| `log.fields` | array&lt;str&gt; | — | 日志字段顺序 |

---

## 3. 顶层结构示例

```jsonc
{
  "schema_version": 1,
  "baseline":   { "auto_total_shots": 4, "stop_fire_default": true, "...": "..." },
  "mode":       { "enabled": { "...": true }, "initial": { "system_mode": "UNINITIALIZED" } },
  "timing":     { "max_control_cycle_ms": 100, "charge_time_s": 30, "...": "..." },
  "motor": [
    { "id": 1, "role": "charge_right", "bus": "CAN0", "node": "0x201",
      "pid": { "kp": 3, "ki": 0, "kd": 0, "integral_limit": 16000,
               "output_limit": 16000, "control_period_ms": 20, "enabled": true } },
    { "id": 2, "role": "charge_left", "bus": "CAN0", "node": "0x202", "pid": { "...": "..." } },
    { "id": 3, "role": "reload_right", "bus": "CAN0", "node": "0x203", "pid": { "...": "..." } },
    { "id": 4, "role": "reload_left", "bus": "CAN0", "node": "0x204", "pid": { "...": "..." } },
    { "id": 5, "role": "yaw", "bus": "CAN0", "node": "0x205", "pid": { "...": "..." } },
    { "id": 6, "role": "force", "bus": "CAN0", "node": "0x206", "pid": { "...": "..." } }
  ],
  "charge":      { "charge_speed_rpm": 5000, "home_speed_rpm": 5000 },
  "can":        { "interface": "CAN0", "bitrate": "1Mbps", "frames": [ "..." ] },
  "sbus":       { "serial_device": "/dev/ttyS10", "functions": [ "..." ] },
  "gpio":       { "limit_switch": [ { "id": 1, "pin": null, "motor_id": 1 } ] },
  "servo":      { "channels": [
    { "id": 5, "mechanism": "FEED", "channel": 4 },
    { "id": null, "mechanism": "FIRE", "channel": 5 }
  ] },
  "vision":     { "port": 10001, "target_field": "yaw_speed" },
  "http":       { "port": 10000, "routes": [ "..." ] },
  "websocket":  { "port": 10002, "message_version": "1.0" },
  "persistence":{ "directory": "./Config", "file_name": "dart_control.json" },
  "fault":      { "table": [ "..." ] },
  "thread":     { "sched_policy": "SCHED_OTHER" },
  "log":        { "level": "INFO", "output_directory": "./Logs/" },
  "reserved_001": 0
}
```

`reserved_001` ～ `reserved_100` 为顶层扁平键（`persistence.reserved_variable_*` 规定其命名与类型）。

---

## 4. 校验规则（`ATOM-15` 必须实现）

1. **必须**校验 `schema_version` 存在且为受支持版本。
2. **必须**校验每个已知键的类型与范围（范围取自 `Design/Config.md`）。
3. 缺失的**必需**键（硬件映射、安全参数）→ `initialize()` 返回失败，系统进入 `INIT_FAILED`。
4. 缺失的**可选**键 → 使用 `Config.md` 定义的内置默认值。
5. `null` 值（`[待定]` 项）**必须**报告为"未接入"，**禁止**替换为猜测值。
6. 未知键**应**报告为警告而非失败，以容忍前向兼容的新增项。
7. 单字段错误**必须**能够独立报告（键名 + 原因），不得只报"配置错误"。

> **形式化校验入口**：`Rule/ConfigSchema.schema.json` 以 JSON Schema 2020-12 表达以上规则。
> `ATOM-15` **应**优先使用该 schema 驱动校验（结构、类型、枚举、`const`、范围、必需项、
> `additionalProperties: false` 拒绝未知键），再叠加 `Config.md` 的语义性范围校验。
> 这样做可避免把键表在代码里再抄写一遍——那会立刻产生第三份真相。

---

## 5. 待裁决事项

| # | 事项 | 说明 |
|---|---|---|
| 1 | **舵机参数归口** | 供弹侧已于 v1.9 裁决：供弹机构硬件变更、不再用角度控制，`feed.angle*_deg` 与 `servo.channels[]` 的 FEED 角度字段一并作废。剩余问题仅限 FIRE：`Config.md` §5.7 与 §9.2 两处记发射舵机角度，需确认 §9.2 是否仅冗余 |
| 2 | **`schema_version` 与 `persistence.version_migration` 的关系** | 前者为 `const 1`，后者为 `false`；需确认两者语义不冲突（前者是结构版本，后者是迁移策略） |
| 3 | **`Design/Config.md` 章节号复核** | 本文件的来源引用按编制时的章节号给出；`Design/Config.md` 已升版，若有章节号变动需逐条校正（数值引用不受影响） |
| 4 | **`servo_mechanisms` 与 `servo.channels` 的重复** | 换弹（v1.7）与供弹角度（v1.9）均已从键表移除，重复面收敛至 FIRE 的角度/通道两处登记；需确认哪一份是权威，避免同值两写 |

---

## 6. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-09-27 | 首次编制。规定键名格式、域封闭集合、数组与元素约定、值类型与枚举字面量、单位后缀、七条禁令；建立 `baseline`/`mode`/`timing`/`motor`/`force`/`reload`/`feed`/`fire`/`can`/`sbus`/`gpio`/`servo`/`vision`/`http`/`websocket`/`persistence`/`fault`/`thread`/`log` 全域键表；给出校验规则与 5 项待裁决。数值一律以 `Design/Config.md` 为准。 |
| 1.1 | 2026-10-10 | ① **撤回错误声明**：v1.0 声称编码器阈值存在 7500 vs 2500 冲突，经全仓检索 `Atoms/` 内不存在任何 2500 rpm 编码器阈值（2500 仅为舵机脉宽），该冲突不存在，权威值唯一为 7500。② 按"全入 JSON"决定改写：HTTP 路由字段与错误结构、WS 消息类型与 payload 字段、控制源选择规则表（`sbus.rules[]`）、故障安全规则（`fault.rules`）全部改为**必须**入 JSON 的结构化字段，并禁止代码另行硬编码。③ 新增形式化契约 `Rule/ConfigSchema.schema.json`（JSON Schema 2020-12）作为机器可读校验入口。④ 待裁决事项更新为 4 项（舵机归口、`schema_version` 语义、章节号复核、`servo_mechanisms` 与 `servo.channels` 重复）。 |
| 1.2 | 2026-10-11 | 新增 `sbus.channel_scale_x` 必填配置，类型为整数，范围 1～10；明确所有连续 SBUS 通道共用该缩放变量，并同步更新 JSON Schema。 |
| 1.3 | 2026-10-14 | 补齐 `timing.can_comm_timeout_ms` 的必需性、初始值 200、范围 1～6000 和热修改规则，并同步 JSON Schema 与初始配置。 |
| 1.4 | 2026-10-14 | 将 `motor[].role` 同步为现有六电机角色，修正电机标识说明；保持数组按 id 标识，不依赖数组顺序；不变更初始配置、PID 数值或其他校验约束。 |
| 1.5 | 2026-10-14 | 新增 `charge.charge_speed_rpm` 与 `charge.home_speed_rpm`，默认值均为 5000；确定 `servo.channels[]` 的 `FEED=4`、`FIRE=5` PCA9685A 通道映射，并保留 FIRE 舵机编号及角度字段的待定状态。 |
| 1.6 | 2026-10-14 | **统一 SBUS 变力通道标准**：`force.target_default_source` 由 `SBUS_CH3` 修正为 `SBUS_CH12`（CH12 为变力专用映射，不使用通用 `x`），并同步 `Rule/ConfigSchema.schema.json` 的取值约束。 |
| 1.7 | 2026-10-14 | 按 `Config.md` §5.7 更正 §2.7.1：换弹机构为**左右两台 M3508**，原「`reload[]` 四个舵机」键结构作废；§5 待裁决第 1 项章节号 §5.6 → §5.7。`servo_mechanisms.reload[]` 与初始 JSON 的键结构迁移待确认，未在本版实施。 |
| 1.8 | 2026-10-14 | 落实蓄力 P1/P2、SBUS CH8/CH9 映射、ATOM-07 完整舵机配置与 IIC/线程配置；新增变力 `center_deadband_rpm` 和固定 `center_action=ZERO_TORQUE`，`ZERO_SPEED` 仅保留为扩展说明。 |
| 1.9 | 2026-10-15 | 按用户裁决（经 ATOM-10 工作目录一次性例外授权修订本文件）：① §2.7.1 新增 `reload.speed_rpm` 单个幅值键（2000，方向为消费模块内约定：抬起左正右负、放下左负右正）；② §2.7.2 供弹机构硬件变更、不再使用角度控制，`feed.angle1_deg`/`feed.angle2_deg` 与 `timing.feed_angle2_hold_ms` 作废，保持时间由 `feed.hold_ms` 承载；③ 删除旧四舵机残留 `servo.strategy.reload_lift_group`/`reload_lower_group`，`servo.strategy.feed_sequence` 改为 `STANDBY_TO_ACTION_TO_STANDBY`；④ §2.10.3 注明 FEED 角度字段固定 `null`；⑤ 待裁决 #1/#4 收敛至 FIRE。**未同步** `ConfigSchema.schema.json` 与初始 `Config/dart_control.json`（不在本次授权范围；schema 与 JSON 的迁移待 ATOM-15 接入时实施）。 |
| 1.10 | 2026-10-16 | 双源超时键清理（经用户一次性例外授权修订本文件，全工程消费调查为依据）：① 删除零消费的 `timing.http_request_timeout_ms`，HTTP 请求超时唯一键为 `http.request_timeout_ms`；② 删除零消费的 `timing.websocket_push_period_ms`，推送周期唯一键为 `websocket.push_period_ms`；③ `timing.websocket_io_timeout_ms` 因 ATOM-17 语义独立（单次读写上限，transport 尚未兑现）暂留；④ vision 四键待 ATOM-12 落地时定型。已同步 `ConfigSchema.schema.json` 与初始 `Config/dart_control.json`。 |
| 1.11 | 2026-10-16 | 绑定 manual_fire=CH7、manual_feed=CH13，两者均通用缩放、无持续帧数确认，只有 CH14～CH16 保留；同步机器 Schema 的按功能名通道约束，不新增必填功能条目或配置键，保留 1.10 超时键清理等已有修订。 |


