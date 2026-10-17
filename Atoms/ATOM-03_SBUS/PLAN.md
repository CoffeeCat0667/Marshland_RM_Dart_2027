# ATOM-03 三文件实现

- 日期：2026-10-16。
- 用户确定的唯一功能：串口读到 SBUS -> 解码 -> 转换到已确定范围 -> 写入公共结构体。
- 本次只改本模块、公共 Sbus.hpp 与对应设计说明；不迁移外部代码。

## 1. 最终代码文件

1. Inc/atom03_SBUS/sbus_private.hpp：唯一私有头，内部环形缓冲和解码声明。
2. Src/sbus.cpp：唯一实现，串口配置与读取、组帧、解码、映射、数据更新。
3. ../Public/Sbus.hpp：唯一公共头，SbusData 和 ReceiveSbus 声明。

CMakeLists.txt 为工程构建描述，不增加任何代码管理层。私有 Inc 不向消费者导出。

## 2. 删除范围

- 删除 sbus_core、sbus_config、linux_sbus_byte_stream、lifecycle_adapter 的全部独立头和源文件。
- 不保留 SbusCore、LinuxSbusByteStream、UART 抽象、生命周期接口、配置视图或线程管理器。
- 不保留 start/stop/cancel 对象、后台线程、控制权解释、急停解释、发射确认、授权、序号、时间戳或失联降级。
- 保留的停止标志仅用于退出阻塞接收循环，文件描述符在函数返回时关闭。

## 3. 公共调用

```cpp
bool ReceiveSbus(const char* serial_device, SbusData& data,
                 const std::atomic<bool>& stop_requested,
                 std::uint8_t channel_scale_x, std::uint16_t frame_bytes,
                 std::uint16_t channel_center, std::uint16_t channel_minimum,
                 std::uint16_t channel_maximum,
                 std::uint32_t read_timeout_ms = 20U) noexcept;
```

- 设备名和全部必要的数值参数直接传入，不读取 IConfigView 或 RuntimeContext。
- 函数阻塞运行，线程由调用方提供；调用方负责数据和停止标志的存活期。
- stop_requested=true 后在 poll 返回或当前数据批次结束后退出；最坏等待还受超时、锁等待和调度影响，不承诺立即 cancel。
- 返回 true 仅表示正常请求停止；打开失败、配置失败、参数错误和永久 I/O 错误返回 false。
- 固定 100000 baud、8E2；不进行软件反转。

## 4. 数据和映射

- 公共 SbusData 只保留 mutex、16 路 scaled_channels 和 online，不公开原始通道。
- 原始值来自 16 路 11-bit 解析，只保存在接收函数的局部数组中；帧头 0x0F，不校验帧尾，不解析 Byte 23。
- 帧长由参数决定，半帧保存在有限环形缓冲区；没有额外帧副本。
- 通用输出 `(raw-center)*x`，x 为 uint8_t、1～10。
- 推荐 364/1024/1684 输出 -660*x/0/+660*x；CH12 独立输出 -2000/0/+2000 rpm。
- CH7 发射输入位于 scaled_channels[6]，CH13 供弹舵机输入位于 scaled_channels[12]；二者独立使用通用缩放，无持续帧数确认；仅 CH14～CH16 保留。
- 不固定中点或有效范围，只检查最短载荷和运算边界，避免越界/除零。
- 有效整帧加锁一次更新全部映射通道并置 online=true；进入接收、超时、无效数据、错误和函数退出置 online=false，保留上次数组。
- 读者接收 const SbusData&，持有其 mutex 同时复制 scaled_channels 和 online，不持锁等待接收函数结束；离线时不能使用旧数组作为新输入。
- online 反映接收事件，不是独立帧龄看门狗或执行授权。ReceiveSbus 参数和返回值不变。

## 5. 验证范围

- 清点模块恰好一个私有头、一个 cpp、一个公共 Sbus.hpp。
- ARM 严格警告语法检查、Public-only 调用示例检查。
- 现有 WSL ARM 构建目录中增量构建 atom_03_sbus，不构建最终程序。
- 环形回绕、半帧、多帧、配置帧长、通道边界及 CH12 映射检查。
- git diff --check；确认旧文件和管理层依赖已经移除。
- 未进行真实串口测试，不声明外部装配兼容。

## 6. 三文件重构时的验证记录

- 精确文件检查通过：一个 47 行私有头、一个 149 行 cpp、一个 34 行公共 Sbus.hpp，共 230 行代码。
- 唯一 cpp 通过 ARM 编译器 `-fsyntax-only -Wall -Wextra -Werror` 检查。
- 只包含 Sbus.hpp 的接收/消费示例通过 ARM 语法检查；非系统头依赖只有 Sbus.hpp 本身。
- 独立算法模型验证 1024 帧、16384 个通道和 512 个回绕帧；覆盖分块读取、不同帧长、x=1/10、最短载荷访问及对称/非对称 CH12 范围。不是生产函数运行测试。
- 模块增量构建未通过：上层 Atoms/CMakeLists.txt 引用了当前不存在的 ATOM-04_CAN 和 ATOM-05_Motor_PID，CMake 配置阶段中止，尚未进入本模块编译。
- 未修改这些外部模块、未进行全量重编译，也未用其他工程掩盖上层配置失败。
- 范围限定 git diff --check 通过；真实串口、停止等待和目标机运行仍未测试。

## 7. 历史：公共数据再次收缩

- 用户已明确授权：SbusData 只保留 scaled_channels 和 mutex。
- 删除 raw_channels、online、setOffline 和所有公共状态更新；不新增状态替代它们。
- 保持一私有头、一 cpp、一公共 Sbus.hpp；串口、解码、缩放、锁保护和接收函数返回契约不变。
- 本次 ARM 增量构建通过：`[100%] Built target atom_03_sbus`，只构建模块，没有进行全量重编译或最终程序构建。
- ARM 严格语法检查通过：`-fsyntax-only -Wall -Wextra -Werror`。
- Public-only 编译期契约检查通过：结构化绑定确认恰好两个成员；raw_channels/online 不存在；const SbusData& 可以持锁复制；ReceiveSbus 参数与返回类型保持不变。
- 删除符号检查、三文件边界检查和范围限定 git diff --check 通过。
- 映射与组帧算法本轮未改动；未进行真实串口或多线程压力运行测试，也未迁移外部代码。

## 8. 按用户决定恢复 online

- 公共数据字段固定为 mutex、scaled_channels、online；raw_channels 仍只保存在接收局部数组中。
- 恢复 setOffline 内部辅助函数；有效帧的数组和 online 在同一把锁内更新，所有异常/退出只清 online、不清数组。
- 不增加文件、生命周期或统计功能，不修改接收函数签名和数值映射。
- 本次 ARM 模块增量构建通过：`[100%] Built target atom_03_sbus`，未进行全量重编译或最终程序构建。
- ARM 严格语法与 Public-only 契约检查通过：恰好 mutex/scaled_channels/online 三个成员，不公开 raw_channels，const 消费者可持锁同时复制数组和在线状态。
- ReceiveSbus 签名、三文件边界及范围限定 git diff --check 检查通过；未进行真实串口或并发压力运行测试。

## 9. CH7/CH13 统一缩放

- 用户确认废弃旧 CH7 确认规则，CH7/CH13 均直接发布 `(raw-center)*x`；不添加计数器、动作执行或新的公共字段。
- 解码循环已覆盖这两个通道且算法相同，补充源码和公共头用途说明，不增加重复分支。
- 同步 Config、Interface、ConfigSchema、机器 Schema 和 ATOM3 接入说明；CH12 专用映射保持不变，保留通道改为 CH14～CH16。
- 文档版本为 Config 1.6、Interface 1.9、ConfigSchema 1.11、ATOM3ConnectProject 1.4；保留原有其他版本记录，不回退已有修订。
- ARM 模块增量构建与严格语法检查通过；Public-only 消费示例确认公共结构仍只有 mutex/scaled_channels/online，可持锁读取 CH7/CH13。
- 机器 Schema 的 11 个正反例通过，验证 CH7/CH13 正确绑定、错绑/空值/漏填/重复错误绑定拒绝；未增加必填功能条目，原有 SWITCH/JOYSTICK 输入标签保持兼容。
- 独立算法模型验证 720 帧、2160 个 CH7/CH13/CH12 结果及 360 个回绕帧；覆盖 x=1～10、两种标定、两个帧长和 CH7/CH13 独立的低/中/高组合。此项是模型检查，不是生产函数或真实串口运行测试。
- 未增加源码文件、计数器、执行器调用或公共字段；未迁移其他模块与运行配置文件。
