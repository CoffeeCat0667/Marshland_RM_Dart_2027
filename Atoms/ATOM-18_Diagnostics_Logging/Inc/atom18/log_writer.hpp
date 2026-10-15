// ============================================================================
// log_writer.hpp — 异步日志写线程（模块内部）
//
// 行为权威：Design/Config.md §16.1、用户决策基线（PLAN §0）：
//   * 普通队列有界（kQueueCapacity），满时丢弃最旧并累计 dropped_count，
//     写入方（含控制线程）永不阻塞；
//   * ERROR 与安全事件走独立保底通道，受理后立即 flush；
//   * 普通日志最多缓冲 1 秒（单调时钟计时）；
//   * 每次写入后检查轮转阈值（单文件上限）；轮转后按 max_files 与
//     max_total_size_bytes 清理最旧文件；
//   * stdout_echo = CRITICAL_ONLY 时同步输出 WARN 及以上等级。
//
// 文件命名：marshland-<UTC 启动时刻毫秒>-<序号>.jsonl，序号进程内单调。
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "IMonotonicClock.hpp"
#include "atom18/log_config.hpp"
#include "atom18/stored_event.hpp"

namespace marshland::atom18 {

class LogWriter final {
public:
    static constexpr std::size_t kQueueCapacity = 4096;
    static constexpr std::size_t kSafetyQueueCapacity = 128;
    static constexpr std::int64_t kFlushIntervalUs = 1000ll * 1000;  // 1 s

    explicit LogWriter(const marshland::app::IMonotonicClock& clock) noexcept;
    ~LogWriter();

    LogWriter(const LogWriter&) = delete;
    LogWriter& operator=(const LogWriter&) = delete;

    // 启动写线程并打开首个日志文件；失败返回 false（可稍后重试）
    [[nodiscard]] bool Start(const LogConfig& config) noexcept;

    // 冲刷并停止；析构自动调用
    void Stop() noexcept;

    // 入队（不阻塞）。safety=true 走保底通道
    void Enqueue(const StoredEvent& event, bool safety) noexcept;

    // 热加载时替换配置（写线程每轮迭代在锁内拷贝配置副本）
    void UpdateConfig(const LogConfig& config) noexcept;

    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint64_t dropped_count() const noexcept
    {
        return dropped_count_.load();
    }
    [[nodiscard]] std::uint64_t written_count() const noexcept
    {
        return written_count_.load();
    }
    [[nodiscard]] std::uint64_t rotation_count() const noexcept
    {
        return rotation_count_.load();
    }

private:
    void ThreadMain() noexcept;
    bool WriteLine(const LogConfig& cfg, const StoredEvent& event) noexcept;
    void FlushFile() noexcept;
    bool OpenNewFile(const LogConfig& cfg) noexcept;
    void RotateIfNeeded(const LogConfig& cfg) noexcept;
    void CleanupOldFiles(const LogConfig& cfg) noexcept;
    void EchoStdout(const LogConfig& cfg, const StoredEvent& event,
                    const std::string& line) noexcept;

    const marshland::app::IMonotonicClock& clock_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<StoredEvent> queue_;
    std::deque<StoredEvent> safety_queue_;
    bool stop_requested_{false};
    LogConfig config_{};                 // 由 mutex_ 保护；写线程每轮拷贝副本

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dropped_count_{0};
    std::atomic<std::uint64_t> written_count_{0};
    std::atomic<std::uint64_t> rotation_count_{0};

    // 以下仅写线程访问
    std::ofstream file_;
    std::string current_path_{};
    std::uint64_t current_file_size_{0};
    std::uint64_t file_seq_{0};
    std::int64_t last_flush_us_{0};
};

} // namespace marshland::atom18
