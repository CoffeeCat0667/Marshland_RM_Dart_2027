#include "atom18/log_writer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

#include "atom18/json_lines.hpp"

namespace marshland::atom18 {

namespace fs = std::filesystem;

LogWriter::LogWriter(const marshland::app::IMonotonicClock& clock) noexcept
    : clock_(clock)
{
}

LogWriter::~LogWriter()
{
    Stop();
}

bool LogWriter::Start(const LogConfig& config) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_.load()) {
            return true;
        }
        config_ = config;
        if (!OpenNewFile(config_)) {
            return false;
        }
        stop_requested_ = false;
        thread_ = std::thread([this] { ThreadMain(); });
        running_.store(true);
        return true;
    } catch (...) {
        return false;
    }
}

void LogWriter::UpdateConfig(const LogConfig& config) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
    } catch (...) {
    }
}

void LogWriter::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) {
            return;
        }
        stop_requested_ = true;
    }
    cv_.notify_all();
    try {
        thread_.join();
    } catch (...) {
    }
    running_.store(false);
}

void LogWriter::Enqueue(const StoredEvent& event, bool safety) noexcept
{
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_ || !running_.load()) {
                dropped_count_.fetch_add(1);
                return;
            }
            std::deque<StoredEvent>& queue = safety ? safety_queue_ : queue_;
            const std::size_t capacity =
                safety ? kSafetyQueueCapacity : kQueueCapacity;
            while (queue.size() >= capacity) {
                queue.pop_front();  // 队列满丢弃最旧（用户决策基线 #5）
                dropped_count_.fetch_add(1);
            }
            queue.push_back(event);
        }
        cv_.notify_one();
    } catch (...) {
        dropped_count_.fetch_add(1);
    }
}

void LogWriter::ThreadMain() noexcept
{
    try {
        for (;;) {
            std::deque<StoredEvent> pending;
            std::deque<StoredEvent> safety;
            LogConfig cfg;
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                    return stop_requested_ || !queue_.empty() ||
                           !safety_queue_.empty();
                });
                safety.swap(safety_queue_);
                pending.swap(queue_);
                cfg = config_;
                stop = stop_requested_;
            }

            // 保底通道优先，逐条立即刷新（Config §16.1）
            for (const StoredEvent& event : safety) {
                if (WriteLine(cfg, event)) {
                    FlushFile();
                }
            }
            for (const StoredEvent& event : pending) {
                WriteLine(cfg, event);
            }

            const std::int64_t now = clock_.nowMicros();
            if (stop || now - last_flush_us_ >= kFlushIntervalUs) {
                FlushFile();
            }

            if (stop) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty() && safety_queue_.empty()) {
                    break;
                }
            }
        }
        FlushFile();
        if (file_.is_open()) {
            file_.close();
        }
    } catch (...) {
        // 写线程不得抛出；异常时尽力收尾
        try {
            if (file_.is_open()) {
                file_.flush();
                file_.close();
            }
        } catch (...) {
        }
    }
    running_.store(false);
}

bool LogWriter::WriteLine(const LogConfig& cfg, const StoredEvent& event) noexcept
{
    try {
        if (!file_.is_open()) {
            return false;
        }
        const std::string line = SerializeEventJsonLine(event, cfg.fields);
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.put('\n');
        current_file_size_ += line.size() + 1;
        written_count_.fetch_add(1);
        EchoStdout(cfg, event, line);
        RotateIfNeeded(cfg);  // 每次写入时检查轮转（用户决策基线 #9）
        return true;
    } catch (...) {
        return false;
    }
}

void LogWriter::FlushFile() noexcept
{
    try {
        if (file_.is_open()) {
            file_.flush();
        }
    } catch (...) {
    }
    last_flush_us_ = clock_.nowMicros();
}

bool LogWriter::OpenNewFile(const LogConfig& cfg) noexcept
{
    try {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        fs::create_directories(cfg.output_directory);
        const std::int64_t utc_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        char name[96];
        std::snprintf(name, sizeof(name), "marshland-%lld-%llu.jsonl",
                      static_cast<long long>(utc_ms),
                      static_cast<unsigned long long>(file_seq_++));
        const fs::path path = fs::path(cfg.output_directory) / name;
        file_.open(path, std::ios::out | std::ios::trunc);
        if (!file_.is_open()) {
            return false;
        }
        current_path_ = path.string();
        current_file_size_ = 0;
        last_flush_us_ = clock_.nowMicros();
        return true;
    } catch (...) {
        return false;
    }
}

void LogWriter::RotateIfNeeded(const LogConfig& cfg) noexcept
{
    if (!cfg.rotation_enabled || cfg.max_file_size_bytes <= 0) {
        return;
    }
    if (current_file_size_ < static_cast<std::uint64_t>(cfg.max_file_size_bytes)) {
        return;
    }
    if (!OpenNewFile(cfg)) {
        return;
    }
    rotation_count_.fetch_add(1);
    CleanupOldFiles(cfg);
}

void LogWriter::CleanupOldFiles(const LogConfig& cfg) noexcept
{
    try {
        struct Entry {
            fs::path path;
            std::uintmax_t size;
            fs::file_time_type time;
        };
        std::vector<Entry> files;
        const fs::path dir(cfg.output_directory);
        std::error_code ec;
        if (!fs::exists(dir, ec)) {
            return;
        }
        for (const auto& item : fs::directory_iterator(dir, ec)) {
            if (ec) {
                return;
            }
            if (!item.is_regular_file()) {
                continue;
            }
            const fs::path& path = item.path();
            const std::string filename = path.filename().string();
            if (filename.rfind("marshland-", 0) != 0 ||
                path.extension() != ".jsonl") {
                continue;
            }
            files.push_back(Entry{path, item.file_size(ec),
                                  fs::last_write_time(path, ec)});
            if (ec) {
                files.pop_back();
                ec.clear();
            }
        }
        std::sort(files.begin(), files.end(),
                  [](const Entry& a, const Entry& b) { return a.time < b.time; });

        std::uintmax_t total = 0;
        for (const Entry& file : files) {
            total += file.size;
        }
        const std::uintmax_t max_total =
            cfg.max_total_size_bytes > 0
                ? static_cast<std::uintmax_t>(cfg.max_total_size_bytes)
                : 0;
        const std::size_t max_files =
            cfg.max_files > 0 ? static_cast<std::size_t>(cfg.max_files) : 0;

        // 超限删除最旧的单个日志文件（原子操作 14）；当前文件除外
        std::size_t remaining = files.size();
        for (const Entry& file : files) {
            const bool over_count = max_files > 0 && remaining > max_files;
            const bool over_total = max_total > 0 && total > max_total;
            if ((!over_count && !over_total) || remaining <= 1) {
                break;
            }
            if (file.path.string() == current_path_) {
                continue;
            }
            const std::uintmax_t size = file.size;
            if (fs::remove(file.path, ec) && !ec) {
                total -= size;
                --remaining;
            }
            ec.clear();
        }
    } catch (...) {
    }
}

void LogWriter::EchoStdout(const LogConfig& cfg, const StoredEvent& event,
                           const std::string& line) noexcept
{
    bool echo = false;
    switch (cfg.stdout_echo) {
    case StdoutEcho::NONE:
        break;
    case StdoutEcho::ALL:
        echo = true;
        break;
    case StdoutEcho::CRITICAL_ONLY:
        echo = event.level == diagnostics::LogLevel::WARN ||
               event.level == diagnostics::LogLevel::ERROR;
        break;
    }
    if (!echo) {
        return;
    }
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

} // namespace marshland::atom18
