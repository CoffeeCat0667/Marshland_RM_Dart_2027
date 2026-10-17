// ============================================================================
// log.cpp — ATOM-18 日志系统实现（Atoms/Public/Log.hpp 唯二接口）
//
// 设计见本目录 PLAN.md：
//   §1 日志文件写入磁盘设计：JSON Lines、单文件轮转、按文件数/总量清理最旧；
//   §2 日志打印到终端设计：按 Init 阈值过滤、行式文本、立即 flush。
//
// 唯一对外接口：marshland::log::Init / marshland::log::Log（见 Public/Log.hpp）。
// 本文件内的所有符号都处于匿名命名空间，不构成任何对外接口。
// ============================================================================
#include "Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace marshland::log {

namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// 编译期常量：对外接口不接收配置，因此原设计"从 Config 读取"的项在此固定
// ---------------------------------------------------------------------------
constexpr const char* kOutputDirectory = "./Logs/";               // 相对程序工作目录
constexpr std::int64_t kMaxFileSizeBytes = 10ll * 1024 * 1024;    // 单文件 10 MiB
constexpr std::int64_t kMaxTotalSizeBytes = 100ll * 1024 * 1024;  // 目录总量 100 MiB
constexpr std::int64_t kMaxFiles = 10;                            // 最多保留文件数

const char* ToString(Level level) noexcept
{
    switch (level) {
    case Level::TRACE: return "TRACE";
    case Level::DEBUG: return "DEBUG";
    case Level::INFO:  return "INFO";
    case Level::WARN:  return "WARN";
    case Level::ERROR: return "ERROR";
    }
    return "UNKNOWN";
}

std::int64_t NowUtcMs() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "YYYY-MM-DDTHH:MM:SS.mmmZ"（UTC，定长 24 字符）
std::string FormatUtcIso8601Ms(std::int64_t utc_ms)
{
    const std::time_t secs = static_cast<std::time_t>(utc_ms / 1000);
    std::tm tm_utc{};
    ::gmtime_r(&secs, &tm_utc);

    const auto clamp = [](int value, int low, int high) {
        return value < low ? low : (value > high ? high : value);
    };
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  clamp(tm_utc.tm_year + 1900, 0, 9999),
                  clamp(tm_utc.tm_mon + 1, 1, 12),
                  clamp(tm_utc.tm_mday, 1, 31),
                  clamp(tm_utc.tm_hour, 0, 23),
                  clamp(tm_utc.tm_min, 0, 59),
                  clamp(tm_utc.tm_sec, 0, 60),
                  clamp(static_cast<int>(utc_ms % 1000), 0, 999));
    return buffer;
}

// JSON 字符串转义（仅文件写入用；终端打印为原样文本）
void AppendJsonEscaped(std::string& out, std::string_view text)
{
    for (const char ch : text) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char esc[8];
                std::snprintf(esc, sizeof(esc), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += esc;
            } else {
                out += ch;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 全局状态：唯二接口共享，全部由 g_mutex 保护
// ---------------------------------------------------------------------------
std::mutex g_mutex;
Level g_terminal_level{Level::INFO};  // Init 未调用时的终端阈值
std::ofstream g_file;
std::string g_current_path;
std::uint64_t g_current_size{0};
std::uint64_t g_file_seq{0};

// 打开新的日志文件；返回是否成功
bool OpenNewFileLocked() noexcept
{
    if (g_file.is_open()) {
        g_file.flush();
        g_file.close();
    }

    std::error_code ec;
    fs::create_directories(kOutputDirectory, ec);

    char name[64];
    std::snprintf(name, sizeof(name), "marshland-%lld-%llu.jsonl",
                  static_cast<long long>(NowUtcMs()),
                  static_cast<unsigned long long>(g_file_seq++));

    const fs::path path = fs::path(kOutputDirectory) / name;
    g_file.open(path, std::ios::out | std::ios::trunc);
    if (!g_file.is_open()) {
        return false;
    }
    g_current_path = path.string();
    g_current_size = 0;
    return true;
}

// 超过文件数或总量上限时删除最旧的日志文件；当前文件始终保留
void CleanupOldFilesLocked() noexcept
{
    struct Entry {
        fs::path path;
        std::uintmax_t size;
        fs::file_time_type time;
    };

    std::vector<Entry> files;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(kOutputDirectory, ec)) {
        if (ec) {
            return;
        }
        if (!item.is_regular_file()) {
            continue;
        }
        const std::string name = item.path().filename().string();
        if (name.rfind("marshland-", 0) != 0 ||
            item.path().extension() != ".jsonl") {
            continue;
        }
        files.push_back(Entry{item.path(), item.file_size(ec),
                              fs::last_write_time(item.path(), ec)});
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
    const std::size_t max_files =
        kMaxFiles > 0 ? static_cast<std::size_t>(kMaxFiles) : 0;

    std::size_t remaining = files.size();
    for (const Entry& file : files) {
        const bool over_count = max_files > 0 && remaining > max_files;
        const bool over_total = total > static_cast<std::uintmax_t>(kMaxTotalSizeBytes);
        if ((!over_count && !over_total) || remaining <= 1) {
            break;
        }
        if (file.path.string() == g_current_path) {
            continue;
        }
        const std::uintmax_t size = file.size;
        if (fs::remove(file.path, ec) && !ec) {
            total -= size;
            --remaining;
        }
        ec.clear();
    }
}

// 磁盘写入：所有等级都写；每行一条 JSON Lines 记录并立即落盘
void WriteToFileLocked(std::int64_t utc_ms, Level level, std::string_view content)
{
    if (!g_file.is_open() && !OpenNewFileLocked()) {
        return;  // 打不开文件：静默降级，不影响终端打印
    }

    std::string line;
    line.reserve(160);
    line += "{\"timestamp\":\"";
    line += FormatUtcIso8601Ms(utc_ms);
    line += "\",\"level\":\"";
    line += ToString(level);
    line += "\",\"message\":\"";
    AppendJsonEscaped(line, content);
    line += "\"}\n";

    g_file.write(line.data(), static_cast<std::streamsize>(line.size()));
    g_file.flush();  // 同步落盘：进程异常退出时已写日志不丢
    g_current_size += line.size();

    if (g_current_size >= static_cast<std::uint64_t>(kMaxFileSizeBytes)) {
        if (OpenNewFileLocked()) {
            CleanupOldFilesLocked();
        }
    }
}

// 终端打印：行式文本，内容原样输出（不做 JSON 转义）
void PrintToTerminalLocked(std::int64_t utc_ms, Level level,
                           std::string_view content) noexcept
{
    std::string line;
    line.reserve(content.size() + 40);
    line += FormatUtcIso8601Ms(utc_ms);
    line += " [";
    line += ToString(level);
    line += "] ";
    line += content;  // 内容原样 -> 终端可读
    line += '\n';

    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

} // namespace

// ---------------------------------------------------------------------------
// 唯二对外接口
// ---------------------------------------------------------------------------

void Init(Level level) noexcept
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_terminal_level = level;

    std::error_code ec;
    fs::create_directories(kOutputDirectory, ec);  // 提前暴露目录不可写问题
}

void Log(Level level, std::string_view content) noexcept
{
    const std::int64_t utc_ms = NowUtcMs();
    std::lock_guard<std::mutex> lock(g_mutex);

    WriteToFileLocked(utc_ms, level, content);  // ① 一定写磁盘（全部等级）
    if (level >= g_terminal_level) {            // ② 达阈值才打印终端
        PrintToTerminalLocked(utc_ms, level, content);
    }
}

} // namespace marshland::log
