/**
 * @file    PCA9685A.cpp
 * @brief   PCA9685A 驱动实现
 */

#include "PCA9685A.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <algorithm>

// ================================================================
//  构造 / 析构
// ================================================================

PCA9685A::PCA9685A(const char* device, uint8_t address)
    : fd_(-1), addr_(address), frequency_(0.0f)
{
    fd_ = open(device, O_RDWR);
    if (fd_ < 0) {
        std::cerr << "[PCA9685A] 无法打开 " << device
                  << ": " << std::strerror(errno) << std::endl;
        return;
    }

    if (ioctl(fd_, I2C_SLAVE, addr_) < 0) {
        std::cerr << "[PCA9685A] ioctl(I2C_SLAVE, 0x" << std::hex
                  << static_cast<int>(addr_) << ") 失败: "
                  << std::strerror(errno) << std::dec << std::endl;
        close(fd_);
        fd_ = -1;
        return;
    }
}

PCA9685A::~PCA9685A()
{
    if (fd_ >= 0) {
        close(fd_);
    }
}

// ================================================================
//  I2C 底层
// ================================================================

bool PCA9685A::writeReg(uint8_t reg, uint8_t value)
{
    if (fd_ < 0) return false;
    uint8_t buf[2] = {reg, value};
    return write(fd_, buf, 2) == 2;
}

bool PCA9685A::readReg(uint8_t reg, uint8_t& value)
{
    if (fd_ < 0) return false;
    if (write(fd_, &reg, 1) != 1) return false;
    return read(fd_, &value, 1) == 1;
}

// ================================================================
//  初始化
// ================================================================

bool PCA9685A::begin()
{
    if (!isReady()) return false;

    // ---- 软件复位 ----
    reset();

    // ---- 唤醒并开启自动增量 ----
    uint8_t mode1;
    if (!readReg(MODE1, mode1)) {
        std::cerr << "[PCA9685A] begin(): 无法读取 MODE1" << std::endl;
        return false;
    }

    // 若芯片处于休眠则唤醒
    if (mode1 & SLEEP) {
        writeReg(MODE1, (mode1 & ~SLEEP) | AUTO_INC);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    } else {
        writeReg(MODE1, mode1 | AUTO_INC);
    }

    // ---- 配置输出模式为推挽 ----
    writeReg(MODE2, TOTEM_POLE);

    // 关闭全部通道
    setAllPWM(0, 0);

    return true;
}

// ================================================================
//  频率设置
// ================================================================

void PCA9685A::setPWMFrequency(float freqHz)
{
    if (!isReady()) return;

    // 钳位
    freqHz = std::clamp(freqHz, MIN_FREQ_HZ, MAX_FREQ_HZ);
    frequency_ = freqHz;

    // prescale = round(OSC / (4096 * freq)) - 1
    float raw = std::round(OSC_HZ / (PWM_STEPS * freqHz)) - 1.0f;
    uint8_t prescale = static_cast<uint8_t>(std::clamp(raw, static_cast<float>(PRESCALE_MIN),
                                                       static_cast<float>(PRESCALE_MAX)));

    float actual = OSC_HZ / (PWM_STEPS * (prescale + 1.0f));

    std::cout << "[PCA9685A] 目标频率=" << freqHz << " Hz"
              << " → prescale=" << static_cast<int>(prescale)
              << " → 实际=" << actual << " Hz" << std::endl;

    // ---- 标准写 PRE_SCALE 时序 (datasheet §7.3.5) ----
    uint8_t oldMode;
    readReg(MODE1, oldMode);

    // Step 1: 进入休眠
    writeReg(MODE1, (oldMode & ~RESTART) | SLEEP);
    // Step 2: 写预分频
    writeReg(PRE_SCALE, prescale);
    // Step 3: 唤醒 (关键: 必须清除 SLEEP)
    writeReg(MODE1, oldMode & ~SLEEP);
    // Step 4: 等待振荡器起振
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    // Step 5: 重启 PWM 引擎
    writeReg(MODE1, (oldMode & ~SLEEP) | RESTART);
}

// ================================================================
//  PWM 控制 (原始 12-bit 接口)
// ================================================================

void PCA9685A::setPWM(uint8_t channel, uint16_t on, uint16_t off)
{
    if (!isReady() || channel > 15) return;

    uint8_t base = LED0_ON_L + (channel * 4);
    writeReg(base,     on  & 0xFF);
    writeReg(base + 1, (on  >> 8) & 0x0F);
    writeReg(base + 2, off & 0xFF);
    writeReg(base + 3, (off >> 8) & 0x0F);
}

void PCA9685A::setAllPWM(uint16_t on, uint16_t off)
{
    if (!isReady()) return;

    writeReg(ALL_LED_ON_L,      on  & 0xFF);
    writeReg(ALL_LED_ON_L  + 1, (on  >> 8) & 0x0F);
    writeReg(ALL_LED_OFF_L,     off & 0xFF);
    writeReg(ALL_LED_OFF_L + 1, (off >> 8) & 0x0F);
}

// ================================================================
//  PWM 控制 (占空比百分比接口)
// ================================================================

void PCA9685A::setDutyCycle(uint8_t channel, float percent)
{
    percent = std::clamp(percent, 0.0f, 100.0f);
    uint16_t off = static_cast<uint16_t>(std::round(4095.0f * percent / 100.0f));
    setPWM(channel, 0, off);
}

void PCA9685A::setAllDutyCycle(float percent)
{
    percent = std::clamp(percent, 0.0f, 100.0f);
    uint16_t off = static_cast<uint16_t>(std::round(4095.0f * percent / 100.0f));
    setAllPWM(0, off);
}

// ================================================================
//  舵机控制
// ================================================================

void PCA9685A::setServoAngle(uint8_t channel, float angleDeg)
{
    if (!isReady() || channel > 15) return;

    // 钳位角度
    angleDeg = std::clamp(angleDeg, 0.0f, 180.0f);

    // 计算周期 (ms)
    // period_ms = 1000 / frequency
    float periodMs = 1000.0f / frequency_;

    // 计算脉宽: 0.5ms + (angle/180) * 2.0ms
    // 0°=0.5ms, 180°=2.5ms, 线性插值
    float pulseMs = 0.5f + (angleDeg / 180.0f) * 2.0f;

    // 转换为 off_tick: round(pulse_ms / period_ms * 4096)
    uint16_t off = static_cast<uint16_t>(std::round(pulseMs / periodMs * 4096.0f));

    setPWM(channel, 0, off);
}

void PCA9685A::setServoPulse(uint8_t channel, float pulseWidthUs)
{
    if (!isReady() || channel > 15) return;

    // 钳位脉宽: 不能超过周期
    float periodUs = 1000000.0f / frequency_;
    pulseWidthUs = std::clamp(pulseWidthUs, 0.0f, periodUs);

    // 转换为 off_tick: round(pulse_us / period_us * 4096)
    uint16_t off = static_cast<uint16_t>(std::round(pulseWidthUs / periodUs * 4096.0f));

    setPWM(channel, 0, off);
}

// ================================================================
//  电源管理
// ================================================================

void PCA9685A::sleep()
{
    uint8_t mode;
    if (readReg(MODE1, mode)) {
        writeReg(MODE1, mode | SLEEP);
    }
}

void PCA9685A::wakeup()
{
    uint8_t mode;
    if (readReg(MODE1, mode)) {
        writeReg(MODE1, (mode & ~SLEEP) | RESTART);
    }
}

// ================================================================
//  工具 / 调试
// ================================================================

void PCA9685A::reset()
{
    if (!isReady()) return;

    // PCA9685A 软件复位: 向通用呼叫地址 0x00 写入 0x06
    // 注意: 会复位同一条 I2C 总线上所有 PCA9685 芯片
    if (ioctl(fd_, I2C_SLAVE, 0x00) < 0) {
        std::cerr << "[PCA9685A] SWRST 失败" << std::endl;
        return;
    }
    uint8_t swrst = 0x06;
    if (write(fd_, &swrst, 1) != 1) {
        std::cerr << "[PCA9685A] SWRST 写入失败" << std::endl;
    }

    // 切回原地址
    ioctl(fd_, I2C_SLAVE, addr_);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void PCA9685A::dumpRegisters()
{
    if (!isReady()) return;

    std::cout << "\n============ PCA9685A Registers ============" << std::endl;

    auto print = [](const char* name, uint8_t addr, uint8_t val, const char* extra = "") {
        std::cout << "  " << name << "  (0x" << std::hex << std::setw(2)
                  << std::setfill('0') << static_cast<int>(addr)
                  << "): 0x" << std::setw(2) << static_cast<int>(val)
                  << std::dec << std::setfill(' ') << extra << std::endl;
    };

    uint8_t v;
    readReg(MODE1, v);
    print("MODE1    ", MODE1, v,
          (std::string(" (SLEEP=") + ((v >> 4) & 1 ? "1" : "0") +
           " AI="  + ((v >> 5) & 1 ? "1" : "0") +
           " RESTART=" + ((v >> 7) & 1 ? "1" : "0") + ")").c_str());

    readReg(MODE2, v);
    print("MODE2    ", MODE2, v,
          (std::string(" (OUTDRV=") + ((v >> 2) & 1 ? "1" : "0") + ")").c_str());

    readReg(PRE_SCALE, v);
    float f = OSC_HZ / (PWM_STEPS * (v + 1.0f));
    std::cout << "  PRE_SCALE (0xFE): 0x" << std::hex << std::setw(2)
              << std::setfill('0') << static_cast<int>(v) << std::dec
              << std::setfill(' ') << " (" << static_cast<int>(v)
              << " → " << f << " Hz)" << std::endl;

    for (int ch = 0; ch < 4; ++ch) {
        uint8_t base = LED0_ON_L + ch * 4;
        uint8_t on_l, on_h, off_l, off_h;
        readReg(base,     on_l);
        readReg(base + 1, on_h);
        readReg(base + 2, off_l);
        readReg(base + 3, off_h);
        uint16_t on  = ((on_h  & 0x0F) << 8) | on_l;
        uint16_t off = ((off_h & 0x0F) << 8) | off_l;
        float duty = (off * 100.0f) / 4095.0f;
        std::cout << "  LED" << ch << "     (0x" << std::hex << std::setw(2)
                  << std::setfill('0') << static_cast<int>(base) << std::dec
                  << std::setfill(' ') << "): ON=" << std::setw(4) << on
                  << " OFF=" << std::setw(4) << off
                  << " → " << std::fixed << std::setprecision(1) << duty << "%" << std::endl;
    }
    std::cout << "=============================================" << std::endl;
}
