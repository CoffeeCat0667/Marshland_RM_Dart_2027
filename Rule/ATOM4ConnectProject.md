# ATOM-04_CAN 工程接入说明

> 版本：1.0
> 日期：2026-10-12
> 适用模块：`Atoms/ATOM-04_CAN`
> 对照文档：`Rule/ATOM3ConnectProject.md`（ATOM-03 接入形态）

## 1. 接入目标

ATOM-04 负责 Config 指定 SocketCAN 接口上的单帧收发、原始帧定点投递、CAN 节点在线状态维护、
总线/节点错误事件发布，以及两组电机控制帧（`0x200` / `0x1FF`）的大端序编解码与
零力矩安全帧的开环周期发送。

模块不承担机构流程判断、PID 计算、控制权仲裁、发射授权，也不解析电机反馈字段，
不在模块内部实现 `main()`。

**分工边界（用户裁决）**：电机反馈帧 `0x201`~`0x206` 由 ATOM-04 **原样投递**给公共端口
`ICanFrameSink`，字段布局、单位与错误码映射全部归 ATOM-05；ATOM-04 不解析、不缓存、
不解释任何反馈字段。

## 2. CMake 接入方向

```text
Marshland 可执行
  -> Atoms/Public/AppCoordinator（顶层装配）
  -> Marshland::atom_04_can
  -> Marshland::PublicHeaders
```

要求：

1. 上层工程在 `add_subdirectory(Atoms)` **之前**建立 `Marshland::PublicHeaders`
   （见 `Atoms/Public/Headers/CMakeLists.txt` 的既有说明）。
2. 上层工程通过 `add_subdirectory(Atoms/ATOM-04_CAN)` 加入本模块。
3. 本模块提供目标 `atom_04_can` 与别名 `Marshland::atom_04_can`。
4. 本模块依赖 `Marshland::PublicHeaders`、C++ 标准库与目标 Ubuntu ARM 的
   SocketCAN/POSIX 接口；**不重复定义**架构、编译器、C++ 标准或编译选项。
5. 有 `Src/*.cpp` 时模块目标为 `STATIC`（参与增量编译），无源码时为 `INTERFACE` 占位。
6. 不定义 `project()`，不创建可执行文件。

本模块**未修改** `Atoms/CMakeLists.txt` 与上层 `CMakeLists.txt`：聚合行
`Marshland::atom_04_can` 与 `add_subdirectory(ATOM-04_CAN)` 在实现前已就位。

## 3. 公共契约

`Atoms/Public` 新增两个头文件，并已登记进 `Atoms/Public/Headers/CMakeLists.txt`：

### 3.1 `CanTypes.hpp`

- `CanFrame`：`can_id`（已去掉 SocketCAN 标志位）、`dlc`、8 字节 `payload`、单调时间戳；
- `CanError`：`OK / NOT_OPEN / PARAM_INVALID / SEND_FAILED / SEND_INCOMPLETE / RECEIVE_TIMEOUT /
  BUS_OFF / BUS_ERROR / FRAME_INVALID / UNKNOWN_ID / UNSUPPORTED`；
- `CanIoResult`：错误 + `errno`，用于把故障定位到具体系统调用；
- `CanNodeState`：`UNKNOWN / ONLINE / TIMEOUT / FAULT`；`CanNodeStatus`：节点状态快照；
- `CanBusStatus`：`open`、`last_error`、`last_errno`、`rx/tx/tx_fail/unknown_id` 计数；
- `CanEvent` / `CanEventKind`：`BUS_ERROR / NODE_STATE_CHANGE / ZERO_TORQUE_FAILED`；
- `CanNodeStatusStore`：互斥锁保护的整快照替换与复制读取；
- **槽位契约**：`queryMotorSlot()` / `motorSlotOf()` / `motorNodeOf()` 与
  `static_assert` 固化的 `0x200`/`0x1FF` 槽位映射。

### 3.2 `CanInterfaces.hpp`

- `ICanTransport`：`open(interface_name)` / `close()` / `receive(timeout)` / `send(frame)` /
  `busStatus()`；
- `ICanFrameSink`：**唯一**的反馈帧投递端口（ATOM-05 侧实现，装配层绑定）；
- `ICanEventSink`：总线/节点错误变化发布端口；
- `ICanNodeStatusReader`：节点/总线状态读取端口；
- `ICanSafetyNotifier`：零力矩发送失败上报端口（**声明存在，接线未落地**，见 §9 待接入点）。

公共头文件不包含 `AppCoordinator.hpp`、任何 ATOM 私有头，也不泄漏 `<linux/can.h>`。

## 4. 装配对象图

由 `Entry.cpp` 或等价顶层装配代码创建并持有以下对象，创建顺序必须保证依赖对象寿命更长：

```text
IConfigView 实现（ATOM-15）
CanNodeStatusStore                     <- 节点状态仓储
LinuxSocketCanTransport                <- ICanTransport 实现（SocketCAN）
ICanFrameSink 实现（ATOM-05 适配器）    <- 接收原始反馈帧
CanCore::Dependencies{transport, node_store, frame_sink, event_sink?, safety_notifier?}
CanCore(deps)
LifecycleAdapter(core, config_view)
```

`CanCore` 与 `LifecycleAdapter` 必须由装配层同时持有：适配器**只持引用**，内核寿命必须长于适配器。
`event_sink` 与 `safety_notifier` 为可选端口，允许为 `nullptr`（此时仍会记录事件路径内部计数，
但不得把"未接线"当成"已实现安全动作"）。

## 5. 生命周期注册

上层装配层把 `LifecycleAdapter` 注册到：

```text
RuntimeStep::InitializeSocketCan
```

注册时必须：

- 显式填写 `required`（CAN 为硬件链路，应为 `true`）；
- 检查 `addModule()` 返回值；
- 失败时读取 `lastRegistrationError()`；
- 不在 ATOM-04 内自行注册；
- 不把模块初始化结果伪装为注册信息。

`initialize()` 失败时如实返回 `success=false`，`error_message` 足以定位到配置键或设备：
例如 `"CAN config unusable: MISSING"` 或
`"SocketCAN open failed: BUS_OFF (errno=100, interface='can0')"`。

## 6. 配置接入

配置一律经 `marshland::config::IConfigView` 查询，键名遵循 `Rule/ConfigSchema.md`；
ATOM-04 不打开、不解析 JSON。**逐项检查 `ConfigStatus`**，任何非 `OK` 都会让初始化失败。

| 配置键 | 要求 | 用途 |
|---|---|---|
| `can.data_bits` | 必须为 8 | 经典 CAN 数据位 |
| `can.start_mode` | 必须为 `CLASSIC_CAN_FD_OFF` | 显式声明 FD 关闭 |
| `can.rx_filter` | 必须为 `NONE` | 不过滤 |
| `can.error_frame_handling` | 必须为 `DISCARD` | 不请求错误帧 |
| `can.zero_torque.send_mode` | 必须为 `OPEN_LOOP` | 零力矩不闭环确认 |
| `can.silence_trigger` | 必须为 `ZERO_TORQUE_SEND_FAILED` | 全机静默触发条件 |
| `timing.can_io_period_ms` | 正整数，1~60000 ms | 接收轮询切片与控制帧周期（20） |
| `timing.can_comm_timeout_ms` | 正整数，1~60000 ms | 节点离线判定（200） |
| `timing.zero_torque_send_period_ms` | 正整数，1~60000 ms | 零力矩持续发送周期（20） |
| `motor[].id` | 正整数 | 电机标识（1~6） |
| `motor[].node` | 十六进制字符串 `"0x201"` | CAN 节点；必须落在 `0x201`~`0x206` |

缺失、类型错误或越界一律使 `initialize()` 失败，**禁止臆造默认值**。

## 7. 数据与线程流

```text
can0 (SocketCAN, CAN_RAW, 经典 CAN)
  -> LinuxSocketCanTransport::receive(poll, 50 ms 切片)
  -> CanCore::receiveLoop（独立接收线程，SCHED_OTHER）
  -> ProtocolDispatcher::onReceived
       * 0x201~0x206 -> CanNodeStatusStore 刷新 ONLINE + 原样投递 ICanFrameSink（ATOM-05）
       * 其他 ID     -> UNKNOWN_ID，不投递、不臆造
  -> 超时检查（timing.can_comm_timeout_ms）-> NODE_STATE_CHANGE 事件

CanCore::transmitLoop（独立发送线程）
  * 绝对期限推进（Config.md §15），非"睡固定时长"
  * **控制帧周期**：发送当前**已锁存**的槽位值（两个控制帧各一次）。
    初值全 0，即 Config.md §6.2 的零力矩帧；ATOM-05 锁存控制量后，本周期发送的
    就是控制帧——不再被零力矩帧覆盖（见缺陷 D3）。
  * **零力矩安全帧**：0x200 / 0x1FF，8 字节全零，每 timing.zero_torque_send_period_ms
    一次，开环不确认；无视锁存值，安全动作优先。
  * 控制帧发送失败 -> 该帧锁存值立即归零 + 节流的 BUS_ERROR 事件
  * 零力矩发送失败 -> 强制锁存归零 + ZERO_TORQUE_FAILED 事件（不节流）+ ICanSafetyNotifier
```

线程停止流程：

```text
LifecycleAdapter::shutdown()
  -> CanCore::stop()
       -> running=false
       -> ICanTransport::close()   （shutdown+close，唤醒阻塞中的 poll/recv）
       -> receive_thread_.join() / transmit_thread_.join()
```

`stop()` 幂等、`noexcept`，且对"从未成功 `initialize()`"的调用安全。

## 8. 帧语义与槽位表

| 帧 ID | DLC | 槽位 → 电机 | 依据 |
|---|---|---|---|
| `0x200` | 8 | slot0=电机1/`0x201`（charge_right）、slot1=电机2/`0x202`（charge_left）、slot2=电机3/`0x203`（reload_right）、slot3=电机4/`0x204`（reload_left） | `Design/Config.md` §6.2 控制帧按 `0x201`~`0x204` 排列 |
| `0x1FF` | 8 | slot0=电机5/`0x205`（yaw）、slot1=电机6/`0x206`（force）、slot2/slot3 未使用（恒 `0x0000`） | `Design/Config.md` §6.2 第 256 行 |

- 多字节量一律**大端序**，`int16` 先写高字节；
- 零力矩帧 payload 严格为 8 个 `0x00`；
- 槽位规则实现为 `constexpr` 函数并带 `static_assert`，编译期即可判定；
- **不得**把 `0x1FF` 槽位 0 当作"电机 1"：该帧承载的是电机 5/6。

## 9. 安全边界与待接入点

ATOM-04 **不做**安全编排：

- 零力矩发送失败只做两件事：发布 `ZERO_TORQUE_FAILED` 事件、调用 `ICanSafetyNotifier`（若已注入）；
- 具体"全机静默"动作由装配层绑定到安全模块（ATOM-14）；
- 不直接调用 `AppCoordinator`、`StateArbitrator` 或任何 ATOM 私有接口；
- 发送路径不存在"失败转成功"分支。

**待接入点**：

| ID | 事项 | 当前状态 |
|---|---|---|
| P-01 | `ICanSafetyNotifier` 的实际接线（全机静默动作归属） | 仅声明；`AtomIntegration.md` §11 第 7 条端口未落地 |
| P-02 | `ICanEventSink` 到诊断/安全事件流（`ILifecycleEventSink`）的桥接 | 仅声明；`AtomIntegration.md` §11 第 4 条 |
| P-03 | 电机反馈字段解析与错误码映射归属 ATOM-05 | ATOM-04 已按裁决不实现 |
| P-04 | 电机控制帧电流值的实际提交方（ATOM-05 PID 输出） | `CanCore::latchMotorCurrents(frame_id, slots)` 已就绪（下一发送周期生效）；等待调用方接入 |

## 10. 与既有设计的偏差登记

| 项 | 设计/配置写法 | 实现取值 | 理由 |
|---|---|---|---|
| SocketCAN 接口名 | `Design/Config.md` §6.1 与 `Config/dart_control.json` 写 `"CAN0"` | `"can0"` | Linux 网络接口名区分大小写；用户明确裁决使用 `can0`。若目标机实际接口名为 `CAN0`，需先用 `ip link` 确认，否则 `initialize()` 会如实报 `NOT_OPEN`/`BUS_OFF` |
| 控制帧槽位来源 | `Rule/ConfigSchema.md` 定义了 `can.frames[].fields[].slot` | 取自 `Design/Config.md` §6.2 的固定映射常量 | `Config/dart_control.json` 不含 `can.frames[]`；用户裁决"新增声明即可"。未新增任何配置键，未修改 `ConfigSchema.md` |

## 11. 验证责任

本模块的验证脚本为 `Atoms/ATOM-04_CAN/.verify_atom04.sh`（WSL aarch64 环境执行）：

1. 语法/警告门：`g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror` 对 6 个源文件零告警；
2. SocketCAN 传输单独检查 `<linux/can.h>` 依赖；
3. ARM 目标编译：`aarch64-linux-gnu-g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -O2 -c` 全部 6 个源文件；
4. 逻辑测试执行：`Tests/Atom04CanTests.cpp` 覆盖 84 项断言；
5. 依赖方向：无 ATOM 私有头、无 `AppCoordinator`、无 `main()`。

当前结果：5/5 通过（含 aarch64 交叉编译与测试运行）。

## 12. 代码评审缺陷修复（2026-10-12）

评审发现并修复 7 个缺陷（详见 `Atoms/ATOM-04_CAN/PLAN.md` §14）。与接入方相关的两点：

1. **控制帧提交方式变更**：原来的 `sendMotorCurrents()`（立即发送、返回发送结果）
   已被取消。现为：

   ```cpp
   marshland::atom04::MotorSlotValues slots{/* slot0..slot3 电流 mA */};
   core.latchMotorCurrents(0x200U, slots);   // 锁存，下一次发送周期生效
   core.forceZeroCurrents();                 // 安全路径：全部槽位立即归零
   ```

   理由：控制帧与零力矩帧共用同一个 20 ms 周期。若提交端"立即发送"、
   而发送线程每周期再发一次零力矩帧，后者的全零载荷会覆盖前者，电机永远无法出力（缺陷 D3）。

2. **零力矩帧不再由发送线程无条件独占总线**：控制帧周期发送已锁存值（初值全零），
   零力矩安全帧仍按其周期独立发送且始终为全零。

## 13. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-12 | 首次编制。登记公共契约、对象图、`InitializeSocketCan` 注册点、配置键表、线程模型、槽位表、安全边界、待接入点与 `can0` 偏差。 |
| 1.1 | 2026-10-12 | 代码评审修复：控制帧改为"锁存 + 周期发送"，取消 `sendMotorCurrents()`；记录 7 项缺陷修复；更新 P-04 与线程流描述。 |
