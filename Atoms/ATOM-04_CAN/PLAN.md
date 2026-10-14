# ATOM-04_CAN 实现方案

> 状态：**已实施代码，通过静态/形式化验证、aarch64 交叉编译与 ASan/UBSan；已过两轮代码评审
> 并累计修复 17 项缺陷（§14 第一轮 8 项、§15 第二轮 10 项）。未在目标 ARM 主机上联调。**
> 更新日期：2026-10-12（第二轮评审修复 v1.2）
> 验证方式：`Atoms/ATOM-04_CAN/.verify_atom04.sh`（语法/警告门、SocketCAN 头检查、
> aarch64 目标编译、逻辑测试执行、依赖方向检查），5/5 通过。
> 接入说明：`Rule/ATOM4ConnectProject.md`。

## 1. 目标与边界

实现 ATOM-04 的 CAN/SocketCAN 传输适配模块，提供：

- SocketCAN 接口初始化与关闭；
- 单帧接收、发送及明确的成功/超时/BUS_OFF/总线错误结果；
- 原始 CAN 帧到设备协议解析入口的分派；
- CAN 节点在线状态维护与节点/总线错误事件发布；
- 按配置的周期或触发策略调度电机控制帧与状态帧。

本模块不负责机构流程判断、PID 计算或进程 `main` 入口。

## 2. 已确认的协议约束

- 电机控制帧使用 CAN ID `0x200` 与 `0x1FF` 两组帧；
- 每组帧 DLC 固定为 8；payload 为 4 个有符号 `int16` 电流值；
- 多字节整数统一使用大端序，高字节先发送；
- 零力矩安全帧为：
  - `0x200` + 8 字节全零 payload；
  - `0x1FF` + 8 字节全零 payload；
- 零力矩帧按配置的安全周期持续发送，不等待闭环确认；
- 零力矩发送失败必须报告 CAN 故障并触发全机静默策略；
- 不臆造未在配置或设备资料中定义的反馈 ID、字段布局、单位和错误映射。

## 3. 建议的目录与组件

```text
ATOM-04_CAN/
├── CMakeLists.txt
├── PLAN.md
├── Inc/
│   └── atom04_can/
│       ├── can_types.hpp          # 帧、错误、节点状态、事件等公共类型
│       ├── can_adapter.hpp        # 适配器对外接口
│       ├── socketcan_transport.hpp# SocketCAN 传输抽象
│       ├── motor_frame_codec.hpp  # 0x200/0x1FF 编解码与零力矩帧
│       └── protocol_dispatcher.hpp# 原始帧分派到设备协议解析器
└── Src/
    ├── can_adapter.cpp
    ├── socketcan_transport.cpp
    ├── motor_frame_codec.cpp
    └── protocol_dispatcher.cpp
```

如最终实现可保持为 header-only，可按规则省略 `Inc`/`Src`；当前方案优先采用 `.hpp`/`.cpp` 分层，便于隔离 Linux SocketCAN 细节和后续测试替换。

## 4. 接口设计方向

### 4.1 传输层

定义可替换的传输接口，至少覆盖：

- `open(interface_name)` / `close()`；
- `receive(timeout)`；
- `send(frame)`；
- 总线状态查询或错误映射。

Linux SocketCAN 实现负责 `AF_CAN`、`CAN_RAW`、接口索引、读写超时以及 `errno` 到模块错误的转换。发送 API 必须仅在内核确认完整写入 CAN 帧后返回成功。

### 4.2 适配器层

`CanAdapter` 负责组合传输层、协议分派器和节点状态仓储，提供：

- 初始化指定接口；
- 单帧收发；
- 接收帧分派；
- 节点在线状态更新；
- 错误事件通知；
- 电机控制/状态帧调度入口；
- 零力矩安全帧发送。

不在适配器中加入机构状态机或 PID 算法。

### 4.3 电机帧编解码

使用显式的大端序编码/解码函数，检查：

- CAN ID 是否为 `0x200` 或 `0x1FF`；
- DLC 是否为 8；
- 电流值是否在 `int16` 范围内；
- 零力矩帧 payload 是否严格为 8 个零字节。

电机顺序、反馈字段与单位通过配置/设备资料注入，不在本模块硬编码未确认的语义。

## 5. 错误与状态模型

建议使用强类型错误枚举区分：初始化失败、参数无效、发送失败、接收超时、BUS_OFF、总线错误、帧格式错误、未识别 ID 和解析失败。节点状态至少包含在线、离线、超时和故障，并通过事件回调或事件接口发布状态变化。

错误路径必须可观察，禁止把发送失败转换成成功；零力矩发送失败还要单独触发安全处理回调。

## 6. CMake 初始化策略

本目录的 `CMakeLists.txt` 只提供可被上层工程调用的模块目标，不创建可执行文件、不定义 `main`，也不覆盖上层的架构、编译标准和编译选项。

计划提供一个 `atom04_can` 静态库目标（源文件创建后加入），公开 `Inc` 头文件目录，并链接 Linux SocketCAN 所需的系统能力；若实现阶段决定完全由上层统一链接系统库，则保留模块目标的接口依赖而不重复定义编译策略。

## 7. 实现顺序

1. 建立公共类型、帧表示和错误模型；
2. 实现电机帧大端序编解码与零力矩帧构造；
3. 实现 SocketCAN 传输层及错误映射；
4. 实现协议分派、节点状态和错误事件；
5. 实现 `CanAdapter` 组合接口与调度入口；
6. 接入上层配置/设备资料允许的字段；
7. 在 ARM 目标或 aarch64 交叉环境下验证收发、超时、BUS_OFF、总线错误及零力矩失败处理。

## 8. 当前信息缺口

后续编码前需要从允许读取的配置/设备资料中确认：

- SocketCAN 接口配置对象的实际字段与生命周期；
- 设备协议解析器的既有接口；
- 电机反馈 ID、字段布局、单位和错误映射；
- 周期/触发调度接口及全机静默策略的调用接口；
- 上层对模块 target 名称和链接依赖的约定。

在这些接口明确前，不实现猜测性的跨模块调用。
---

## 9. 实施结果（2026-10-12）

用户已确认：

- 依据 `Config/dart_control.json` 的 `motor[].id`/`motor[].node` 与
  `Design/Config.md` §6.2 确定槽位映射，**不新增配置键**；
- **不解析反馈帧**，反馈帧原样投递给 ATOM-05；
- 安全静默端口**仅声明**，接线由装配层负责；
- 时钟为 ATOM 内部单调时钟，**严格不可注入**；
- SocketCAN 接口名使用 `"can0"`；
- 授权实编译验证。

### 9.1 本次实施文件清单

**新增（13 个文件）**

- `Atoms/Public/CanTypes.hpp`
- `Atoms/Public/CanInterfaces.hpp`
- `Atoms/ATOM-04_CAN/Inc/atom04_can/can_config.hpp`
- `Atoms/ATOM-04_CAN/Inc/atom04_can/motor_frame_codec.hpp`
- `Atoms/ATOM-04_CAN/Inc/atom04_can/socketcan_transport.hpp`
- `Atoms/ATOM-04_CAN/Inc/atom04_can/protocol_dispatcher.hpp`
- `Atoms/ATOM-04_CAN/Inc/atom04_can/can_core.hpp`
- `Atoms/ATOM-04_CAN/Inc/atom04_can/lifecycle_adapter.hpp`
- `Atoms/ATOM-04_CAN/Src/can_config.cpp`
- `Atoms/ATOM-04_CAN/Src/motor_frame_codec.cpp`
- `Atoms/ATOM-04_CAN/Src/socketcan_transport.cpp`
- `Atoms/ATOM-04_CAN/Src/protocol_dispatcher.cpp`
- `Atoms/ATOM-04_CAN/Src/can_core.cpp`
- `Atoms/ATOM-04_CAN/Src/lifecycle_adapter.cpp`
- `Atoms/ATOM-04_CAN/Tests/Atom04CanTests.cpp`
- `Atoms/ATOM-04_CAN/.verify_atom04.sh`
- `Rule/ATOM4ConnectProject.md`

**修改（2 个文件）**

- `Atoms/Public/Headers/CMakeLists.txt`（登记 `CanTypes.hpp`、`CanInterfaces.hpp`，同步允许列表注释）
- `Atoms/ATOM-04_CAN/PLAN.md`（本节）

**明确未修改**

- 上层 `NewVersion/CMakeLists.txt`、`Entry.cpp`；
- `Atoms/CMakeLists.txt`（`atom_04_can` 聚合行与 `add_subdirectory` 已就位）；
- `Atoms/ATOM-04_CAN/CMakeLists.txt`（目标名、别名、PublicHeaders 链接、`Src/*.cpp` GLOB 已就位）；
- 其余 18 个 ATOM 的私有实现；`AppCoordinator`；
- `Design/**`（只读）；`Rule/ConfigSchema.md`；`Config/dart_control.json`。

### 9.2 接口冻结结论

- 传输端口 `ICanTransport`：`open/close/receive/send/busStatus`，全部 `noexcept`，错误经 `CanIoResult` 表达；
- 反馈投递端口 `ICanFrameSink`：只投递**原始帧**，无字段语义；
- 节点状态：`UNKNOWN/ONLINE/TIMEOUT/FAULT`，`FAULT` 为终态，静默不得改写；
- 超时判定：`timing.can_comm_timeout_ms`（实际配置 200 ms）；
- 零力矩：`0x200` + `0x1FF` 各 8 字节全零，按 `timing.zero_torque_send_period_ms`（20 ms）开环连发。

### 9.3 槽位映射（编译期固化）

```text
0x200 slot0..3 -> 0x201, 0x202, 0x203, 0x204   (电机 1..4)
0x1FF slot0..1 -> 0x205, 0x206                  (电机 5 YAW、6 变力)
0x1FF slot2..3 -> 未使用，恒 0x0000
```

该映射由 `CanTypes.hpp` 的 `queryMotorSlot()` + `static_assert` 在编译期校验。

## 10. 静态与形式化验证结果

- [x] 无 `main()`、无独立可执行目标（`.verify_atom04.sh` pass 5）。
- [x] `Inc/` 内无任何 ATOM 私有头、无 `AppCoordinator.hpp`。
- [x] `g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror` 六源文件零告警。
- [x] `aarch64-linux-gnu-g++` 以相同标准与告警等级 `-O2 -c` 全部六个源文件通过。
- [x] SocketCAN 传输编译通过（`<linux/can.h>`）；公共头不泄漏该平台头。
- [x] 大端序：`1000 -> 03 E8`、`-1 -> FF FF`、`-32768 -> 80 00`。
- [x] 非法帧 ID（`0x123`、`0x201`）编码被拒且不修改输出对象。
- [x] 零力矩帧：先污染 payload 再构造，确认 8 字节全零。
- [x] 反馈帧 `0x201` 判为控制帧失败（`isValidControlFrame == false`）。
- [x] `0x201`~`0x206` 原样投递，未做任何字段解析；payload 逐字节保持不变。
- [x] 未识别 ID（`0x300`）与未登记节点（`0x202`）返回 `UNKNOWN_ID` 且不投递。
- [x] 超时边界：199.999 ms 不超时，恰好 200 ms 判 `TIMEOUT`。
- [x] `FAULT` 不被超时检查改写；反馈恢复后回 `ONLINE` 且 `frame_count` 递增。
- [x] 配置校验：`NOT_LOADED`、缺失键、取值不符、周期越界、未定义节点、重复 node、
      重复 motor id 全部如实失败。
- [x] 失败加载**不部分提交**：失败后既有 `CanRuntimeConfig` 保持不变。
- [x] `shutdown()` 幂等 `noexcept`；`stop()` 对未启动状态安全；先 `close()` 唤醒再 `join()`。
- [x] 生命周期适配器为独立翻译单元且 39 行（≤ 50），无业务逻辑。
- [x] 依赖方向为 `ATOM -> Public`。
- [ ] **未在目标 Ubuntu ARM 主机上做真实总线联调**（无硬件）；`ip link` 侧 bitrate=1000000
      与采样点/SJW 由系统配置负责，不在 SocketCAN 运行期下发。

## 11. 待接入点（不得当作已实现）

| ID | 事项 | 状态 |
|---|---|---|
| P-01 | `ICanSafetyNotifier` 到全机静默动作的实际接线（ATOM-14 归属） | 仅声明；`AtomIntegration.md` §11 第 7 条端口未落地 |
| P-02 | `ICanEventSink` 到诊断/生命周期事件流（ATOM-18）的桥接 | 仅声明；`AtomIntegration.md` §11 第 4 条 |
| P-03 | 电机反馈字段布局、单位与 C620 错误码映射 | 归 ATOM-05；ATOM-04 按裁决不实现 |
| P-04 | 电机控制帧电流值的实际提交方（ATOM-05 PID 输出） | `CanCore::latchMotorCurrents()` 已就绪（锁存后由发送线程按周期发出），等待调用方 |
| P-05 | 总线错误帧的细粒度分类（`CAN_RAW_ERR_FILTER` 未启用） | `DISCARD` 语义下只区分 `BUS_OFF`/`BUS_ERROR`，不做错误帧字段解析 |

## 12. 偏差登记

| 项 | 设计/配置 | 实现 | 理由 |
|---|---|---|---|
| 接口名 | `Design/Config.md` §6.1、`dart_control.json` 写 `"CAN0"` | `"can0"` | 用户裁决；Linux 接口名区分大小写 |
| 槽位来源 | `ConfigSchema` 定义 `can.frames[].fields[].slot` | `Design/Config.md` §6.2 固定映射常量 | `dart_control.json` 无 `can.frames[]`；用户裁决"新增声明即可"，未新增配置键 |
| `timing.can_comm_timeout_ms` | `dart_control.json` 存在该键 | 作为必需键读取 | 但 `Rule/ConfigSchema.md` §2.3 的 timing 表中尚未登记该键名，属文档登记缺口（不擅自修改 Rule 文档） |

## 13. 残余风险

1. 未在真实 CAN 总线上验证 BUS_OFF 的 `errno` 取值；映射表按 Linux 常见取值实现，
   未知 `errno` 一律归入 `BUS_ERROR` 并保留 `errno` 供诊断。
2. `0x1FF` 的 RTR/远程帧语义未在真实电调上确认；当前按数据帧处理。
3. 上层 `NewVersion/CMakeLists.txt` 引用了不存在的 `marsland_app` 目标，会阻塞整树
   `cmake` 配置；本模块已在 aarch64 下独立编译通过，但整树构建需上层修复后方可验证。

---

## 14. 代码评审与缺陷修复（2026-10-12）

评审方式：严格告警扫描（`-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast
-Wformat=2 -Wold-style-cast -Wcast-qual -Wswitch-enum -Wunused-const-variable=2`）、
ASan+UBSan 全量测试、以及针对发送/超时路径的定向代码走查。脚本：`.review_atom04.sh`。

| ID | 严重度 | 缺陷 | 影响 | 修复 |
|---|---|---|---|---|
| D1 | 低 | `kConfiguredInterfaceName` 仅声明、无任何引用（死常量） | 误导读者以为查了 `CAN0` | 删除常量，偏差改由注释与接入文档 §10 记录 |
| D2 | **高** | 节点超时检查只在 `receive() == RECEIVE_TIMEOUT` 分支执行 | **持续帧流会把 200 ms 离线判定彻底饿死**：节点静默后永不判离线 | `pollTimeouts()` 提升为每个循环周期无条件执行 |
| D3 | **严重** | `transmitLoop` 每周期无条件发送零力矩帧，控制帧周期内不发任何内容 | **每个周期用 8 字节全零覆盖调用方的控制量，电机永远无法出力**（等于零力矩常闭） | 控制帧改为"锁存 + 周期发送已锁存值"（初值全零）；新增 `latchMotorCurrents()` / `latchedCurrents()` / `forceZeroCurrents()` / `flushLatchedFrames()`；取消立即发送语义的 `sendMotorCurrents()` |
| D4 | 中 | `LinuxSocketCanTransport` 的 `unknown_id_count_` 从不自增（恒 0），却被写入 `busStatus()` | 调用方读到"未识别 ID 数为 0"的假信息 | 传输层删除该字段；由内核投递层统一维护并在 `snapshotBusStatus()` 合并 |
| D5 | 中 | 超时判定用 `last_frame_time_us == 0` 表示"从未收到" | 单调时钟起点附近的**真实帧**会被误判已超时 | `CanNodeStatus` 新增显式 `has_received`；判定改为 `has_received && 超时` |
| D6 | 低 | `int * 1000ULL` 触发 `-Wsign-conversion`；`receiveLoop` 中 `timeout` 为死变量 | 告警噪声；死代码 | 先转 `uint64_t` 再相乘；删除死变量 |
| D7 | 中 | 持续总线错误/接收失败路径每轮都发布事件且无退避 | 故障时事件风暴 + RX 线程空转占满 CPU | 总线错误按"错误码变化或出现次数为 2 的幂、上限 64"节流；失败路径加 5 ms 退避。节点状态跃变为边沿事件，**不节流**（避免丢真实跃变） |
| D8 | 低 | `can_core.hpp` 未包含 `<string>` 却声明 `std::string&`；`<vector>` 未使用 | 头文件不自足 | 补 `<string>`、删 `<vector>` |

### 14.1 修复后验证

- 严格告警：6 个源文件 **零输出**（缺陷数 0）。
- `-Wall -Wextra -Wpedantic -Werror`：6/6 通过；`aarch64-linux-gnu-g++ -O2 -c`：6/6 通过。
- ASan + UBSan 全量测试：**ALL CHECKS PASSED**，无内存/未定义行为报错。
- TSan 无法在 WSL 下运行（`unexpected memory mapping`，环境的 ASLR 限制），
  已用 ASan/UBSan 与互斥锁审查替代；**不得记为"线程检测已通过"**。
- 测试断言数：115（新增 CanCore 锁存/发送、D5 时钟起点、失败路径确定性检查）。

### 14.2 新增回归测试

- `testCoreLatchAndTransmit`：用记录型传输替身验证发送线程确实发出**非零载荷**的
  `0x200` 帧（`03 E8` = 1000）、非法 ID 不可锁存、`forceZeroCurrents()` 后回到全零、
  零力矩发送失败产生**恰好 2 条** `ZERO_TORQUE_FAILED` 事件、`stop()` 幂等。
- `testDispatchAndNodeStates` 扩展：`timestamp_us == 0` 的真实帧不得被判超时；
  从未收到反馈的节点保持 `UNKNOWN` 且不被超时轮询处理。

---

## 15. 第二轮代码评审与缺陷修复（2026-10-12）

评审来源：外部逐行复核（10 条），本次逐条复现并修复。验证：`.verify_atom04.sh`
（严格告警 + aarch64 + 9 个具名用例）与 `.sanitize_atom04.sh`（ASan/UBSan 全用例）。

### 15.1 缺陷与修复

| ID | 级别 | 缺陷 | 影响 | 修复 |
|---|---|---|---|---|
| F1 | **P0** | TX 线程每周期**无条件**发送零力矩帧 | 零力矩帧与控制帧共用同一对 CAN ID，稳态**逐帧交替**：实测 300 ms 内 15 个控制帧 + 15 个全零帧。等效每个周期给电机发一次零电流命令，**电机无法连续出力** | 新增 `TorqueOutputMode` 门控（默认 `ZERO_TORQUE` fail-safe）；每周期**只执行当前模式**，两模式互斥；切换时清零锁存控制量 |
| F2 | **P0** | `flushLatchedFrames()` 固定返回 `CanError::OK` | 违反"发送失败不得虚报成功"；ATOM-05/安全模块无法判断电流是否写入内核 | `sendLatchedFrames()` 改为返回 `CanError` 并记录首个失败；`flushLatchedFrames()` 按模式透传真实结果 |
| F3 | P1 | 0x1FF 的 slot2/slot3 未强制清零 | 可发出虚构的 0x207/0x208 数据 | `buildControlFrame` 对**无对应 CAN 节点**的槽位恒定编码 `0x0000` |
| F4 | P1 | 接收侧只校验 CAN ID，不校验 DLC | 截断/损坏帧被当作有效心跳刷新 ONLINE 并投递给 ATOM-05 | `onReceived` 前置 `dlc != 8 -> FRAME_INVALID`，不刷新状态、不投递 |
| F5 | P1 | FAULT 不是终态，后续反馈即恢复 ONLINE | 与公共契约"FAULT 不可被普通路径覆盖"矛盾；故障可被隐式清除 | FAULT 改为粘滞：仍计数并转发帧（否则 ATOM-05 看不到恢复尝试），但状态不被反馈或超时轮询改写；新增 `markOnline()` 显式解除 |
| F6 | P1 | 配置只含部分电机即可启动 | `Design/Config.md` §6.2 要求全部 6 个电机纳入零力矩覆盖；缺失电机永远收不到安全帧 | 强制**恰好 6 个节点**且构成 `0x201`~`0x206` 完整集合；`CanCore::start` 二次防御 |
| F7 | P1 | `snapshot()` 返回登记总数而非实际复制数；`output==nullptr && capacity>0` 解引用空指针 | 调用方按返回值遍历会**越界读**；空指针为 UB | 改为返回实际复制数 + 空指针/零容量安全返回 0；新增 `can.snapshot_semantics` 用例 |
| F8 | P1 | BUS_OFF 由 errno 粗略猜测，且未订阅错误帧 | `Design/Config.md` 验收条件"区分 BUS_OFF 与总线错误"实际未满足 | 订阅 `CAN_RAW_ERR_FILTER = CAN_ERR_BUSOFF`；接收侧识别 `CAN_ERR_FLAG`，`CAN_ERR_BUSOFF -> BUS_OFF`，其余错误帧按 DISCARD 丢弃；`ENETDOWN/ENODEV` 等改归 `BUS_ERROR`（接口不可达 ≠ 控制器 bus-off） |
| F9 | P1 | CMake 未链接 `Marshland::PublicHeaders` | 公共契约依赖未进入 CMake 依赖图 | 补 `if(TARGET Marshland::PublicHeaders)` 链接块（与 ATOM-03/15 同构）。注：19 个模块中仅 2 个原有此块，属模板级欠账 |
| F10 | P1 | 模块目录内存在 `main()` 测试入口 | 违反"ATOM 目录不出现 main"；`.verify` 只扫 `Src` 故漏检 | 测试迁至仓库 `Tests/Atom04CanTests.cpp`，按既有约定改为**具名用例 + Check 抛异常**；顶层 CMake 注册 `marshland_atom04_tests` 与 9 个 CTest 用例；`.verify` 增加"模块内无 main / 无 Tests 目录"检查 |

### 15.2 拒绝的一处越界要求

评审第 6 条另要求校验 `bus`/`role`/控制模式等字段与应用一致。**未采纳**：
`role` 是 ATOM-05 的业务语义，`bus` 是接口绑定，由 CAN 传输层校验会让模块越权解释
业务字段（`Rule/AtomIntegration.md` §2.1 禁止内核依赖业务语义）。仅采纳其
"六节点完整性"部分——那有 `Design/Config.md` §6.2 的直接依据。

### 15.3 修复后验证

| 项 | 结果 |
|---|---|
| 基础告警门（`-Wall -Wextra -Wpedantic -Werror`） | 6/6 通过 |
| 严格告警（+ `-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast -Wformat=2 -Wnull-dereference -Wold-style-cast -Wcast-qual -Wswitch-enum`） | 6/6 **零输出** |
| aarch64 交叉编译（`aarch64-linux-gnu-g++ -O2 -c`） | 6/6 通过 |
| 具名用例 | 9/9 通过 |
| ASan + UBSan（9 个用例全部） | 9/9 通过，0 失败 |
| 结构边界（无 main / 无模块内 Tests / 无横向依赖 / CMake 已链接公共头） | 4/4 通过 |

### 15.4 F1 修复的前后实测对比

同一探针（20 ms 周期、锁存电机1=1000 mA、稳态采样 300 ms）：

```text
修复前（无条件发零力矩）: 0x200 全零帧=15  非零帧=15   <- 逐帧交替，电机无法连续出力
修复后 默认 ZERO_TORQUE  : 0x200 全零帧=15  非零帧=0    <- fail-safe，只发安全帧
修复后 授权 CONTROL      : 0x200 全零帧=0   非零帧=15   <- 授权后只发控制帧
```

### 15.5 新增/变更的公共契约

- `CanTypes.hpp`：`CanNodeStatusStore::snapshot()` 语义更正（返回实际复制数）；
  `CanNodeState` 明确 FAULT 为粘滞终态。
- `CanInterfaces.hpp`：新增 `TorqueOutputMode`（`ZERO_TORQUE` / `CONTROL`）。
- 破坏性变更：**无**（`latchMotorCurrents` 等 1.1 已引入的接口保持不变）。

### 15.6 仍未闭环

- 未在真实 CAN 总线上验证 `CAN_ERR_BUSOFF` 的实际到达行为（无硬件）。
- TSan 在 WSL 下无法运行（环境 ASLR 限制），线程安全仍只有 ASan/UBSan + 互斥锁走查背书。
- 顶层 `NewVersion/CMakeLists.txt` 引用的 `marsland_app` 目标不存在，整树 `cmake` 配置仍无法完成；
  本模块与测试目标均已通过独立 aarch64 编译验证。
