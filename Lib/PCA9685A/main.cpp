/**
 * @file    main.cpp
 * @brief   PCA9685A 演示程序
 *
 * 功能:
 *  - 设置 PWM 频率为 50Hz
 *  - 通道1占空比5%, 通道2占空比10%, ..., 通道16占空比80%
 *  - 循环保持, 永久运行 (Ctrl+C 安全退出)
 */

#include "PCA9685A.hpp"

#include <cmath>
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

// ---- 全局变量 (信号处理用) ----
static PCA9685A*          g_pca     = nullptr;
static std::atomic<bool>  g_running { true };

void onSignal([[maybe_unused]] int sig)
{
    std::cout << "\n[main] 收到退出信号, 安全关闭中..." << std::endl;
    g_running = false;
}

// ================================================================
int main()
{
    signal(SIGINT,  onSignal);
    signal(SIGTERM, onSignal);

    // ---- 参数 ----
    const char*       DEVICE   = "/dev/i2c-7";
    constexpr uint8_t ADDRESS  = 0x40;
    constexpr float   FREQ     = 50.0f;
    constexpr int     CHANNELS = 16;

    // ---- 初始化 ----
    PCA9685A pca(DEVICE, ADDRESS);
    g_pca = &pca;

    if (!pca.isReady() || !pca.begin()) {
        std::cerr << "[main] PCA9685A 初始化失败" << std::endl;
        return 1;
    }

    pca.setPWMFrequency(FREQ);
    pca.dumpRegisters();   // 验证寄存器写入

    // ---- 配置各通道 ----
    std::cout << "\n============ 通道配置 ============" << std::endl;
    std::cout << " 通道  |  占空比  |  OFF 值  " << std::endl;
    std::cout << "-------+----------+----------" << std::endl;

    for (int ch = 0; ch < CHANNELS; ++ch) {
        float    duty = (ch + 1) * 5.0f;                     // 5%, 10%, ..., 80%
        uint16_t off  = static_cast<uint16_t>(std::round(4095.0f * duty / 100.0f));

        pca.setPWM(ch, 0, off);

        std::cout << "  CH"  << std::setw(2) << (ch + 1)
                  << "   |  " << std::setw(4) << std::fixed << std::setprecision(1) << duty << "%"
                  << "   |  " << std::setw(4) << off << std::endl;
    }

    // ---- 永久保持 ----
    std::cout << "\n[main] 运行中, 按 Ctrl+C 退出..." << std::endl;

    int t = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "\r[main] 已运行 " << std::setw(5) << ++t << " s" << std::flush;
    }

    // ---- 安全退出 ----
    std::cout << "\n[main] 关闭全部通道..." << std::endl;
    pca.setAllPWM(0, 0);

    std::cout << "[main] 程序结束." << std::endl;
    return 0;
}
