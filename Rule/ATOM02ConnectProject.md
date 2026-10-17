# ATOM-02 接入约定

日期：2026-10-17。以 `Design/Atomic/ATOM-02_State_Arbitration.md` 为准。
本次 ATOM-02 实现已获得用户明确职责豁免；不改变其他模块的职责禁令。

## 公开面

- `Atoms/Public/StateMachineTypes.hpp`：`marshland::state::ControlSource` 及 toString。
- `Atoms/Public/IStateMachine.hpp`：`control_source()`、`updateControlSource()`，
  以及全局访问器。旧 SystemInputs、系统状态、急停、授权和自动阶段接口均删除。
- `Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp`：装配层构造
  `StateMachine(const sbus::SbusData&, const web::WebData&)`。

## 共享数据

由 Entry 创建 `marshland::sbus::SbusData` 和 `marshland::web::WebData`。
生产者和状态机必须共享同一对象，不能各自创建副本。对象生命周期必须覆盖全部
读写线程。读取 `online` 及 `scaled_channels[4]` 时持有该对象的 mutex。

SBUS 在线时按 CH5 负/零/正选择 AUTO/WEB/SBUS，直接返回；SBUS 离线才读取
Web，Web 在线时负值选 AUTO、非负值选 WEB；双离线选 AUTO。Web 无 SBUS 档。
CH7 不参与，在线 SBUS 选 WEB 时不检查 Web 是否在线，不用离线输入的旧通道值。

## 主线程与关闭

1. 创建输入对象和 StateMachine；在线程启动前注册全局访问器。
2. 在线程创建前阻塞 SIGINT/SIGTERM，子线程继承屏蔽状态。
3. 主线程每 20 ms 调用 updateControlSource，使用单调时钟和限时信号等待。
4. 收到退出信号时在主线程调用 RuntimeCoordinator::shutdown，停止并 join
   已启动线程，反序清理，然后清空全局访问器。

不创建状态机线程，不由 SBUS/HTTP 写入方调用更新，不保留旧 API 兼容空实现，
不将已删除的急停、授权或自动流程逻辑搬入 Entry。

## 当前接入边界

Entry 的 SBUS/HTTP 等模块仍含原有 TODO；本次只接入选择器数据引用和主循环。
HttpServer 只更新 WebData.online，不上报 SystemInputs；SSE 不再返回不存在的
system_state/estop 字段。HTTP 现有通道写入白名单未开放 CH5；本次未扩大 ATOM-16
功能范围。具体测试、ARM 增量验证和根 CMake 阻断见 Tests/README.md。
