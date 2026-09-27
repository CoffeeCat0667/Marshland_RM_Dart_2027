# PCA9685A 驱动库使用指南

## 1. 概述

`PCA9685A.hpp` / `PCA9685A.cpp` 是一个面向 Linux I2C 子系统的 **PCA9685A 16通道 12位 PWM 驱动库**。API 设计简洁，适合直接集成到项目中。

## 2. 硬件连接

```
  PCA9685A          主控 (ARM / 树莓派)
 ┌──────────┐       ┌─────────────────┐
 │ SDA  ────┼───────┤ I2Cx_SDA        │
 │ SCL  ────┼───────┤ I2Cx_SCL        │
 │ VCC  ────┼───────┤ 3.3V / 5V       │
 │ GND  ────┼───────┤ GND             │
 │ OE   ────┼───────┤ GND ← 必须拉低!  │
 │ A0~A5 ───┼───────┤ GND (地址=0x40)  │
 └──────────┘       └─────────────────┘
```

> **⚠️ OE 引脚必须接 GND**，否则所有通道不会输出 PWM 波形。

## 3. 快速开始

### 3.1 编译

```bash
cd Release
make          # 编译
make run      # 编译 + 运行
```

### 3.2 最简示例

```cpp
#include "PCA9685A.hpp"

int main() {
    PCA9685A pca("/dev/i2c-7", 0x40);   // 1. 打开设备
    pca.begin();                         // 2. 复位 + 初始化
    pca.setPWMFrequency(50.0f);          // 3. 设频率 50Hz

    pca.setDutyCycle(0, 25.0f);          // 4. 通道0 → 25% 占空比
    // ... 程序运行 ...

    pca.setAllPWM(0, 0);                 // 5. 退出前关闭
}
```

## 4. API 参考

### 4.1 构造

```cpp
PCA9685A pca("/dev/i2c-7");             // 使用默认地址 0x40
PCA9685A pca("/dev/i2c-7", 0x41);       // 指定地址 0x41
```

### 4.2 初始化

```cpp
bool ok = pca.begin();          // 软件复位 + 唤醒 + 配置推挽输出
bool ready = pca.isReady();     // 检查 I2C 是否正常打开
```

### 4.3 频率设置

```cpp
pca.setPWMFrequency(50.0f);     // 50Hz  (舵机常用)
pca.setPWMFrequency(200.0f);    // 200Hz (LED 调光)
pca.setPWMFrequency(1000.0f);   // 1kHz  (电机)
// 范围: 1Hz ~ 3500Hz (典型 24~1526Hz)
```

### 4.4 占空比 (百分比接口 — 推荐)

```cpp
pca.setDutyCycle(channel, percent);     // 单通道
pca.setAllDutyCycle(percent);           // 全部通道

// channel: 0~15
// percent: 0.0 ~ 100.0

pca.setDutyCycle(0,  5.0f);    // CH1  → 5%
pca.setDutyCycle(1, 50.0f);    // CH2  → 50%
pca.setDutyCycle(2, 75.0f);    // CH3  → 75%
```

### 4.5 原始 PWM 值 (12-bit 接口)

```cpp
pca.setPWM(channel, on_tick, off_tick);  // 单通道原始值
pca.setAllPWM(on_tick, off_tick);        // 全部通道原始值

// 分辨率: 0 ~ 4095
// 常用模式:
pca.setPWM(ch, 0, 2048);        // ~50% 占空比 (从 0 亮到 2048)
pca.setPWM(ch, 0, 0);           // 始终关闭
pca.setPWM(ch, 4096, 0);        // 始终开启
```

### 4.6 电源管理

```cpp
pca.sleep();        // 休眠 (振荡器停, 零功耗输出)
pca.wakeup();       // 唤醒 + 恢复 PWM
```

### 4.7 调试

```cpp
pca.dumpRegisters();    // 打印 MODE1/2, PRE_SCALE, LED0~3 到 stdout
pca.reset();            // 软件复位 (不影响其他 I2C 上的 PCA9685)
```

### 4.8 50HZ舵机位置控制函数
```cpp
pca.setServoAngle(uint8_t channel, float angleDeg);    // [0.0,180.0]角度控制
pca.setServoPulse(uint8_t channel, float pulseWidthUs);//MG996R 典型范围: 500us ~ 2500us
```

## 5. 频率对照表

| 应用场景 | 推荐频率 | prescale | 实际频率 |
|----------|----------|----------|----------|
| 舵机控制 | 50 Hz | 121 | 50.03 Hz |
| 舵机 (高响应) | 330 Hz | 17 | 338.98 Hz |
| LED 调光 | 200 Hz | 30 | 196.85 Hz |
| LED (无频闪) | 1000 Hz | 5 | 1016.36 Hz |
| LED (极高) | 1526 Hz | 3 | 1525.88 Hz |

## 6. 占空比 → Tick 换算

```
off_tick = round(4095 × duty% / 100)

 5% →   205       25% →  1024       50% →  2048       75% →  3071
10% →   410       30% →  1229       60% →  2457       80% →  3276
15% →   614       40% →  1638       70% →  2867      100% →  4095
```

## 7. 头文件集成

将 `PCA9685A.hpp` 和 `PCA9685A.cpp` 复制到你的项目，在代码中:

```cpp
#include "PCA9685A.hpp"
```

如果你的项目使用 CMake:

```cmake
add_executable(your_app
    main.cpp
    PCA9685A.cpp
)
target_link_libraries(your_app pthread)
```

## 8. 常见问题

| 现象 | 可能原因 | 解决 |
|------|----------|------|
| 无 PWM 输出 | OE 引脚未接地 | OE → GND |
| 无 PWM 输出 | SLEEP 未清除 | 调用 `begin()` 后检查 `dumpRegisters()` 的 SLEEP=0 |
| 频率不准确 | 内部振荡器偏差 | 正常, ±3% 以内 |
| I2C 通信失败 | 地址错误 / 总线未启用 | `i2cdetect -y <bus>` 确认地址 |
| 编译报错 | 缺少 I2C 头文件 | `apt install libi2c-dev` (部分发行版) |
