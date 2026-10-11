# ATOM-03_SBUS 工程接入说明

> 版本：1.0  
> 日期：2026-10-11  
> 适用模块：`Atoms/ATOM-03_SBUS`

## 1. 接入目标

ATOM-03 负责 UART10 上的 SBUS 接收、25 字节组帧、帧有效性判断、CH1～CH16 解码和公共通道快照维护。UART10 已经过硬件电平反转，软件不再做反相。

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
4. ATOM-03 只依赖 `Marshland::PublicHeaders` 和 C++ 标准库。
5. 上层工程负责决定 ARM 工具链、C++ 标准、警告选项和最终可执行文件。
6. 本模块不定义 `project()`，不创建可执行文件，不设置全局编译选项。

## 3. 公共契约

`Atoms/Public` 新增：

- `SbusTypes.hpp`
  - `SbusChannelSnapshot`：16 通道原始值、统一缩放值、CH5 控制源、CH6 急停状态、CH7 20 帧确认状态、帧状态和在线时间；
  - `SbusChannelStore`：使用互斥锁保护完整快照替换和复制读取。
- `SbusInterfaces.hpp`
  - `IUartByteStream`：UART 字节读取和 `cancel()`；
  - `ISbusSnapshotReader`：公共快照读取端口。

公共头文件不包含 `AppCoordinator.hpp`、ATOM-02 私有头或其他 ATOM 私有头。

## 4. 装配对象图

由 `Entry.cpp` 或等价顶层装配代码创建并持有以下对象，创建顺序必须保证依赖对象寿命更长：

```text
IConfigView 实现
IMonotonicClock 实现
IUartByteStream 的 UART10 实现
SbusChannelStore
SbusCore(IUartByteStream&, IMonotonicClock&, SbusChannelStore&)
LifecycleAdapter(SbusCore&, IConfigView&)
```

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
UART10
  -> IUartByteStream::read()
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
- CH12～CH16：保留，不映射。

连续通道使用：

```text
output = (raw - 1024) * x
```

帧尾不参与有效性判断；帧长度、起始字节和通道范围仍参与判断。无效帧不更新公共快照和最近有效帧时间。

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

本阶段按用户要求不执行编译。
