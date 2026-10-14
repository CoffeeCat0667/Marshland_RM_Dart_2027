/**
 * @file    PCA9685A.hpp
 * @brief   PCA9685A 16通道 12位 PWM 驱动 (I2C)
 *
 * @details 通过 Linux I2C 子系统驱动 PCA9685A 芯片。
 *          支持:
 *          - 频率 24Hz ~ 1526Hz
 *          - 16 通道独立 PWM / 统一 PWM
 *          - 占空比百分比接口
 *          - 休眠/唤醒
 *          - 软件复位
 *          - 寄存器转储调试
 *
 * @note    OE 引脚必须接 GND 才能输出 PWM。若 OE 悬空/拉高则无输出。
 *
 * 硬件连接示例:
 * @code
 *   PCA9685A        主控 (如树莓派/ARM板)
 *   ┌──────────┐    ┌──────────────┐
 *   │ SDA ─────┼────┤ I2Cx_SDA     │
 *   │ SCL ─────┼────┤ I2Cx_SCL     │
 *   │ VCC ─────┼────┤ 3.3V / 5V    │
 *   │ GND ─────┼────┤ GND          │
 *   │ OE  ─────┼────┤ GND (重要!)  │
 *   │ A0~A5 ───┼────┤ GND (地址0x40)│
 *   └──────────┘    └──────────────┘
 * @endcode
 */

#ifndef PCA9685A_HPP
#define PCA9685A_HPP

#include <cstdint>

class PCA9685A {
public:
    // ================================================================
    //  构造 / 析构
    // ================================================================

    /**
     * @brief 打开 I2C 设备并绑定从机地址
     * @param device   I2C 总线设备节点, 如 "/dev/i2c-7"
     * @param address  7位 I2C 地址 (不含读写位), 默认 0x40
     */
    explicit PCA9685A(const char* device, uint8_t address = 0x40);

    /** @brief 关闭 I2C 设备 */
    ~PCA9685A();

    // 禁止拷贝 (I2C fd 独占)
    PCA9685A(const PCA9685A&)            = delete;
    PCA9685A& operator=(const PCA9685A&) = delete;

    // ================================================================
    //  初始化
    // ================================================================

    /**
     * @brief   软件复位 + 初始配置
     * @details 执行 SWRST, 唤醒芯片, 开启自动增量, 配置推挽输出。
     * @return  true 成功, false 失败
     */
    bool begin();

    // ================================================================
    //  频率设置
    // ================================================================

    /**
     * @brief   设置所有通道的 PWM 输出频率
     * @param   freqHz 目标频率 (Hz), 范围 24 ~ 1526 (会自动钳位)
     * @details 内部根据 25MHz 振荡器计算预分频值:
     *          prescale = round(25e6 / (4096 * freqHz)) - 1
     *
     *          **注意**: 调用此函数会短暂停止 PWM 输出 (SLEEP 过程),
     *          应在初始化阶段调用, 不要在运行中频繁切换。
     */
    void setPWMFrequency(float freqHz);

    /**
     * @brief 返回当前设定频率的计算值 (Hz)
     */
    float frequency() const { return frequency_; }

    // ================================================================
    //  PWM 控制 (原始 12-bit 接口)
    // ================================================================

    /**
     * @brief   设置单个通道的 PWM 开关时刻
     * @param   channel 通道索引 [0, 15]
     * @param   on      点亮时刻 [0, 4095], 通常设为 0 (周期起点即亮)
     * @param   off     熄灭时刻 [0, 4095], off > on 时产生有效脉冲
     *
     * @note    特殊值:
     *          - (0, 0)             → 始终关闭
     *          - (4096, 0)          → 始终开启
     *          - (0, 4095)          → 全周期
     *          - (0, duty_ticks)    → 常用: 从周期起点亮到 duty_ticks 处灭
     */
    void setPWM(uint8_t channel, uint16_t on, uint16_t off);

    /**
     * @brief   同时设置全部 16 通道的 PWM
     * @param   on  统一点亮时刻
     * @param   off 统一熄灭时刻
     * @details 写入 ALL_LED 寄存器, 比逐通道设置效率更高
     */
    void setAllPWM(uint16_t on, uint16_t off);

    // ================================================================
    //  舵机控制
    // ================================================================

    /**
     * @brief   设置舵机角度 (适用于 MG996R / SG90 等 180° 舵机)
     * @param   channel  通道索引 [0, 15]
     * @param   angleDeg 目标角度 [0.0, 180.0], 自动钳位
     *
     * @details 脉冲宽度与角度的对应关系:
     *          - 0°   → 0.5ms 高电平
     *          - 90°  → 1.5ms 高电平
     *          - 180° → 2.5ms 高电平
     *
     *          换算公式 (基于当前频率):
     *            off_tick = round((0.5ms + angle/180 * 2.0ms) / period_ms * 4096)
     *
     *          **前提**: 须先调用 setPWMFrequency(50) 设定为 50Hz。
     *          频率改变后脉冲刻度会随之变化, 函数自动适配。
     */
    void setServoAngle(uint8_t channel, float angleDeg);

    /**
     * @brief   以微秒为单位设置舵机脉宽 (更精细的控制)
     * @param   channel      通道索引 [0, 15]
     * @param   pulseWidthUs 高电平脉宽 (微秒), e.g. 1500 = 1.5ms
     *
     * @details 通用接口, 不限于 180° 舵机。
     *          MG996R 典型范围: 500us ~ 2500us
     *          - 500us  → 0°
     *          - 1500us → 90°
     *          - 2500us → 180°
     */
    void setServoPulse(uint8_t channel, float pulseWidthUs);

    // ================================================================
    //  PWM 控制 (占空比百分比接口) — 通用
    // ================================================================

    /**
     * @brief   以百分比设置单个通道占空比
     * @param   channel 通道索引 [0, 15]
     * @param   percent 占空比 [0.0, 100.0], 自动钳位
     */
    void setDutyCycle(uint8_t channel, float percent);

    /**
     * @brief   以百分比设置全部通道占空比
     * @param   percent 占空比 [0.0, 100.0]
     */
    void setAllDutyCycle(float percent);

    // ================================================================
    //  电源管理
    // ================================================================

    /** @brief 进入休眠模式 (振荡器停止, PWM 无输出, 省电) */
    void sleep();

    /** @brief 从休眠模式唤醒并恢复 PWM */
    void wakeup();

    // ================================================================
    //  工具 / 调试
    // ================================================================

    /** @brief 软件复位 (SWRST), 恢复寄存器默认值 */
    void reset();

    /** @brief 转储关键寄存器到 stdout (调试用) */
    void dumpRegisters();

    /** @brief 设备是否就绪 (I2C 打开且地址绑定成功) */
    bool isReady() const { return fd_ >= 0; }

private:
    // ---- I2C 底层 ----
    bool writeReg(uint8_t reg, uint8_t value);
    bool readReg(uint8_t reg, uint8_t& value);

    int     fd_;        // I2C 文件描述符
    uint8_t addr_;      // 7-bit I2C 地址
    float   frequency_; // 当前设定频率 (Hz)

    // ---- PCA9685A 寄存器地址 ----
    static constexpr uint8_t MODE1          = 0x00;
    static constexpr uint8_t MODE2          = 0x01;
    static constexpr uint8_t PRE_SCALE      = 0xFE;
    static constexpr uint8_t LED0_ON_L      = 0x06;
    static constexpr uint8_t ALL_LED_ON_L   = 0xFA;
    static constexpr uint8_t ALL_LED_OFF_L  = 0xFC;

    // ---- 常量 ----
    static constexpr float   OSC_HZ        = 25.0e6f;  // 内部振荡器
    static constexpr float   PWM_STEPS     = 4096.0f;  // 12-bit 分辨率
    static constexpr float   MIN_FREQ_HZ   = 1.0f;
    static constexpr float   MAX_FREQ_HZ   = 3500.0f;
    static constexpr uint8_t PRESCALE_MIN  = 3;
    static constexpr uint8_t PRESCALE_MAX  = 255;

    // ---- MODE1 位 ----
    static constexpr uint8_t SLEEP         = 0x10;
    static constexpr uint8_t AUTO_INC      = 0x20;
    static constexpr uint8_t RESTART       = 0x80;

    // ---- MODE2 位 ----
    static constexpr uint8_t TOTEM_POLE    = 0x04;
};

#endif // PCA9685A_HPP
