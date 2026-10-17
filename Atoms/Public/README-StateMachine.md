# StateMachine 控制源接口

2026-10-17 修订。StateMachine 只选择控制源，不维护系统状态、急停、发次、自动
阶段或发射授权；旧 `updateInputs/getInputs`、状态迁移和流程接口已经删除。

## 输入与调用

```cpp
#include "Atoms/ATOM-02_State_Arbitration/Inc/StateArbitration.hpp"

marshland::sbus::SbusData sbus_data;
marshland::web::WebData web_data;
marshland::state::StateMachine machine{sbus_data, web_data};

// Register before module threads start; both producers use these same objects.
marshland::state::setGlobalStateMachine(&machine);
machine.updateControlSource(); // Entry main thread: every 20 ms.
auto source = machine.control_source();
// Stop and join all readers before clearing the accessor or destroying inputs.
marshland::state::setGlobalStateMachine(nullptr);
```

普通模块只需包含 `IStateMachine.hpp`，经 `getGlobalStateMachine()` 查询
`control_source()`；只有装配层需要包含 ATOM-02 实现头并注入数据引用。

| 条件 | 选择 |
|---|---|
| SBUS 在线，`scaled_channels[4] < 0` | AUTO |
| SBUS 在线，`scaled_channels[4] == 0` | WEB |
| SBUS 在线，`scaled_channels[4] > 0` | SBUS |
| SBUS 离线，Web 在线且 CH5 < 0 | AUTO |
| SBUS 离线，Web 在线且 CH5 >= 0 | WEB |
| 两者离线 | AUTO |

CH5 的下标是 4，不是 CH7 的下标 6。364/1024/1684 是原始值，推荐标定及正 x
缩放后对应负/零/正；Web 无 SBUS 档。在线 SBUS 的选择不依赖 Web 是否在线。

更新时锁住所读取输入的 `mutex`，取得一致的在线标志和 CH5；不同时持有两个
输入锁，不改写输入，控制源以原子变量发布。构造和查询不主动更新；没有内部线程、
去抖、超时监测或硬件操作，控制源选择不等于安全授权。

## 验证

WSL ARM 增量构建与运行方式见 `Tests/README.md` 的“ATOM-02 当前接口验证”。
当前根目录 Entry 的 SBUS/HTTP 生产者仍为原有 TODO 适配层；共享数据已经注入，
但本次不把这些占位模块宣称为真实设备接入。HTTP 现有通道路由也尚不接受 CH5。
