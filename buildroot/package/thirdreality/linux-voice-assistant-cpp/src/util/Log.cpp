#include "util/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

namespace lva::log {

namespace {

std::atomic<int> g_level{static_cast<int>(Level::kInfo)};
std::mutex g_write_mtx;

const char* LevelTag(Level level) noexcept {
    switch (level) {
        case Level::kError: return "ERROR";
        case Level::kWarn:  return "WARN ";
        case Level::kInfo:  return "INFO ";
        case Level::kDebug: return "DEBUG";
    }
    return "?    ";
}

// Optional file sink, guarded by g_write_mtx so open/write never race.
std::FILE*  g_file        = nullptr;
std::string g_file_path;
std::size_t g_file_max    = 0;

}  // namespace

void SetLevel(Level level) noexcept {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

Level GetLevel() noexcept {
    return static_cast<Level>(g_level.load(std::memory_order_relaxed));
}

void SetLogFile(std::string_view path, std::size_t max_bytes) noexcept {
    std::lock_guard<std::mutex> lock(g_write_mtx);

    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
    g_file_path.clear();
    g_file_max = 0;

    if (path.empty()) return;

    std::string p(path);
    std::error_code ec;
    const std::filesystem::path fs_path(p);
    if (fs_path.has_parent_path()) {
        std::filesystem::create_directories(fs_path.parent_path(), ec);
    }

    std::FILE* f = std::fopen(p.c_str(), "ae");  // append, close-on-exec
    if (f == nullptr) {
        // Non-fatal: keep logging to stderr only.
        std::fprintf(stderr,
                     "[log] cannot open log file '%s'; stderr only\n",
                     p.c_str());
        std::fflush(stderr);
        return;
    }

    g_file      = f;
    g_file_path = std::move(p);
    g_file_max  = max_bytes;
}

void Write(Level level, std::string_view tag, std::string_view message) noexcept {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t   = clock::to_time_t(now);
    const auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                         now.time_since_epoch())
                         .count() %
                     1'000'000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    char ts[32];
    std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d.%06ld",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long>(us));

    std::lock_guard<std::mutex> lock(g_write_mtx);
    std::fprintf(stderr, "[%s] %s [%.*s] %.*s\n",
                 ts, LevelTag(level),
                 static_cast<int>(tag.size()), tag.data(),
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);

    if (g_file != nullptr) {
        std::fprintf(g_file, "[%s] %s [%.*s] %.*s\n",
                     ts, LevelTag(level),
                     static_cast<int>(tag.size()), tag.data(),
                     static_cast<int>(message.size()), message.data());
        std::fflush(g_file);

        // Single capped file: when it reaches max_bytes, truncate and start
        // over (no rotation / second file).
        if (g_file_max > 0) {
            const long pos = std::ftell(g_file);
            if (pos >= 0 &&
                static_cast<std::size_t>(pos) >= g_file_max) {
                std::fclose(g_file);
                g_file = std::fopen(g_file_path.c_str(), "we");  // truncate
            }
        }
    }
}

}  // namespace lva::log
