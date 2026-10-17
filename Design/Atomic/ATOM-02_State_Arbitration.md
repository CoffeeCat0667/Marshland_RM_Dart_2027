# ATOM-02：控制源选择器（保留 StateMachine 类名）

## 元信息

| 项目 | 内容 |
|---|---|
| Atom ID | ATOM-02 |
| 程序代号 | Marshland |
| 状态 | 启用 |
| 修订日期 | 2026-10-17 |
| 授权 | 用户明确豁免本次 ATOM-02 修改的职责禁令，并授权同步相关设计 |
| 关联契约 | `Atoms/Public/Sbus.hpp`、`Web.hpp`、`IStateMachine.hpp`、`StateMachineTypes.hpp` |

## 单一职责

`marshland::state::StateMachine` 仅保存和发布当前控制源。由装配层传入同一份
`marshland::sbus::SbusData` 和 `marshland::web::WebData` 引用，状态机在
`updateControlSource()` 中主动读取它们，不接收或维护 `SystemInputs`。

本修订取代此前本模块的系统状态、自动阶段、发次、急停、发射授权和意图仲裁设计。
删除 `updateInputs/getInputs`、状态迁移、自动启动/停止/推进、急停确认/解除、
`canFire`、相关计数与日志。不添加兼容空实现，也不把删除的逻辑转交其他模块。
旧设计中的相关行为不能被宣称为本模块仍提供的保障。

## 数据读取和 CH5

- `SbusData::online` 和 `SbusData::scaled_channels[4]` 是 SBUS 输入。
- `WebData::online` 和 `WebData::scaled_channels[4]` 是 Web 输入。
- 下标 4 是 CH5；CH7 不参与控制源选择。
- 每个输入对象的在线字段和 CH5 必须在持有其 `mutex` 时一并读取。
- 公共数组保存 `(raw - center) * x`，不是原始 SBUS 值；x 为正数。

| 输入 | 原始拨杆位置 | 公共 CH5 数值 | 控制源 |
|---|---|---|---|
| SBUS | 364 | 负值 | `AUTO` |
| SBUS | 1024 | 0 | `WEB` |
| SBUS | 1684 | 正值 | `SBUS` |
| Web | AUTO | 负值 | `AUTO` |
| Web | WEB | 0 | `WEB` |

Web 没有 SBUS 档；Web 正值也按 WEB 处理，不能据此分配 SBUS 控制源。
按中心化值的符号直接解释档位，不新增阈值、去抖或连续帧确认。

## 每次更新的顺序

1. 锁定 SBUS 数据。若在线，按 SBUS CH5 更新并立即返回；不读取 Web。
2. SBUS 离线时释放其锁，再锁定 Web 数据。若在线，按 Web CH5 更新。
3. 两者都离线时设置 `AUTO`，不使用离线对象保留的通道值。

SBUS 在线时 CH5 选择 WEB，即使 Web 离线也保持 WEB；这里选择的是拨杆指定的
控制源，不额外判断该控制源的可用性。断连后下一次更新重新选择，重连后按当前
CH5 重新选择。控制源变化不执行硬件动作，也不启动、停止或恢复自动流程。

## 调用与生命周期

- 构造时默认控制源为 `AUTO`，构造函数不主动读取输入。
- 公开接口仅保留 `control_source()` 和 `updateControlSource()`。
- `Entry.cpp` 主线程每 20 ms 调用一次更新，使用单调时钟控制周期。
- 不创建状态机线程、定时器或回调；其他线程只查询控制源。
- 发布和查询控制源使用原子变量；不同时持有两个输入对象的锁。
- 全局访问器在模块线程启动前注册，在全部线程退出后清空；输入对象和状态机
  生命周期覆盖所有使用它们的线程。
- SIGINT/SIGTERM 在启动线程前阻塞，由主线程限时等待并处理；退出时主线程
  调用既有 RuntimeCoordinator::shutdown()，停止并 join 已启动线程，再反序清理。

## 验收

- 覆盖 SBUS CH5 三档、Web AUTO/WEB、双离线默认 AUTO。
- SBUS 在线时忽略 Web 在线状态及 CH5；SBUS 离线后使用 Web CH5。
- 重连重新应用 CH5，CH7 不影响结果，输入字段不被状态机修改。
- 更新只在显式调用时发生；并发查询和生产者写入遵守公共数据锁契约。
- 主线程能响应 SIGINT/SIGTERM，退出前等待已启动线程结束。
- 只用 WSL ARM 交叉编译或目标 ARM 主机增量编译，不执行全量重编译。
