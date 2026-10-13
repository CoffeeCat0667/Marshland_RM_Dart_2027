# ATOM-03_SBUS 工程接入说明

> 版本：1.2
> 日期：2026-10-11  
> 适用模块：`Atoms/ATOM-03_SBUS`

## 1. 接入目标

ATOM-03 负责配置指定串口设备上的 SBUS 接收、25 字节组帧、帧有效性判断、CH1～CH16 解码和公共通道快照维护。输入已经过硬件电平反转，软件不再做反相。

模块不直接驱动电机、舵机或其他执行器，也不在模块内部实现 `main()`。控制权最终仲裁、安全恢复和机构动作由上层装配及相应模块负责。

## 2. CMake 接入方向

```text
Marshland executable
  -> Atoms/Public/AppCoordinator（顶层装配）
  -> Marshland::atom_03_sbus
  -> Marshland::PublicHeaders
```

要求：

1. 上层工程先建立 `Atoms/Public` 的 `Marshland::PublicHeaders`。
2. 上层工程通过 `add_subdirectory(Atoms/ATOM-03_SBUS)` 加入本模块。
3. ATOM-03 提供目标 `atom_03_sbus` 和别名 `Marshland::atom_03_sbus`。
4. ATOM-03 依赖 `Marshland::PublicHeaders`、C++ 标准库和目标 Ubuntu ARM 的 POSIX/Linux 串口接口。
5. 上层工程负责决定 ARM 工具链、C++ 标准、警告选项和最终可执行文件。
6. 本模块不定义 `project()`，不创建可执行文件，不设置全局编译选项。

## 3. 公共契约

`Atoms/Public` 新增：

- `SbusTypes.hpp`
  - `SbusChannelSnapshot`：16 通道原始值、统一缩放值、CH5 控制源、CH6 急停状态、CH7 20 帧确认状态、帧有效性、在线状态、**帧序号（`frame_sequence`，每接收一个有效帧递增，供诊断与消费方判断快照新鲜度）**和最近有效帧时间；
  - `SbusChannelStore`：使用互斥锁保护完整快照替换和复制读取。
- `SbusInterfaces.hpp`
  - `IUartByteStream`：UART 参数配置、批量字节读取和 `cancel()`；
  - `ISbusSnapshotReader`：公共快照读取端口。

公共头文件不包含 `AppCoordinator.hpp`、ATOM-02 私有头或其他 ATOM 私有头。

## 4. 装配对象图

由 `Entry.cpp` 或等价顶层装配代码创建并持有以下对象，创建顺序必须保证依赖对象寿命更长：

```text
IConfigView 实现
IMonotonicClock 实现
LinuxSbusByteStream 的配置指定串口实现
SbusChannelStore
SbusCore(IUartByteStream&, IMonotonicClock&, SbusChannelStore&)
LifecycleAdapter(SbusCore&, IConfigView&)
```

本模块提供 `LinuxSbusByteStream` 作为 `IUartByteStream` 的 Linux 实现；装配层应创建该对象并将其引用传给 `SbusCore`。

`LifecycleAdapter` 只持有 `SbusCore` 和 `IConfigView` 的引用；装配层必须保证 `SbusCore`、UART、时钟、配置视图和快照存储的寿命长于适配器。

## 5. 生命周期注册

上层装配层将 `LifecycleAdapter` 注册到：

```text
RuntimeStep::InitializeSbusAndVision
```

注册时必须：

- 显式填写 `required`；
- 检查 `addModule()` 返回值；
- 失败时读取 `lastRegistrationError()`；
- 不在 ATOM-03 内自行注册；
- 不把模块初始化结果伪装为注册信息。

本阶段不规定 `required` 的最终值，由系统启动策略决定 SBUS 是否属于必需输入链路。

## 6. 配置接入

配置通过装配层注入的 `marshland::config::IConfigView` 查询，不由 ATOM-03 打开或解析 JSON。
UART 设备名由 `RuntimeContext::values` 传入；波特率、数据位、奇偶校验和停止位在 Linux UART 实现内固定为 `100000 baud, 8E2`。

RuntimeContext 参数键：

| 键 | 类型/允许值 | 用途 |
|---|---|---|
| `sbus.serial_device` | 非空字符串，例如 `/dev/ttyS10` | 实际串口设备路径 |

Linux UART 固定串口格式：

```text
100000 baud, 8 data bits, even parity, 2 stop bits (8E2)
```

本模块读取：

| 键 | 类型/范围 | 用途 |
|---|---|---|
| `sbus.channel_scale_x` | `uint8_t`，1～10 | 所有连续通道统一缩放 |
| `timing.sbus_receive_period_ms` | 正整数 | SBUS 接收周期配置 |
| `timing.sbus_loss_timeout_ms` | 正整数 | 失联阈值 |
| `sbus.frame_bytes` | 固定 25 | 帧长度 |
| `sbus.joystick_center` | 固定 1024 | 连续量中心 |
| `sbus.valid_min` | 固定 364 | 通道有效下限 |
| `sbus.valid_max` | 固定 1684 | 通道有效上限 |

缺失、类型错误或越界配置必须使生命周期初始化失败。

## 7. 数据和线程流

```text
配置指定的串口设备
  -> LinuxSbusByteStream::configure(RuntimeContext::values["sbus.serial_device"])
  -> IUartByteStream::read() 批量读取
  -> ATOM-03 有限容量环形缓冲区
  -> 持续消费缓冲区，避免 UART 数据无限积压
  -> SbusCore 独立读取线程
  -> 25 字节组帧
  -> SbusDecoder
  -> 局部完整快照
  -> SbusChannelStore 互斥替换
  -> 其他模块复制读取快照
```

线程停止流程：

```text
LifecycleAdapter::shutdown()
  -> SbusCore::stop()
  -> IUartByteStream::cancel()
  -> worker.join()
```

UART 后端必须保证 `cancel()` 能打断阻塞读取。

## 8. 通道规则

- CH1：空；
- CH2：蓄力机构速度；
- CH3：换弹机构速度；
- CH4：YAW 电机速度；
- CH5：364=AUTO、1024=WEB、1684=SBUS；
- CH6：`raw - 1024 > 0` 为正常，`< 0` 为急停，0 为中点；
- CH7：原始值 364 连续 20 个有效帧确认发射请求，中途其他有效值清零计数；
- CH8：蓄力机构左电机速度；
- CH9：蓄力机构右电机速度；
- CH10：换弹机构左电机速度；
- CH11：换弹机构右电机速度；
- CH12：变力机构速度，线性映射为 `-2000～+2000 rpm`；`364=-2000 rpm`、`1024=0 rpm`、`1684=+2000 rpm`；
- CH13～CH16：保留，不映射。

连续通道使用：

```text
output = (raw - 1024) * x
```

CH12 使用独立的变力机构速度映射，不使用通用缩放变量 `x`：

```text
force_rpm = (raw - 1024) * 2000 / 660
```

帧尾不参与有效性判断；帧长度、起始字节和通道范围仍参与判断。无效帧不更新公共快照和最近有效帧时间。
异常统一通过 `online=false` 表示，不增加额外异常状态或统计接口。
**`SbusChannelSnapshot::frame_sequence` 保留**：每接收一个有效帧递增一次，用于诊断与消费方判断快照新鲜度；
它**不**构成帧序号**连续性检查**，也不会因序号跳变而使帧失效。

## 9. 控制和安全边界

ATOM-03 只在快照中保存 CH5～CH7 的结果：

- 不直接调用执行器；
- 不直接包含 `AppCoordinator.hpp`；
- 不直接调用 `StateArbitrator::UpdateInputs()` 或字段 setter；
- 后续若需上报输入或提交意图，必须通过 Public 窄端口，由装配层绑定到 `AppCoordinator`、ATOM-02 或 SafetyManager；
- 失联后普通 SBUS 控制失效并降级 WEB；
- 断连/重连不解除急停锁定；
- 重连不恢复已终止的自动发射流程。

## 10. 验证责任

本模块静态验证至少包括：

- Public 依赖方向和头文件边界；
- 25 字节组帧且不校验帧尾；
- 通道范围和位打包解码；
- 局部快照加互斥替换；
- CH5/CH6/CH7 规则；
- CH7 20 帧确认与清零；
- 失联计时和 WEB 降级；
- `cancel()`、线程 join 和幂等 shutdown；
- 无 `main()`、无独立可执行目标；
- CMake 目标和别名命名。

## 11. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.1 | 2026-10-11 | 串口设备改为由 `RuntimeContext::values["sbus.serial_device"]` 传入，`100000 8E2` 固定在 Linux UART 实现内；补充 `LinuxSbusByteStream` 装配说明。 |
| 1.2 | 2026-10-11 | 明确 `SbusChannelSnapshot::frame_sequence` **保留**（每有效帧递增，供诊断与快照新鲜度判断），并澄清它不构成帧序号连续性检查；§8 的"不增加…帧序号连续性检查或统计接口"措辞随之修正。同步 `Design/Config.md` §7.1 通道表以本文件为准（CH12 为已映射的变力通道、CH13～CH16 保留、CH2/CH3 不区分左右电机）。 |

本阶段按用户要求不执行编译。
