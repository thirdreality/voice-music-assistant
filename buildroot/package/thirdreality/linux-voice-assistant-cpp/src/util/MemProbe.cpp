#include "util/MemProbe.h"

#include <dirent.h>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

#include "util/Log.h"

namespace lva::memprobe {

namespace {

constexpr const char* kTag = "memprobe";
constexpr long kStepKb = 1024;          // log when RssAnon / in-use moves by this much
constexpr int kKeepAliveSec = 60;

long ReadStatusKb(const char* key) {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (f == nullptr) return -1;
    char line[128];
    const std::size_t n = std::strlen(key);
    long kb = -1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, key, n) == 0) {
            std::sscanf(line + n, "%ld", &kb);
            break;
        }
    }
    std::fclose(f);
    return kb;
}

// utime+stime ticks of one thread, or -1.
long ReadThreadTicks(const std::string& tid, std::string* name) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", tid.c_str());
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return -1;
    char buf[512];
    const std::size_t len = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[len] = '\0';
    // "<tid> (<comm>) S ..." ; comm may contain spaces, so anchor on the last ')'.
    const char* lp = std::strchr(buf, '(');
    const char* rp = std::strrchr(buf, ')');
    if (lp == nullptr || rp == nullptr || rp < lp) return -1;
    if (name != nullptr) name->assign(lp + 1, rp);
    unsigned long ut = 0, st = 0;
    // fields after ')': state ppid pgrp session tty tpgid flags minflt cminflt
    // majflt cmajflt utime stime
    if (std::sscanf(rp + 2,
                    "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                    &ut, &st) != 2) {
        return -1;
    }
    return static_cast<long>(ut + st);
}

void Loop() {
    ::pthread_setname_np(::pthread_self(), "memprobe");

    long last_rss = -1;
    long last_inuse_kb = -1;
    int since_log = 0;
    std::map<std::string, long> last_ticks;  // tid -> ticks
    // Ring of the last kWindow seconds of per-thread deltas, so a memory jump
    // is reported together with who was busy just before it, not only during it.
    constexpr int kWindow = 3;
    std::map<std::string, long> window[kWindow];
    std::map<std::string, std::string> names;
    int slot = 0;

    for (;; std::this_thread::sleep_for(std::chrono::seconds(1))) {
        const long rss = ReadStatusKb("RssAnon:");
        const struct mallinfo mi = ::mallinfo();
        // Under M_ARENA_MAX=1 this is the whole malloc heap (brk + mmapped).
        const long inuse_kb = (static_cast<long>(mi.uordblks) +
                               static_cast<long>(mi.hblkhd)) / 1024;

        // Per-thread CPU deltas, kept for the last kWindow seconds.
        slot = (slot + 1) % kWindow;
        window[slot].clear();
        if (DIR* d = ::opendir("/proc/self/task")) {
            while (const dirent* e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                std::string name;
                const long t = ReadThreadTicks(e->d_name, &name);
                if (t < 0) continue;
                auto it = last_ticks.find(e->d_name);
                if (it != last_ticks.end() && t > it->second) {
                    window[slot][e->d_name] = t - it->second;
                }
                names[e->d_name] = name;
                last_ticks[e->d_name] = t;
            }
            ::closedir(d);
        }

        ++since_log;
        const bool moved =
            last_rss < 0 ||
            (rss >= 0 && (rss - last_rss >= kStepKb || last_rss - rss >= kStepKb)) ||
            (last_inuse_kb >= 0 &&
             (inuse_kb - last_inuse_kb >= kStepKb ||
              last_inuse_kb - inuse_kb >= kStepKb));
        if (moved || since_log >= kKeepAliveSec) {
            // Ticks per thread over the window; list those above ~10% of one CPU
            // (CLK_TCK is 100, so 30 ticks over 3 s).
            std::map<std::string, long> sum;
            for (const auto& w : window) {
                for (const auto& kv : w) sum[kv.first] += kv.second;
            }
            std::string busy;
            for (const auto& kv : sum) {
                if (kv.second < 30) continue;
                char item[96];
                std::snprintf(item, sizeof(item), " %.12s/%.16s+%ld",
                              kv.first.c_str(), names[kv.first].c_str(),
                              kv.second);
                busy += item;
            }
            LVA_LOGI(kTag,
                     "rss_anon=%ld kB (d%+ld) inuse=%ld kB (d%+ld) arena=%d kB "
                     "free=%d kB releasable=%d kB mmapped=%d kB blocks=%d busy_3s_ticks:%s",
                     rss, last_rss < 0 ? 0 : rss - last_rss, inuse_kb,
                     last_inuse_kb < 0 ? 0 : inuse_kb - last_inuse_kb,
                     mi.arena / 1024, mi.fordblks / 1024, mi.keepcost / 1024,
                     mi.hblkhd / 1024, mi.hblks,
                     busy.empty() ? " -" : busy.c_str());
            last_rss = rss;
            last_inuse_kb = inuse_kb;
            since_log = 0;
        }
    }
}

}  // namespace

long ReadRssAnonKb() { return ReadStatusKb("RssAnon:"); }

void Start() {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread(Loop).detach();
}

}  // namespace lva::memprobe
