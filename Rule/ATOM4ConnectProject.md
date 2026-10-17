# ATOM-04_CAN 工程接入说明

> 版本：2.0
> 日期：2026-10-16
> 适用模块：`../Atoms/ATOM-04_CAN_Motor_Control`
> 对照文档：`Rule/ATOM3ConnectProject.md`（ATOM-03 接入形态）、`Rule/AtomIntegration.md`

---

## 1. 接入目标与职责边界

ATOM-04 是 **CAN/SocketCAN 传输与路由层**。它负责：打开配置指定的 SocketCAN 接口、
单帧收发、把原始反馈帧按 ID 投递给上层、维护 CAN 节点在线状态、发布总线/节点事件、
按安全相位发送电机控制帧或零力矩安全帧。

**它不负责**：机构流程判断、PID 计算、控制权仲裁、发射授权、电机反馈字段解析。
`Design/Atomic/ATOM-04_CAN.md` 明确「CANAdapter 不承担机构流程判断或 PID 计算」。

### 1.1 与 ATOM-05 的分工（用户裁决）

- 电机反馈帧（`0x201`~`0x206`）由 ATOM-04 **原样投递**，ATOM-05 负责解析；
- ATOM-04 **不解析任何反馈字段**，不做单位换算、不解释错误码；
- 电机控制量由 ATOM-05（PID 输出）经 `CanCore::submit()` 提交。

### 1.2 禁止事项

- 不在本模块定义 `main()`、不创建可执行文件；
- 不设置架构、编译器、C++ 标准或编译选项；
- 不包含 `AppCoordinator.hpp` 或任何其它 ATOM 的私有头；
- 不自行注册生命周期（由装配层调用 `addModule()`）。

---

## 2. CMake 接入方向

```text
Marshland 可执行
  -> marshland_public / marshland_atoms（顶层聚合）
  -> Marshland::atom_04_can
  -> Marshland::PublicHeaders
```

要求：

1. 顶层工程必须在 `add_subdirectory(Atoms)` **之前**建立 `Marshland::PublicHeaders`
   （见 `Atoms/Public/Headers/CMakeLists.txt` 的说明）；
2. 由 `Atoms/CMakeLists.txt` 通过 `add_subdirectory(ATOM-04_CAN)` 引入；
3. 本模块提供目标 `atom_04_can` 与别名 `Marshland::atom_04_can`；
4. 本模块已声明对 `Marshland::PublicHeaders` 的依赖
   （`if(TARGET Marshland::PublicHeaders)` 块），依赖进入 CMake 依赖图；
5. 有 `Src/*.cpp` 时模块为 `STATIC` 库；不定义 `project()`。

---

## 3. 公共契约（Atoms/Public）

只有两个头文件属于本模块拥有的公共契约，均已登记进
`Atoms/Public/Headers/CMakeLists.txt`。

### 3.1 `CanTypes.hpp` —— 数据形状

| 类型 | 用途 |
|---|---|
| `CanFrame` | 与平台无关的帧镜像：`can_id`、`dlc`、`payload[8]`、`timestamp_us`（已剥离 SocketCAN 标志位） |
| `CanError` | 11 个值的强类型错误：`OK` `NOT_OPEN` `PARAM_INVALID` `SEND_FAILED` `SEND_INCOMPLETE` `RECEIVE_TIMEOUT` `BUS_OFF` `BUS_ERROR` `FRAME_INVALID` `UNKNOWN_ID` `UNSUPPORTED` |
| `CanResult` | 返回值载体：`failed` / `error` / `system_errno`（errno 用于把故障定位到具体调用） |
| `TorqueOutputMode` | **安全相位**：`ZERO_TORQUE`（只发全零帧）/ `CONTROL`（发已提交的控制量） |
| `CanNodeState` | `UNKNOWN` / `ONLINE` / `TIMEOUT` / `FAULT`（`FAULT` 为粘滞终态） |
| `CanNodeStatus` | 单节点状态：`motor_id` `node_id` `state` `has_received` `last_frame_time_us` `frame_count` |
| `CanCoreSnapshot` | 一次读取全部可观测状态：`running` `open` `last_error` `last_errno` `mode` `rx_count` `tx_count` `tx_fail_count` `unknown_id_count` `nodes[6]` `node_count` |
| `MotorSlotValues` | `std::array<std::int16_t,4>`：一次提交的 4 个电流值 |
| `MotorSlotQuery` | 槽位查询结果：`valid` `frame_id` `slot` |

常量（6 个）：`kMotorControlFrameIdA`(0x200)、`kMotorControlFrameIdB`(0x1FF)、
`kCanPayloadBytes`(8)、`kMotorSlotsPerFrame`(4)、`kMotorNodeMin`(0x201)、`kMaxMotorNodes`(6)。

函数（1 个）：`queryMotorSlot(node_id) -> MotorSlotQuery` —— **电机节点 → (控制帧 ID, 槽位)**，
编译期可判定。槽位映射由 `static_assert` 在编译期固化。

### 3.2 `CanInterfaces.hpp` —— 需要装配层提供的端口

| 端口 | 方法 | 谁实现 | 触发时机 |
|---|---|---|---|
| `ICanFrameSink` | `onCanFrame(const CanFrame&)` | **ATOM-05** | 每收到合法反馈帧（DLC=8 且 ID ∈ 0x201~0x206）逐帧调用，原样投递 |
| `ICanEventSink` | `onCanEvent(CanEventKind, CanError, detail)` | ATOM-18 / ATOM-14（**可选**） | `BUS_ERROR`（节流）、`NODE_STATE_CHANGE`（不节流）、`ZERO_TORQUE_FAILED`（不节流） |
| `ICanSafetyNotifier` | `onZeroTorqueSendFailed(CanError, int)` | **ATOM-14**（**可选**） | 零力矩帧发送失败时，触发全机静默（F-CAN-002） |

枚举 `CanEventKind`：`BUS_ERROR` / `NODE_STATE_CHANGE` / `ZERO_TORQUE_FAILED`。

> **不泄漏平台头**：`<linux/can.h>` 等只出现在 `Src/socketcan_transport.cpp`，
> 公共头只依赖 C++ 标准库。

---

## 4. 装配对象图

由装配层创建并持有，**创建顺序必须保证依赖对象寿命更长**：

```cpp
// 1) 平台传输实现（模块内提供）
marshland::atom04::LinuxSocketCanTransport transport;

// 2) 端口实现（由其它模块或装配层提供）
marshland::can::ICanFrameSink&      frame_sink;        // ATOM-05 实现（必需）
marshland::can::ICanEventSink*      event_sink;        // 可选，可为 nullptr
marshland::can::ICanSafetyNotifier* safety_notifier;   // 可选，可为 nullptr

// 3) 内核
marshland::atom04::CanCore core(
    marshland::atom04::CanCore::Dependencies{transport, frame_sink,
                                             event_sink, safety_notifier});

// 4) 生命周期适配器（只持引用，寿命必须短于 core）
marshland::atom04::LifecycleAdapter adapter(core, config_view);
```

**可选端口的语义**：`event_sink` / `safety_notifier` 为 `nullptr` 时对应回调不触发，
但内部计数与 `snapshot()` 仍然更新。**不得把"未接线"当作"已实现安全动作"**（见 §7 待接入点）。

---

## 5. 生命周期注册

注册到：

```text
RuntimeStep::InitializeSocketCan
```

```cpp
const bool ok = runtime.addModule(runtime::ModuleRegistration{
    runtime::RuntimeStep::InitializeSocketCan,
    /* required = */ true,              // CAN 为硬件链路
    std::move(adapter)});
if (!ok) {
    // 必须读取 lastRegistrationError()，禁止静默忽略
}
```

- 注册只允许发生在装配层；模块**不自行注册**、模块之间不互相注册；
- `required` 必须显式书写，不依赖默认值；
- `initialize()` 失败时如实返回 `success = false`，`error_message` 形如：
  `"CAN config unusable: MISSING"` 或
  `"SocketCAN start failed: BUS_ERROR (errno=19, interface='can0')"`；
- `shutdown()` 幂等且 `noexcept`，对"从未成功 initialize"的调用安全。

---

## 6. 配置接入

配置一律经 `marshland::config::IConfigView` 查询，键名遵循 `Rule/ConfigSchema.md`。
**逐项检查 `ConfigStatus`，任何非 `OK` 都使初始化失败**；**禁止臆造默认值**。

| 配置键 | 要求 | 用途 |
|---|---|---|
| `can.data_bits` | 必须为 8 | 经典 CAN 数据位 |
| `can.start_mode` | 必须为 `CLASSIC_CAN_FD_OFF` | 显式声明 CAN FD 关闭 |
| `can.rx_filter` | 必须为 `NONE` | 不过滤 |
| `can.error_frame_handling` | 必须为 `DISCARD` | 不处理错误帧内容（但订阅 `CAN_ERR_BUSOFF`） |
| `can.zero_torque.send_mode` | 必须为 `OPEN_LOOP` | 零力矩不闭环确认 |
| `can.silence_trigger` | 必须为 `ZERO_TORQUE_SEND_FAILED` | 全机静默触发条件 |
| `timing.can_io_period_ms` | 正整数，1~60000 ms | 接收轮询切片与控制帧周期（当前 20） |
| `timing.can_comm_timeout_ms` | 正整数，1~60000 ms | 节点离线判定（当前 200） |
| `timing.zero_torque_send_period_ms` | 正整数，1~60000 ms | 零力矩持续发送周期（当前 20） |
| `motor[].id` | 正整数 | 电机标识 |
| `motor[].node` | 十六进制字符串 `"0x201"` | CAN 节点；必须落在 `0x201`~`0x206` |

**六节点完整性（强制）**：配置必须**恰好包含 6 个电机**，且节点集合恰好覆盖
`0x201`~`0x206`（无缺号、无重复）。依据 `Design/Config.md` §6.2：
「全部 6 个电机纳入节点在线监测、反馈解析和零力矩覆盖」。缺失节点会导致该电机
永远收不到零力矩安全帧。

---

## 7. 数据与线程流

```text
can0（SocketCAN，CAN_RAW，经典 CAN，已订阅 CAN_ERR_BUSOFF）
  -> LinuxSocketCanTransport::receive()  poll(POLLIN, rx_period_ms)
  -> CanCore::receiveLoop()              独立接收线程
       -> CanCore::poll(now)             每轮无条件做超时判定（不得被帧流饿死）
       -> CanCore::dispatchFrame()
            * DLC != 8                -> FRAME_INVALID（不刷新状态、不投递）
            * ID 不在 0x201~0x206     -> UNKNOWN_ID（不投递、不臆造）
            * ID 未登记于配置          -> UNKNOWN_ID
            * 合法反馈                 -> 刷新 ONLINE + 原样投递 ICanFrameSink
            * 节点为 FAULT            -> 保持 FAULT（粘滞），但仍计数并转发

CanCore::transmitLoop()                  独立发送线程
  * 绝对期限推进（Config.md §15），非"睡固定时长"
  * 每个周期**只执行当前相位对应的那一种**（两类帧共用同一对 CAN ID，必须互斥）：
      ZERO_TORQUE -> sendZeroTorque()   发 0x200/0x1FF 全零帧（默认相位）
      CONTROL     -> sendLatchedFrames() 发已提交的控制量
  * 控制帧发送失败 -> 该帧锁存值立即归零 + 节流的 BUS_ERROR 事件
  * 零力矩发送失败 -> 强制锁存归零 + ZERO_TORQUE_FAILED 事件 + ICanSafetyNotifier
```

停止流程：

```text
LifecycleAdapter::shutdown()
  -> CanCore::stop()
       -> running = false
       -> transport.close()   （shutdown + close，打断阻塞中的 poll/recv）
       -> join(接收线程) / join(发送线程)
```

### 7.1 提交纪元与并发语义（重要）

`submit()` 在**同一临界区**内更新「载荷 + 模式 + 提交纪元」；发送侧在锁内取三者的
一致视图，并在**写出前复核**纪元。

**但一件事在结构上不可能做到**：撤销出力的那一瞬间，最多有一帧已经通过复核的控制帧
可能仍在写出路径上。因为 `transport.send()` **不在任何临界区内**，已提交到内核边界
的字节无法撤回。

这不是缺陷——**该帧的授权依据在它通过复核时仍然有效**（当时相位仍是 CONTROL），
撤权意图是在它通过之后才落地的。

因此：**`submit()` 的语义是"从下一个发送周期起生效"，不是"追溯撤销已开始的写入"。**
接入方若需要严格的零延迟切换，必须自行在输出层加一层门控，不能依赖本模块的时序。

---

## 8. 帧语义与槽位表

| 帧 ID | DLC | 槽位 → 电机节点 | 依据 |
|---|---|---|---|
| `0x200` | 8 | slot0=`0x201`（电机1）、slot1=`0x202`（电机2）、slot2=`0x203`（电机3）、slot3=`0x204`（电机4） | `Design/Config.md` §6.2 |
| `0x1FF` | 8 | slot0=`0x205`（电机5 YAW）、slot1=`0x206`（电机6 变力）、slot2/slot3 **保留，恒 0x0000** | `Design/Config.md` §6.2 第 256 行 |

- 多字节量一律**大端序**（int16 先高字节后低字节）；
- 零力矩帧 payload 严格为 8 个 `0x00`；
- **保留槽位强制清零**：即使调用方传入非零值，`0x1FF` 的 slot2/slot3 也恒为
  `0x0000`，绝不发出虚构的 `0x207`/`0x208` 数据；
- 槽位映射由 `CanTypes.hpp` 的 `static_assert` 在编译期校验。

---

## 9. 安全边界与待接入点

ATOM-04 只做"如实上报"，**不做安全编排**：

- 零力矩发送失败：发布 `ZERO_TORQUE_FAILED` 事件 + 调用 `ICanSafetyNotifier`（若已注入）；
- 具体"全机静默"动作由装配层绑定到 ATOM-14；
- 不直接调用 `AppCoordinator` / `StateArbitrator`；
- 发送路径**不存在"失败转成功"分支**。

| ID | 待接入点 | 当前状态 |
|---|---|---|
| P-01 | `ICanSafetyNotifier` 的实际接线（全机静默动作） | 仅声明；未接线时只有事件，**无安全动作** |
| P-02 | `ICanEventSink` 到诊断/故障事件流的桥接 | 仅声明 |
| P-03 | 电机反馈字段布局、单位、C620 错误码映射 | 归 ATOM-05；ATOM-04 按裁决不实现 |
| P-04 | 控制量的提交方（ATOM-05 PID 输出） | `submit()` 已就绪，等待调用方 |
| P-06 | **安全相位 → `submit()` 的驱动方** | 接口已就绪、默认 fail-safe；**未接线前系统只发零力矩（安全但不出力）** |
| P-07 | 设备错误 → `markFault()`；故障清除 → `markOnline()` | `markFault` 当前无调用方，`FAULT` 状态实际不可达 |

> **P-06 未接线前，电机不会出力。** 这是刻意的 fail-safe 默认值，联调前必须先接这一根线。

---

## 10. 与既有设计的偏差登记

| 项 | 设计/配置写法 | 实现取值 | 理由 |
|---|---|---|---|
| SocketCAN 接口名 | `Design/Config.md` §6.1 与 `Config/dart_control.json` 写 `"CAN0"` | **`"can0"`** | Linux 接口名区分大小写；用户明确裁决。若目标机接口名确为大写，初始化会如实报 `NOT_OPEN`/`BUS_ERROR` |
| 控制帧槽位来源 | `Rule/ConfigSchema.md` 定义了 `can.frames[].fields[].slot` | 取自 `Design/Config.md` §6.2 的固定映射常量 | `Config/dart_control.json` 不含 `can.frames[]`；用户裁决"新增声明即可"。**未新增任何配置键，未修改 ConfigSchema.md** |

---

## 11. 验证责任与证据边界（重要）

验证脚本：`../Atoms/ATOM-04_CAN_Motor_Control`（在 WSL 中执行）。

| 检查项 | 状态 |
|---|---|
| 基础告警门 `-Wall -Wextra -Wpedantic -Werror` | ✅ 5/5 |
| 严格告警（`-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast -Wformat=2 -Wold-style-cast -Wcast-qual -Wswitch-enum -Wunused-const-variable=2`） | ✅ 5/5 零输出 |
| **aarch64 交叉编译** `aarch64-linux-gnu-g++ -O2 -c` | ✅ 5/5 —— **这是编译证据，不是执行证据** |
| 具名用例执行 | ✅ 9/9 —— 在 **x86_64 宿主**上原生运行的二进制 |
| **ARM 环境执行** | ❌ **从未进行**。宿主为 x86_64，无 `qemu-aarch64`，且无 root 无法安装 |
| 结构边界（无 main / 无模块内 Tests / 无横向依赖 / CMake 已链公共头 / 公开面约束） | ✅ |

**结论：`can.*` 九个用例在 ARM 目标上的行为是"未验证"，不是"通过"。**

### 11.1 测试用例清单

| 用例 | 覆盖 |
|---|---|
| `can.slot_contract` | 槽位映射与边界（编译期 + 运行期） |
| `can.codec` | 大端序、控制帧构造、**保留槽强制清零**、零力矩帧全零、非法 ID 拒绝 |
| `can.receive_validation` | DLC≠8 拒绝、未识别 ID 不投递、合法帧原样投递 |
| `can.node_state_transitions` | ONLINE/TIMEOUT/**FAULT 粘滞**、显式解除、未登记节点 |
| `can.snapshot_semantics` | 启动失败如实回报、节点登记、幂等 start/stop |
| `can.submit_and_gating` | 相位切换状态契约、未运行时提交被拒、发送线程在发帧 |
| `can.send_failure_safety_path` | 零力矩失败 → 事件 + 安全上报 + 计数 |
| `can.transport_error_mapping` | errno 分类（**BUS_OFF 不来自 errno**）、未打开时收发失败、双 close 幂等 |
| `can.config_validation` | 严格校验、六节点完整性、失败不部分提交 |

### 11.2 在 ARM 目标上执行

```bash
cmake -S . -B cmake-build-ubuntu-arm -DMARSHLAND_BUILD_TESTS=ON
cmake --build cmake-build-ubuntu-arm --target marshland_atom04_tests
# 有 qemu 时：
qemu-aarch64 -L /usr/aarch64-linux-gnu \
  cmake-build-ubuntu-arm/marshland_atom04_tests can.submit_and_gating
# 或把该产物拷贝到目标 Ubuntu ARM 主机直接运行
```

### 11.3 已知限制

1. **超时边界无法再被确定性验证**：`poll(now_us)` 已转为模块内部接口，
   外部无法驱动时钟。`can.node_state_transitions` 改为观察真实时间下的
   ONLINE→TIMEOUT→ONLINE 迁移，**不再校验 199.999 ms / 恰好 200 ms 的边界**。
2. **撤权后可能多出一帧**：见 §7.1，属异步发送的固有延迟，非缺陷。
3. 未在真实 CAN 总线上验证 `CAN_ERR_BUSOFF` 的实际到达行为（无硬件）。
4. TSan 在 WSL 下无法运行（`unexpected memory mapping`），线程安全仅由
   ASan/UBSan 与互斥锁走查背书。

---

## 12. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-12 | 首次编制。登记公共契约、对象图、注册点、配置键、线程模型、槽位表、安全边界、待接入点、`can0` 偏差。 |
| 1.1 | 2026-10-12 | 代码评审修复第一轮：控制帧改为"锁存 + 周期发送"；取消 `sendMotorCurrents()`。 |
| 1.2 | 2026-10-12 | 评审修复第二轮（10 项）：新增 `TorqueOutputMode` 门控；`flushLatchedFrames` 诚实回传；接收侧 DLC 校验；FAULT 粘滞；快照返回实际复制数；强制六节点完整性；BUS_OFF 改由 `CAN_ERR_BUSOFF` 判定；CMake 补公共头链接；测试迁至仓库 `Tests/`。 |
| 2.0 | 2026-10-16 | 第三轮：**公开面收敛至 6 方法 + 3 端口 + 6 常量 + 8 类型 + 1 函数 + 1 枚举**；`CanCore` 公开方法仅 `start/stop/submit/snapshot/markFault/markOnline`；`poll` 转私有；新增提交纪元保证相位一致性；删除死代码 `ICanNodeStatusReader`、`ProtocolDispatcher`；测试断言改为只验证可确定语义。**重写本文档**，明确证据边界（ARM 未执行）。 |
