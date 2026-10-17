# ATOM-04_CAN_Motor_Control 闭环控制方案

> 状态：**已实施（2026-10-16 第四轮：闭环控制版）**
> 对外两个入口（`start` / `setRpm`）+ 线程内 PID + 反馈状态结构体；20ms 驱动周期；
> **无安全网**（用户明确选择「不要安全网，调用方自己负责」）。
> 本文件取代上一版「最小 SocketCAN 传输层」方案。
> 验证状态见 §9 —— 已通过严格告警门、aarch64 交叉编译、aarch64 CMake 集成构建、
> 以及**宿主侧逻辑探针实跑**；但**未在真实 CAN 总线上运行过**。

---

## 1. 对外接口

用户要求「对外只保留两个函数」。实际暴露如下——第 1、2 项就是那两个函数，
第 3、4 项是**物理上无法省略**的辅助（理由见 §5 偏差 D-3）。

**契约声明位置**：全部对外面声明在共享公共头 **`Atoms/Public/Motor.hpp`**（不在模块
目录内），端口名为 `marshland::motor::IMotorControl`，实现在
`Atoms/ATOM-04_CAN_Motor_Control/Src/can_motor_control.cpp`。这样其它 ATOM 在目录层面
就能判定「这是公开面」，不会把它误当成 ATOM-04 的私有实现细节。

| # | 用途 | 签名 |
|---|---|---|
| 1 | 入口：以独立线程启动 CAN 接收 | `can::CanResult start(const char* interface_name, const motor::MotorPidParams (&gains)[can::kMaxMotorNodes]) noexcept` |
| 2 | 入口：设置目标转速 | `can::CanResult setRpm(std::uint32_t node_id, std::int16_t rpm) noexcept` |
| 3 | 读取反馈状态 | `can::CanResult state(std::uint32_t node_id, MotorState& out) const noexcept` |
| 4 | 生命周期 | `void stop() noexcept` ／ `bool isRunning() const noexcept` |

### 结构体

```cpp
// namespace marshland::motor，声明于 Atoms/Public/Motor.hpp
// start() 一次性传入，6 组，顺序对应节点 0x201~0x206
struct MotorPidParams { double kp, ki, kd, max, min; };

// state() 原子拷出（用户要求的"实际转速和温度"在 rpm / temperature）
struct MotorState {
    std::uint32_t node_id;        // 0x201~0x206
    bool          online;         // 是否至少收到过一帧合法反馈（不随超时清零）
    std::int16_t  rpm;            // 反馈字节 2..3（通行布局）
    std::int16_t  rotor_position; // 反馈字节 0..1（转子机械角度 0~8191）
    std::int16_t  current;        // 反馈字节 4..5
    std::uint8_t  temperature;    // 反馈字节 6（℃）
    std::uint64_t timestamp_us;
};
```

### 语义约定（均为实现决定，非用户指定）

| 项 | 约定 | 理由 |
|---|---|---|
| 目标转速范围 | `[-6800, 6800]`，越界返回 `PARAM_INVALID`（不静默钳位） | 用户给定范围；越界即调用方错误，明确拒绝优于猜测 |
| 节点范围 | 必须 `0x201~0x206`，否则 `PARAM_INVALID` | 帧 ID 与节点 ID 是两个空间（见 §5 D-5） |
| 未 `start()` 时调用 | `setRpm` 返回 `NOT_OPEN`；`state` 返回全零快照（`node_id` 仍回填） | 读数不应因生命周期阶段而丢失身份 |
| **首次 `setRpm` 之前该电机输出恒 0** | 不把「目标 0 转速」当作默认意图 | 否则上电后 PID 会为自由旋转的电机灌大电流试图把它拉住 |
| `stop()` | 清空目标与使能、复位 PID、清空反馈表；**不发任何 CAN 帧** | 用户选择无安全网；但状态卫生仍应保持（重启不沿用旧目标） |

## 2. 数据流与线程模型

```text
setRpm(节点, 目标转速) ──┐
                         ├─> [唯一的接收线程]
0x201~0x206 反馈帧 ──────┘     ├─ poll(timeout)  收帧 / 到点唤醒
                               ├─ drainFrames()  MSG_DONTWAIT 排空 + 解析 + 更新反馈表
                               └─ 每 20ms: driveOnce()
                                     ├─ 对每台电机 Pid::update(目标, 实测rpm) -> 电流
                                     ├─ 按 queryMotorSlot() 装配 0x200 / 0x1FF 槽位
                                     └─ sendFrame() 发出两组控制帧
```

- **只有一个线程**：收帧、PID、组帧、发送全在它内部，因此 PID 状态无需加锁。
- `poll` 使用「到下一个 20ms 时点还剩多久」作为超时，所以线程既能及时收帧，
  也必然每 20ms 醒一次执行 PID；`stop()` 后线程最多 20ms 退出（`join` 不会挂死）。
- 反馈帧用 `MSG_DONTWAIT` 一次排空，绝不让收帧拖住驱动周期。
- `driveOnce()` 的组帧与发送**不在互斥量内**，避免 I/O 阻塞 `state()` 的读者。
- 超期重新对齐（`next_tick = after + period`），不做追赶风暴。

### 线程安全

| 共享量 | 保护方式 | 访问者 |
|---|---|---|
| `states_[6]` 反馈状态表 | `std::mutex state_mutex_` | 接收线程写；任意线程经 `state()` 读 |
| `target_rpm_[6]` | `std::atomic<std::int16_t>`（relaxed） | 任意线程写；接收线程读 |
| `enabled_[6]` | `std::atomic<bool>`（release/acquire 配对，保证目标值可见） | 任意线程写；接收线程读 |
| `pid_[6]` | 无需锁 | 仅接收线程 |
| `socket_fd_` / `running_` | `std::atomic` | 见下 |

`start()` / `stop()` 必须串行调用；`stop()` 先 `join()` 再关闭描述符，因此不存在
「一个线程正在 `recv` 而另一个线程把 fd 关掉」的竞态。

## 3. C620 反馈帧布局（C620 通行布局）

用户最初口述为「字节 0..1 = 转速」，随后更正为下述通行布局，代码已按此实现。

| 字节 | 含义 | 类型 |
|---|---|---|
| 0..1 | 转子机械角度（0~8191） | int16 大端 |
| 2..3 | **实际转速** | int16 大端 |
| 4..5 | 实际电流 | int16 大端 |
| 6 | 温度（℃） | uint8 |
| 7 | 空 | 忽略 |

> **注意**：字节 0..1 是**机械角度**，不是转速。若错误地把角度当作实测值，PID 会形成
> 错误闭环（剧烈误动作）。该风险已随本次更正消除；偏移量集中在
> `Src/motor_frame_codec.cpp` 的 `kPositionOffset` / `kRpmOffset`，将来若需再调整只改这两行。

只有 `dlc == 8` 且 ID ∈ `0x201~0x206` 的帧被接受；其余（含错误帧、部分读）直接丢弃，
不更新状态、不参与闭环。

## 4. PID

`Inc/atom04_can/pid.hpp` + `Src/pid.cpp`，单电机位置式：

```text
输出 = kp*e + ki*∫e + kd*d(测量)/dt      e = 目标转速 − 实测转速
```

| 设计点 | 说明 |
|---|---|
| 微分作用于**测量值** | 避免 `setRpm()` 阶跃时产生微分冲击（derivative kick）——阶跃是常态 |
| 抗积分饱和 | 把积分项单独限制在 `[min, max]` 内，饱和期间不再累积 |
| 首周期微分 = 0 | 无历史测量可差分，避免启动尖峰 |
| 限幅非法（`max <= min`） | 恒输出 0；`start()` 已先拒绝这种参数，属二次防御 |
| 输出转换 | `toSlotValue()` 先按 `[min,max]` 限幅，再饱和到 int16，绝不回绕 |

PID **不是线程安全的**，必须由单一线程持有；在 ATOM-04 中唯一调用者是接收线程。

## 5. 偏差登记（必读）

### 5.1 对 `Design/Atomic/ATOM-04_CAN.md` 的越界

| ID | 偏差 | 依据与代价 |
|---|---|---|
| **D-1** | **PID 实现在 ATOM-04 内** | 设计文档单一职责明写「CANAdapter **不承担机构流程判断或 PID 计算**」；PID 属 `Design/Atomic` 的 ATOM-05 功能，根 `AGENTS.md` 要求拒绝此类实现。已向用户完整报告，用户裁决「就在这里实现」并给出全部参数来源，构成**知情覆盖** |
| **D-2** | **反馈帧字段解析在 ATOM-04 内** | 设计文档验收条件要求「不臆造 ID 或字段」，PLAN 旧版 P-03 明确「反馈字段布局/单位归 ATOM-05」。现按用户指定的 C620 布局解析 |
| **D-3** | 对外**多出** `state()`、`stop()`、`isRunning()` | 「只保留两个函数」与「暴露一个结构体」不可同时成立——没有读数入口就无法暴露结构体，没有 `stop()` 就无法终止线程。故保留两个功能入口 + 两个必要辅助 |
| **D-4** | **无安全网** | 用户明确选择「不要安全网，调用方自己负责」。`stop()` 不发零力矩帧；发送失败被静默忽略；急停/失联/堵转职责全部外移。**这是安全倒退，已明确告知** |
| **D-5** | 发送函数收「节点 ID」而非帧 ID | `0x201~0x206` 是**反馈** ID；真实发送帧只有 `0x200`/`0x1FF`，每帧 4 个 int16 电流槽位。用户表述的「传入 id: 0x201~0x206」按节点 ID 实现，内部经 `queryMotorSlot()` 映射 |
| **D-7** | 契约物理位置为 `Atoms/Public/Motor.hpp`（用户口述为 `Atoms/Public/Headers/Motor.hpp`） | `Headers/` 目录里目前**只有 `CMakeLists.txt`**，且它不在导出 include 路径上——导出的是 `Atoms/Public/` 本身（`Headers/CMakeLists.txt:30-31`），现有 15 个公共头也全部位于 `Atoms/Public/`。为与约定一致并让 `#include "Motor.hpp"` 直接可用，采用 `Atoms/Public/Motor.hpp`。若确需放在 `Headers/` 下，必须同时把该目录加进 include 路径，会与其余 15 个头不一致 |
| **D-6** | ~~反馈字节顺序与 C620 通行布局相反~~ | **已解决**：用户 2026-10-16 更正确认采用 C620 通行布局（0..1 角度 / 2..3 转速），代码与文档已同步 |

### 5.2 与 `ATOM-04_CAN.md` 原子操作/验收条件的差距

| 设计要求 | 现状 |
|---|---|
| 1 打开接口 / 2 接收一帧 | ✅ 保留（错误上报仍无通道） |
| 3 分派至设备协议解析器 | ⚠️ 部分：现在**确实解析**了 C620 反馈字段（D-2），但没有可插拔的分派器 |
| 4 发送已构造并校验的帧 | ✅ 保留（`buildControlFrame` + 内核确认完整写入） |
| 5 更新节点在线状态 | ⚠️ 仅 `online = 曾收到过合法帧`，**无超时下线判定** |
| 6 发布总线/节点错误事件 | ❌ 无。模块已不 include `CanInterfaces.hpp` |
| 7 按 Config 周期调度 | ⚠️ 有 20ms 固定周期，但**不读 Config**，参数全部由调用方传入 |
| 8 发送失败不虚报成功 | ⚠️ `sendFrame()` 如实返回，但**驱动周期内的失败被静默忽略**（无上报对象） |
| 验收：区分成功/超时/BUS_OFF/总线错误 | ❌ 不满足（错误帧直接丢弃） |
| 验收：两组控制帧 8 字节大端 | ✅ 满足 |
| 验收：零力矩帧失败触发安全处理 | ❌ 完全移除（D-4） |

### 5.3 公共契约中的死类型

模块现在只依赖 `Atoms/Public/CanTypes.hpp`，**不再 include `CanInterfaces.hpp`**。
因此以下契约在树内**已无任何 ATOM-04 消费者**（`Atoms/Public/` 本次未修改）：

`ICanFrameSink`、`ICanEventSink`、`ICanSafetyNotifier`、`CanEventKind`、
`TorqueOutputMode`、`CanNodeState`、`CanNodeStatus`、`CanCoreSnapshot`。

仍然有效并被本模块使用：`CanFrame`、`CanResult`、`CanError`、`MotorSlotValues`、
`queryMotorSlot` + 相关常量。

## 6. 文件清单

| 文件 | 说明 |
|---|---|
| `Atoms/Public/Motor.hpp` | **对外契约**：`IMotorControl` 端口 + `MotorPidParams` / `MotorState` + 转速范围与驱动周期常量（登记于 `Atoms/Public/Headers/CMakeLists.txt`） |
| `Inc/atom04_can/can_motor_control.hpp` | 实现类 `CanMotorControl`（实现上述端口）；**不再声明任何对外契约类型** |
| `Src/can_motor_control.cpp` | socket 打开/绑定、接收线程、20ms 驱动、PID 组帧发送 |
| `Inc/atom04_can/pid.hpp` + `Src/pid.cpp` | 位置式 PID（新增） |
| `Inc/atom04_can/motor_frame_codec.hpp` + `Src/motor_frame_codec.cpp` | 大端序原语、控制帧构建、**C620 反馈帧解析（新增）** |
| `CMakeLists.txt` | 目标名 `atom_04_can_motor_control`；`Src/*.cpp` 由 GLOB 自动纳入 |
| `.verify_atom04.sh` | 4 段验证（见 §9） |

**已删除**：`socketcan_transport.hpp/.cpp`（两个对外入口合并后不再需要独立传输类）、
`can_core.*`、`can_config.*`、`lifecycle_adapter.*`、`can_transport.hpp`。

**保留但当前无调用点**（沿用用户上一轮「保留 codec」的裁决，非本次新增）：
`buildZeroTorqueFrame()`、`isZeroTorquePayload()`、`isValidControlFrame()`。
在无安全网的当前形态下它们是纯工具函数，**不代表模块具备零力矩能力**。

## 7. 环境与外部阻塞（非本模块造成）

1. ~~整树 `cmake` 配置被 ATOM-08 挡住~~ —— **已解决（2026-10-16，用户授权）**。
   ATOM-08 目录已被外部重命名为 `ATOM-08_Control`（模块目标 `atom_08_control`），
   但 `Atoms/CMakeLists.txt` 的聚合链接行仍写 `Marshland::atom_08_charge`。
   已改为 `Marshland::atom_08_control`；整树 aarch64 交叉配置随即通过
   （`Configuring done` / `Generating done`，`-DMARSHLAND_BUILD_TESTS=ON`）。
   遗留项（仅注释，不影响配置）：`Atoms/ATOM-08_Control/CMakeLists.txt:2` 的文件头
   仍写 `# ATOM-08_Charge — 蓄力机构控制`，未改动（属 ATOM-08 自身文件）。
2. **`.verify_atom04.sh` 曾被外部删除**：本 Agent 只删除过两个 `socketcan_transport`
   文件（删除输出可查）。该脚本在本次会话中被发现从磁盘消失（WSL `ls -la` 确认），
   已按原内容重建。同目录的 `CMakeLists.txt`、`Atoms/CMakeLists.txt`、
   `NewVersion/CMakeLists.txt:245` 也都在本会话中被外部改动过——**该目录存在并发编辑**。
3. **ATOM-04 测试目标仍处于注销状态**：`Tests/Atom04CanTests.cpp`（1006 行）针对的是
   已删除的 `CanCore`/`CanRuntimeConfig` 公开面，且与当前 `CanMotorControl` 形态完全不
   匹配。是否重写为覆盖 `pid` / `motor_frame_codec` / 新入口的测试，待用户裁决。
4. `Design/Atomic/ATOM-04_CAN.md` 仍描述零力矩、节点状态、事件等已不存在的功能；
   本 Agent 对该文件只读，**未修改**。

## 8. 残余风险

1. ~~反馈布局可能与实际电调不符~~ —— 已按 C620 通行布局更正（§3）。仍建议首次上电时
   核对两个字段的数值是否合理（静止时转速应接近 0、角度应在 0~8191 内）。
2. **完全没有真实 CAN 验证**：收发路径、BUSOFF 到达方式、`poll` 时序、
   `::send` 在真实驱动下的行为均未在真机或 vcan 上跑过（本环境 `vcan.ko` 存在但
   创建 vcan 接口需要 root，`sudo` 需密码，见 §9）。
3. **无安全网**（D-4）：驱动周期内发送失败静默忽略；进程异常退出后电流指令留在
   电调内，直到电调自身 CAN 超时（该机制未验证）。
4. `online` 只在收到首帧时置位，**永不因超时清零**——它不是「当前在线」的可靠判据，
   使用时必须结合 `timestamp_us`。
5. PID 增益由调用方负责；模块不做任何参数合理性判断（除有限性与 `max > min`）。
6. `state()` 返回的是「最近一帧」的快照，不含丢帧统计。

## 9. 验证状态（实测记录）

执行环境：WSL（Ubuntu 24.04）+ `g++ 13.3.0` + `aarch64-linux-gnu-g++ 13.3.0`。

| 项 | 命令 / 方式 | 结果 |
|---|---|---|
| 基础告警门 | `g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -fsyntax-only` | ✅ 3/3 |
| 严格告警 | 追加 `-Wshadow -Wconversion -Wsign-conversion -Wuseless-cast -Wdouble-promotion -Wformat=2 -Wnull-dereference -Wold-style-cast -Wcast-qual -Wswitch-enum -Wunused-const-variable=2` | ✅ 3/3 **零输出** |
| aarch64 交叉编译 | `aarch64-linux-gnu-g++ -O2 -c` | ✅ 3/3 |
| 结构边界与对外面 | 无 `main` / 无横向依赖 / 目标名 / 公共头链接 / **两入口与两结构体声明在 `Atoms/Public/Motor.hpp`** / **`IMotorControl` 端口存在** / **`Motor.hpp` 已登记进 `Marshland::PublicHeaders`** / **模块头未重复声明契约类型** / **实现类确实实现该端口** / 有 atomic+mutex | ✅ 11/11，`fail=0 pending_delete=0` |
| **CMake 集成（aarch64）** | 探针工程 `add_subdirectory(ATOM-04_CAN_Motor_Control)` + 链接静态库 | ✅ 配置、构建、链接全部成功；产物 `ELF 64-bit ARM aarch64` |
| **逻辑探针实跑（宿主侧）** | 编译并运行 `probe.cpp` | ✅ `PROBE_ALL_OK`，覆盖：PID 上/下限幅、非法限幅恒 0、**反馈帧布局判定**（字节 2..3 的 `03 E8` 必须解出 rpm=1000，字节 0..1 的 `1F 40` 必须解出角度=8000 —— 偏移量一旦对调即立刻失败）、DLC≠8 拒绝且不改 out、控制帧大端与 `0x1FF` 保留槽强制清零、两个入口的前置校验 |
| 真实 CAN 收发 / 线程时序 | vcan 或真机 | ❌ **未执行**。`uid=1000`，`sudo` 需密码，`vcan` 未加载（`vcan.ko` 存在但无法 `modprobe`） |
| 整树 `cmake` 配置（aarch64，`MARSHLAND_BUILD_TESTS=ON`） | `cmake -S NewVersion -B /tmp/marshland-cfg` | ✅ `Configuring done` / `Generating done`（修正 ATOM-08 目标名后，见 §7.1） |
| 整树内构建 ATOM-04 目标 | `cmake --build /tmp/marshland-cfg --target atom_04_can_motor_control` | ✅ 3 个 TU 编译 + 静态库链接成功 |

### 关于逻辑探针

探针（`probe.cpp` + 探针 `CMakeLists.txt`）位于
`C:\Users\a2909\AppData\Local\Temp\atom04probe\`，**不属于工程交付物**。
它抓到过一个只有运行才能暴露的缺陷：`state()` 在 `start()` 之前返回的 `node_id` 为 0
而非被查询的节点号 —— 已修复（`state()` 回填 `node_id`）。
纯编译门（pass 1~3）**无法发现这类行为缺陷**，这是保留探针的价值所在。

**结论：本模块"已通过严格告警门、aarch64 交叉编译、aarch64 CMake 集成构建，并已在
宿主侧实跑逻辑探针；但从未在真实 CAN 总线上运行过"。** 在真机（或 vcan）上跑通
收发与 20ms 时序之前，不得记为"已验证"。
