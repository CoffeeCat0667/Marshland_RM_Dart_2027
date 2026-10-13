# ATOM-03_SBUS 缺陷清单

- 更新日期：2026-10-13
- 设计基线：PRD 0.10、PEF 1.5、Interface 1.6、Config 0.9
- 适用模块：`Atoms/ATOM-03_SBUS`
- 维护约定：只登记未完成事项；已修复事项从未修复列表移除，并在版本记录中保留摘要。
- 本文件只记录 ATOM-03 归属的问题。ATOM-15 配置解析/校验问题另见 `Atoms/ATOM-15_Config_Persistence/BugLists.md`。

## 1. 未修复缺陷

| 编号 | 标题 | 严重度 | 状态 |
|---|---|---|---|
| ATOM03-BUG-001 | SBUS 快照未接入公共输入、仲裁和安全端口 | P1 | 未修复 |
| ATOM03-BUG-002 | `receive_period_ms` 未参与读取调度，UART 轮询超时固定为 100 ms | P1 | 未修复 |
| ATOM03-BUG-003 | UART `read() == false` 后无退避/错误状态，设备异常时可能空转 | P1 | 未修复 |
| ATOM03-BUG-004 | `frame_bytes` 消费契约未强制为 25，配置通过后可能导致所有帧被拒绝 | P1 | 未修复 |

## 2. 缺陷详述

### ATOM03-BUG-001：SBUS 控制链未接通

**位置**：

- `Src/sbus_core.cpp`
- `Inc/atom03_SBUS/sbus_core.hpp`
- `Inc/atom03_SBUS/lifecycle_adapter.hpp`
- `Entry.cpp` 的对象装配

**现状**：

`SbusCore` 只写入 `SbusChannelStore`，生命周期适配器只注入 `SbusCore` 和 `IConfigView`。当前没有注入或调用：

- `IInputReporter` / `AppCoordinator::ReportSbus()`；
- `ReportSoftwareEstop()`；
- `IControlAuthority::SubmitIntent()`；
- ATOM-14 的安全请求端口。

因此有效 SBUS 帧虽然可以更新快照，但不会改变系统控制源、不会上报在线/失联、不会触发急停，也不会产生普通控制或发射请求。该行为不满足 `Design/Atomic/ATOM-03_SBUS.md` 的控制意图与安全入口要求。

**修复要求**：

1. 由装配层向 ATOM-03 注入已批准的窄端口，不允许包含 `AppCoordinator.hpp` 或 ATOM-02 私有头。
2. 有效帧提交 SBUS 在线状态、CH5 控制源状态和按映射生成的输入结果。
3. CH6 急停只提交安全请求，不直接修改执行器或绕过 ATOM-14。
4. CH7 确认请求提交前必须通过 ATOM-02/ATOM-14 规定的授权路径。
5. 失联时上报 `online=false`，恢复时重新上报状态，但不得恢复已终止自动流程或解除急停。
6. 端口调用与快照发布的顺序必须明确，不能让消费方读取到“新快照、旧控制状态”的不一致组合。

**验收条件**：

- 有效 SBUS 帧可以观察到 AppCoordinator/ATOM-02 的输入状态变化；
- CH6 急停请求能够到达安全管理入口；
- 失联和恢复能够更新系统输入状态；
- ATOM-03 本身不直接驱动机构、不直接写 StateArbitrator 字段；
- 所有拒绝/未授权动作不会产生执行器命令。

### ATOM03-BUG-002：`receive_period_ms` 未生效

**位置**：

- `Src/sbus_config.cpp`：读取并保存 `timing.sbus_receive_period_ms`；
- `Src/sbus_core.cpp`：读取线程没有使用 `SbusConfig::receive_period_ms`；
- `Src/linux_sbus_byte_stream.cpp`：`kReadTimeoutMs` 固定为 100 ms，并直接传给 `poll()`。

**现状**：配置值可以成功加载，但不影响 UART 读取行为。`poll()` 的超时是固定 100 ms，和配置的 SBUS 接收周期没有关系。

**修复要求**：

1. 先明确 `receive_period_ms` 的语义：建议定义为无数据时的读取等待/轮询超时，而不是伪装成固定周期任务。
2. 将该参数通过窄接口传给 UART 后端，禁止在 Linux 后端保留无配置的 100 ms 常量。
3. 若改为完全数据驱动读取，则删除该消费键并同步修改 Config/Schema/设计文档；不能保留一个表面可配置、实际无效的参数。
4. 使用单调时钟验证配置变化对行为的实际影响。

**验收条件**：

- 配置 20 ms 与其他合法值时，UART 等待行为可观察地不同；
- 没有数据时不会错误地宣称收到了帧；
- 有数据时不会被固定周期人为延迟；
- `receive_period_ms` 不会与失联超时语义混淆。

### ATOM03-BUG-003：UART 读取失败可能导致线程空转

**位置**：

- `Src/sbus_core.cpp:109-111`；
- `Src/linux_sbus_byte_stream.cpp:147-170`；
- `Public/SbusInterfaces.hpp` 的 `IUartByteStream::read()` 返回契约。

**现状**：`read()` 返回 `false` 后，SbusCore 只更新 offline 状态并立即进入下一次循环。Linux 后端在 `POLLERR`、`POLLHUP`、`POLLNVAL`、不可恢复 `read()` 错误时返回 `false`。如果 `running_` 仍为 true，线程会连续失败、连续加锁并持续占用 CPU。

**修复要求**：

1. 区分正常取消、超时、临时错误和设备错误；推荐将 `bool` 返回值改为明确的读取状态枚举。
2. 正常 `cancel()` 必须让线程退出或尽快返回，不进入错误重试循环。
3. 设备错误必须进入明确的 offline/故障状态，并采用退避、重开或等待生命周期关闭的策略。
4. 不允许在永久错误状态下无延迟 `continue`。
5. 保证 `stop()` 仍可打断读取并完成 join。

**验收条件**：

- 模拟 `POLLHUP`/`POLLERR` 时线程不会高频空转；
- `cancel()` 后线程可在规定时间内退出；
- 临时 EINTR/EAGAIN 与永久设备错误处理不同；
- 失联状态只在状态变化时更新或具备明确节流策略。

### ATOM03-BUG-004：SBUS 固定帧长度消费契约不闭合

**位置**：

- `Src/sbus_config.cpp:58-61` 接受配置中的任意整数帧长度；
- `Src/sbus_core.cpp:136` 始终以固定 25 字节调用解码器；
- `Src/sbus_decoder.cpp:8` 要求 `size == config_.frame_bytes`。

**现状**：ATOM-15 当前允许 `frame_bytes` 在 1～4096 范围内通过校验，但 SBUS 协议固定为 25 字节。只要配置不是 25，模块可以完成初始化，却会拒绝所有帧。

**修复要求**：

- ATOM-03 在配置读取时强制 `frame_bytes == 25`，并检查中心值、最小值、最大值符合协议契约；或统一由 ATOM-15 Schema 强制这些常量；
- Decoder、Core、ConfigStore 和 Schema 使用同一帧长度来源；
- 配置错误必须在生命周期初始化阶段失败，不能等运行时表现为永久离线。

## 3. 相关设计一致性

CH12 已在 `Design/Atomic/ATOM-03_SBUS.md` 修订为变力机构速度：364=-2000 rpm、1024=0 rpm、1684=+2000 rpm；CH13～CH16 保留。CH12 的专用缩放不使用通用 `channel_scale_x`。

## 4. 版本记录

| 版本 | 日期 | 说明 |
|---|---|---|
| 1.0 | 2026-10-13 | 首次登记控制链未接通、读取周期未生效、UART 失败空转和固定帧长度契约不闭合。 |
