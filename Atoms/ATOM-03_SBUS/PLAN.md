# ATOM-03_SBUS Coding Plan

> 状态：已实施代码，未执行编译；保留静态验证结果。
> 更新日期：2026-10-11
> 验证方式：静态代码检查、接口一致性检查和形式化判定；本阶段不编译。

## 1. 已冻结的功能范围

### 1.1 主要职责

ATOM-03 从已完成硬件电平反转的 `UART10` 独立线程读取 SBUS 帧，采用 25 字节组帧方式，不校验帧尾，完成有效性判断和 CH1～CH16 解码，并把结果保存到公共 SBUS 通道结构体。

公共通道结构体必须使用互斥锁保护：

- 读取线程先写入局部临时快照；
- 一帧完整且有效后，在锁内一次性替换公共快照；
- 读取方在锁内复制快照；
- 不允许读取到半帧或跨帧混合数据。

### 1.2 次要职责

后续实现以下 ATOM-03 功能，但不直接驱动电机、舵机或其他执行器：

- 连续通道数值校验与统一缩放；
- CH5 控制源选择状态保存；
- CH6 急停状态保存；
- CH7 发射请求持续计数；
- 最近有效帧时间记录；
- 200 ms SBUS 失联判定；
- 失联后 SBUS 普通控制失效并降级 WEB；
- 有效帧恢复后按 CH5 重新选择控制源；
- 不恢复已终止的自动发射流程；
- 断连或重连不解除急停锁定。

具体机构动作、控制权最终仲裁、安全恢复编排和发射执行不在本模块实现。

## 2. 已冻结的通道和数值规则

| 通道 | 含义 | 处理方式 |
|---|---|---|
| CH1 | 空 | 保留，不产生输出 |
| CH2 | 蓄力机构速度 | 连续值写入结构体 |
| CH3 | 换弹机构速度 | 连续值写入结构体 |
| CH4 | YAW 电机速度 | 连续值写入结构体 |
| CH5 | AUTO / WEB / SBUS 控制权分配 | 364=AUTO，1024=WEB，1684=SBUS |
| CH6 | 软件急停 | 中心化值 `raw - 1024 > 0` 为正常，`< 0` 为急停，0 为中点 |
| CH7 | 发射 | 原始值 364 连续保持 20 个有效帧后确认发射请求 |
| CH8 | 蓄力机构左电机速度 | 连续值写入结构体 |
| CH9 | 蓄力机构右电机速度 | 连续值写入结构体 |
| CH10 | 换弹机构左电机速度 | 连续值写入结构体 |
| CH11 | 换弹机构右电机速度 | 连续值写入结构体 |
| CH12～CH16 | 保留 | 不映射，不产生输出 |

连续通道规则：

- 原始中心值：`1024`；
- 有效范围：`364～1684`，即 `1024 ± 660`；
- 缩放公式：`output = (raw - 1024) * x`；
- `x` 的配置键固定为 `sbus.channel_scale_x`；
- `x` 类型为 `uint8_t`；
- `x` 允许范围为 `1～10`；
- 所有连续通道共用同一个 `x`；
- 输出范围为 `-660*x`～`+660*x`。

无效值处理：通道值超出有效范围的帧不得更新公共快照，不得刷新最近有效帧时间，不得形成任何状态或动作请求。

## 3. 已冻结的运行约束

- UART10 设备参数由 `RuntimeContext` 传入，不在模块内硬编码设备路径和串口参数。
- UART10 软件侧不执行电平反转。
- UART 读取使用独立线程。
- 线程启动、停止和资源释放由 UART/SBUS 后端负责；生命周期适配器只做编排。
- 本模块不负责生命周期注册；上层装配层负责创建对象、注册适配器和检查注册结果。
- 公共 SBUS 类型和通道快照放入 `Atoms/Public`。
- ATOM-03 只能依赖 `Atoms/Public` 和 C++ 标准库，不得包含 `AppCoordinator.hpp` 或其他 ATOM 私有头。
- 不在本模块定义 `main()`，不创建独立可执行文件。
- 本阶段不编译，采用静态和形式化验证。

## 4. 代码范围

### 4.1 计划新增文件

#### `Atoms/Public/`

1. `SbusTypes.hpp`
   - 16 通道原始值类型；
   - 连续值/离散值相关类型；
   - CH5 控制源原始状态；
   - CH6 急停状态；
   - CH7 发射请求计数/确认状态；
   - 帧有效性、帧状态和最近有效帧时间字段；
   - 公共通道快照结构体。

2. `SbusInterfaces.hpp`
   - UART 字节读取窄接口；
   - SBUS 快照读取接口；
   - SBUS 后端启动/停止接口；
   - 只定义抽象能力，不绑定 Linux 串口具体类。

#### `Atoms/ATOM-03_SBUS/Inc/atom03_SBUS/`

1. `sbus_config.hpp`
   - `RuntimeContext` 配置转换；
   - UART10 参数读取；
   - `sbus.channel_scale_x` 的 `uint8_t` 转换和 1～10 范围检查。

2. `sbus_decoder.hpp`
   - 25 字节组帧；
   - 不校验帧尾；
   - 帧长度、通道值范围和解码接口。

3. `sbus_channel_state.hpp`
   - 模块内部状态组合和互斥访问封装；
   - 如果公共快照已足够，此文件只保留内部计数/状态，不重复定义公共结构体。

4. `sbus_uart_reader.hpp`
   - 独立读取线程的生命周期和停止接口；
   - 依赖 `Public` UART 窄接口，不绑定具体设备实现。

5. `sbus_core.hpp`
   - 组合 UART 读取、解码、快照更新、缩放和 CH5～CH7 状态处理。

6. `lifecycle_adapter.hpp`
   - 唯一的 `ILifecycleModule` 实现声明；
   - 只保存核心对象引用，不包含业务循环。

#### `Atoms/ATOM-03_SBUS/Src/`

1. `sbus_decoder.cpp`
   - 组帧、通道位解码和有效性校验实现。

2. `sbus_uart_reader.cpp`
   - 独立线程读取循环；
   - 停止和阻塞读取取消处理。

3. `sbus_core.cpp`
   - 局部快照、互斥替换、缩放、CH5/CH6/CH7 状态逻辑和失联时间更新。

4. `lifecycle_adapter.cpp`
   - 配置读取、核心启动/停止和 `ModuleInitResult` 错误翻译；
   - 不实现机构控制和仲裁业务。

### 4.2 计划修改文件

1. `Atoms/Public/CMakeLists.txt`
   - 将新增 `SbusTypes.hpp` 和 `SbusInterfaces.hpp` 加入公共头文件源列表；
   - 不增加平台选项、编译器选项或全局标准设置。

2. `Atoms/ATOM-03_SBUS/CMakeLists.txt`
   - 将目标别名从现有的 `Marshland::ATOM_03_SBUS` 修正为规范要求的 `Marshland::atom_03_SBUS`；
   - 纳入 `Inc/` 和 `Src/` 文件；
   - 保持模块目标为 `atom_03_SBUS`；
   - 不定义 `project()`，不设置架构、编译器、C++ 标准或编译选项。

3. `Atoms/ATOM-03_SBUS/PLAN.md`
   - 随实现进度更新接口、验证结果和剩余风险；
   - 不把未验证的公共接口或构建结果写成已完成。

### 4.3 明确不修改文件

本次 Coding 计划不修改以下范围：

- 上层 `CMakeLists.txt`；
- `Entry.cpp`；
- ATOM-01、ATOM-02 及其他 ATOM 的私有实现；
- `AppCoordinator` 具体实现；
- 电机、舵机、CAN、GPIO 和 SafetyManager 的机构执行代码；
- 不删除现有功能代码，不进行无关重构。

## 5. Coding 阶段顺序

### 阶段 0：接口冻结

1. 固定 `SbusTypes.hpp` 中的公共字段和线程安全边界。
2. 固定 `SbusInterfaces.hpp` 中的 UART 和快照接口。
3. 固定 `RuntimeContext::values["sbus.channel_scale_x"]` 的解析规则。
4. 明确 `CH5/CH6/CH7` 的结构体字段，不在 ATOM-03 内执行机构动作。

### 阶段 1：公共契约

1. 新增公共 SBUS 类型和接口。
2. 修改 `Atoms/Public/CMakeLists.txt` 登记公共头文件。
3. 静态检查公共头文件不能包含 ATOM 私有头。

### 阶段 2：解析内核

1. 实现 25 字节组帧。
2. 明确不检查帧尾。
3. 校验通道范围 `364～1684`。
4. 解码 CH1～CH16。
5. 使用局部快照和互斥锁原子更新公共结构体。
6. 实现统一 `x` 缩放。

### 阶段 3：状态处理

1. 实现 CH5 三档控制源保存。
2. 实现 CH6 中心化急停保存。
3. 实现 CH7 原始值 364 的 20 个有效帧计数和确认状态。
4. 实现最后有效帧时间和 200 ms 失联状态。
5. 实现恢复后按 CH5 重新选择，不恢复已终止自动流程。

### 阶段 4：UART 线程与生命周期适配器

1. 实现 UART10 后端接口注入。
2. 实现独立读取线程。
3. 确保停止路径不会永久阻塞。
4. 实现生命周期适配器的初始化失败翻译、停止和幂等性。

### 阶段 5：静态形式化验证

- 检查所有头文件依赖方向；
- 检查没有 `main()` 和独立可执行目标；
- 检查 CH1～CH16 映射完整且 CH12～CH16 无输出；
- 检查帧尾不参与有效性判定；
- 检查无效帧不更新快照和时间；
- 检查 `output = (raw - 1024) * x` 的边界；
- 检查 CH5 三档、CH6 符号判定和 CH7 20 帧确认；
- 检查互斥锁覆盖整帧替换和快照读取；
- 检查 `shutdown()` 的幂等性和线程退出路径；
- 检查 CMake 目标和别名命名。

## 6. 验收判定表

| 场景 | 预期结果 |
|---|---|
| 完整 25 字节帧，帧尾正确 | 解码并更新完整快照 |
| 完整 25 字节帧，帧尾错误 | 不因帧尾单独拒绝；其余有效性通过则更新快照 |
| 长度不足或通道值越界 | 丢弃帧，不更新快照，不刷新有效时间 |
| 读取线程与快照读取并发 | 只能读取完整旧快照或完整新快照 |
| CH5=364 | 保存 AUTO |
| CH5=1024 | 保存 WEB |
| CH5=1684 | 保存 SBUS |
| CH6 中心化值大于 0 | 保存正常模式 |
| CH6 中心化值小于 0 | 保存急停状态 |
| CH6 中心化值等于 0 | 保存中点状态，不误触发急停 |
| CH7 原始值连续 19 个有效帧为 364 | 不确认发射 |
| CH7 原始值连续 20 个有效帧为 364 | 确认发射请求 |
| CH7 中途出现非 364 有效帧 | 按确认的计数复位规则处理；默认清零连续计数 |
| 连续通道 raw=1024 | 输出 0 |
| 连续通道 raw=364 | 输出 `-660*x` |
| 连续通道 raw=1684 | 输出 `+660*x` |
| `x=1` 或 `x=10` | 通过范围校验 |
| `x=0` 或 `x>10` | 配置校验失败 |
| 超过 200 ms 无有效帧 | SBUS 普通控制失效并降级 WEB |
| 有效帧恢复 | 按 CH5 恢复控制源，不恢复已终止自动流程 |
| 急停后断连/重连 | 急停状态不自动解除 |

## 7. 实施结果

用户已确认：

- CH7 中途出现非 364 的有效帧时清零连续计数；
- UART 后端支持 cancel()，可打断阻塞读取；
- 允许实施本计划；
- 本轮不编译，只做静态和形式化验证。

因此已按第 4 节范围实施代码和工程接入文档。
## 8. 本次实施文件清单

### 新增

- `Atoms/Public/SbusTypes.hpp`
- `Atoms/Public/SbusInterfaces.hpp`
- `Atoms/ATOM-03_SBUS/Inc/atom03_SBUS/sbus_config.hpp`
- `Atoms/ATOM-03_SBUS/Inc/atom03_SBUS/sbus_decoder.hpp`
- `Atoms/ATOM-03_SBUS/Inc/atom03_SBUS/sbus_core.hpp`
- `Atoms/ATOM-03_SBUS/Inc/atom03_SBUS/lifecycle_adapter.hpp`
- `Atoms/ATOM-03_SBUS/Src/sbus_config.cpp`
- `Atoms/ATOM-03_SBUS/Src/sbus_decoder.cpp`
- `Atoms/ATOM-03_SBUS/Src/sbus_core.cpp`
- `Atoms/ATOM-03_SBUS/Src/lifecycle_adapter.cpp`
- `Rule/ATOM3ConnectProject.md`

### 修改

- `Atoms/Public/CMakeLists.txt`
- `Atoms/ATOM-03_SBUS/CMakeLists.txt`
- `Rule/ConfigSchema.md`
- `Rule/ConfigSchema.schema.json`
- `Atoms/ATOM-03_SBUS/PLAN.md`

### 删除

- 无删除。

### 明确未修改

- 上层 `CMakeLists.txt`；
- `Entry.cpp`；
- ATOM-01、ATOM-02 及其他 ATOM 私有实现；
- `AppCoordinator` 具体实现；
- 电机、CAN、GPIO、舵机和 SafetyManager 执行代码。

## 9. 静态验证结果

- [x] 无 `main()` 和独立可执行文件。
- [x] ATOM-03 头文件位于 `Inc/atom03_SBUS/`，实现位于 `Src/`。
- [x] Public 头文件不包含其他 ATOM 私有头。
- [x] SBUS 25 字节组帧，不校验帧尾。
- [x] CH1～CH16 解码，CH12～CH16 不产生映射输出。
- [x] 局部快照通过互斥锁一次性替换，读取返回完整快照。
- [x] CH5、CH6、CH7 规则已固化。
- [x] CH7 非 364 有效帧清零连续计数。
- [x] `cancel()` 后 join 读取线程，`shutdown()` 幂等。
- [x] `sbus.channel_scale_x` 已加入配置键文档和 JSON Schema。
- [x] `git diff --check` 通过目标文件检查。
- [ ] 未执行 ARM 构建，符合本轮“不编译”要求。
