# ATOM-07 舵机初始化失败诊断报告

> 对象：`Atoms/ATOM-07_Servo_Driver`
> 环境：lubancat（RK3588），Ubuntu ARM，静态链接二进制，PCA9685A @ `/dev/i2c-7`（`rk3x-i2c`）
> 结论：**ATOM-07 的软件复位（SWRST）多发了一个寄存器字节，导致初始化在第一步中止**。Linux/配置/上层装配均无问题。

---

## 1. 现象

```
[Runtime] Initializing ATOM-07 Servo...
[ERROR] ATOM-07 servo open failed: /dev/i2c-7 addr 64: IIC register write failed (errno=6: No such device or address)
[Runtime] Rollback: cleaning up initialized modules...
[Main] ❌ Failed to start runtime:
  - ATOM-07 Servo: init returned false
```

每次启动必现。`errno=6` 是 `ENXIO`，在 I2C 下表示**从机没有应答（NACK）**。

## 2. 现场事实（已实测）

| 检查 | 结果 | 含义 |
|---|---|---|
| `ls /dev/i2c-7` | 存在 | 总线节点正常 |
| `cat /sys/bus/i2c/devices/i2c-7/name` | `rk3x-i2c` | 控制器正常 |
| `i2cdetect -y -r 7` | `40` 有应答 | **器件在线**，地址就是配置里的 `0x40` |
| `i2cget -y 7 0x40 0x00` | `0x06` | 读 `MODE1` 得到 0x06 —— 正是软复位命令字，异常 |
| `i2cset -y 7 0x40 0x00 0x06` | 成功（无输出） | 对 `0x40` 的普通写**完全正常** |

即：总线和器件都正常，只有"广播地址写"这一个操作失败。

## 3. 根因

`Src/Servo.cpp` 的初始化第一步：

```cpp
// Servo.cpp:172-177
selectAddress(kGeneralCallAddress);      // 切到 0x00（通用呼叫/广播）
const std::uint8_t reset_command = kSoftwareResetCommand;   // 0x06
if (!writeRegs(0x00, &reset_command, 1U)) {                 // ← 问题在这里
    return false;
}
```

`writeRegs(reg, data, size)` 的帧构造固定是「寄存器 + 数据」：

```cpp
// Servo.cpp:122-138
frame[0] = reg;                       // 0x00
frame[1 + i] = data[i];               // 0x06
::write(fd, frame, 1 + size);         // 共 2 字节
```

于是总线上实际发出的字节序列是：

```
ATOM-07 实际发送 :  [ 0x00 地址 ] [ 0x00 寄存器 ] [ 0x06 数据 ]     ← 3 字节
PCA9685A datasheet SWRST :  [ 0x00 地址 ] [ 0x06 命令字 ]           ← 2 字节
```

PCA9685A 的软件复位定义是「通用呼叫地址 0x00 之后**只跟一个命令字节 0x06**」，没有寄存器地址字节。ATOM-07 发出的这条帧被设备解释为「把寄存器 0x00（MODE1）写成 0x06」，这正好解释了为什么 `MODE1` 读回是 `0x06`。该畸形帧在设备侧产生 NACK，内核对该次 `write()` 返回 `-1 / ENXIO`，`writeRegs` 返回 false，`initialize()` 在第一步就 `return false`。

**后果**：`MODE1=SLEEP|AI`、`MODE2`、`PRE_SCALE`（50 Hz）、全通道清零这四步**从未执行**，舵机一次也不会动。

### 3.1 对照：工程内已验证可用的实现

`Lib/PCA9685A/PCA9685A.cpp:249-267`（`reset()`）是正确写法：

```cpp
ioctl(fd_, I2C_SLAVE, 0x00);
uint8_t swrst = 0x06;
write(fd_, &swrst, 1);        // 只写 1 字节，无寄存器字节
ioctl(fd_, I2C_SLAVE, addr_); // 切回器件地址
```

两点差异，均在本次故障路径上：
1. 写 **1 字节**而不是 `[寄存器][数据]` 2 字节；
2. 写失败**只记录、不中止**初始化。

## 4. 最小修复（只改 `Src/Servo.cpp`）

### 必修：SWRST 改为单字节写

在 `Impl` 内新增一个不拼寄存器字节的裸写函数（与 `writeRegs` 的循环 / errno 处理保持一致）：

```cpp
bool writeBytes(const std::uint8_t* data, std::size_t size) noexcept
{
    if (fd < 0) { setError(error, "IIC device is not open"); return false; }
    std::size_t written = 0U;
    while (written < size) {
        const ssize_t result = ::write(fd, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            const int saved = errno;
            setErrnoError(error, "IIC write failed", saved);
            return false;
        }
        if (result == 0) { setError(error, "IIC write returned 0 bytes"); return false; }
        written += static_cast<std::size_t>(result);
    }
    return true;
}
```

`initialize()` 第一步改为：

```cpp
selectAddress(kGeneralCallAddress);
const std::uint8_t reset_command = kSoftwareResetCommand;
if (!writeBytes(&reset_command, 1U)) {
    // 与已验证的 Lib 实现一致：SWRST 只用于清除上一次运行残留，不是必需步骤。
    // 建议：记录诊断后继续，不中止初始化。
    setError(error, "SWRST failed");
}
std::this_thread::sleep_for(std::chrono::milliseconds(1));
```

（是否把 SWRST 失败当致命错误由 ATOM-07 自行决定；建议容错，理由是它不影响后续 MODE1/PRE_SCALE 配置的正确性。）

### 建议（不是本次故障的直接原因，1 行说明）

`selectAddress()` 失败时只写 `error` 就返回，调用方无法感知，随后 `writeRegs` 会用**旧地址**继续写总线——现在这个旧地址就是广播地址 0x00，会把寄存器值写到总线上所有器件。建议让它返回 `bool`，`writeRegs` 前检查。此项与本次故障无直接关系，可按需处理。

## 5. 验收

修复前，先用 `i2c-tools` 验证诊断（`-a` 才允许访问 0x00 地址）：

```sh
i2cget -y 7 0x40 0x00            # 当前：0x06
i2cset -y -f -a 7 0x00 0x06      # 单字节 SWRST（Lib 的写法）
i2cget -y 7 0x40 0x00            # 期望：0x11  → 证明广播可用、器件为真 PCA9685A
i2cset -y -f -a 7 0x00 0x00 0x06 # ATOM-07 当前的两字节版，对比应报 NACK
```

修复后程序侧：

1. 启动日志出现 `ATOM-07 servo ready: /dev/i2c-7`，不再 rollback；
2. `i2cget -y 7 0x40 0xfe` 读到 `0x79`（50 Hz 对应的 PRE_SCALE）；
3. 用示波器确认通道 4 / 5 输出 50 Hz、500–2500 µs 脉宽。

## 6. 边界（本次不做的）

- **不把 `Lib/PCA9685A` 整份搬进 ATOM-07**。Lib 是自带 API 的驱动类（内部钳位角度、失败返回 `void`），与 `Atoms/Public/Servo.hpp` 的契约冲突（越界必须拒绝且不写总线、失败经 `lastError()` 上报）。只借用其 SWRST 时序。
- 不改 `Atoms/Public/Servo.hpp` 契约、不改工厂函数、不改上层装配（`Entry.cpp` 已正确调用 `open()`）。
- 不改 `MODE1 / MODE2 / PRE_SCALE / 全通道清零` 时序：与 Lib 语义等价且已符合契约。
- 不改配置读取（`servo.pca9685.i2c_device` / `address` 由 ATOM-15 读出，当前值 `0x40` 与实际器件一致）。
- `i2cdetect` 显示 `0x70` 上还有第二个器件（疑似 I2C 多路复用器），与本故障无关，本次不处理。
