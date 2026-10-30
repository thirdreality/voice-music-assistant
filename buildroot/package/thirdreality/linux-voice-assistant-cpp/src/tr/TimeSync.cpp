#include "tr/TimeSync.h"

#include <fcntl.h>
#include <linux/rtc.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstring>

#include "util/Log.h"

namespace lva::tr {
namespace {

void WriteRtc() {
    // Best effort: boards may lack an RTC, or expose one that is read-only.
    // Use the current UTC clock, not the earlier HA timestamp. No subprocess
    // or marker is needed, and RTC failure must not invalidate system time.
    for (const char* path : {"/dev/rtc", "/dev/rtc0", "/dev/misc/rtc"}) {
        const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) continue;
        timespec now{};
        tm utc{};
        if (::clock_gettime(CLOCK_REALTIME, &now) != 0 || !::gmtime_r(&now.tv_sec, &utc)) {
            ::close(fd);
            return;
        }
        rtc_time value{};
        value.tm_sec = utc.tm_sec;
        value.tm_min = utc.tm_min;
        value.tm_hour = utc.tm_hour;
        value.tm_mday = utc.tm_mday;
        value.tm_mon = utc.tm_mon;
        value.tm_year = utc.tm_year;
        value.tm_wday = utc.tm_wday;
        value.tm_yday = utc.tm_yday;
        value.tm_isdst = 0;
        const int result = ::ioctl(fd, RTC_SET_TIME, &value);
        const int error = errno;
        ::close(fd);
        if (result == 0) {
            LVA_LOGD("time", "updated UTC RTC %s", path);
            return;
        }
        LVA_LOGD("time", "cannot update RTC %s: %s", path, std::strerror(error));
    }
}

}  // namespace

TimeSyncResult ApplyHaTime(std::uint32_t epoch_seconds) {
    // Reject unset clocks and implausible dates (2024-01-01 to 2100-01-01).
    if (epoch_seconds < 1704067200U || epoch_seconds >= 4102444800U) {
        LVA_LOGD("time", "ignoring invalid HA epoch=%u", epoch_seconds);
        return TimeSyncResult::Invalid;
    }

    timex status{};  // modes=0 only reads kernel synchronization state.
    const int ntp_state = ::adjtimex(&status);
    if (ntp_state < 0) {
        LVA_LOGW("time", "cannot read NTP state: %s", std::strerror(errno));
        return TimeSyncResult::Failed;
    }
    if (ntp_state != TIME_ERROR && !(status.status & STA_UNSYNC)) {
        LVA_LOGD("time", "keeping NTP-synchronized clock");
        return TimeSyncResult::NtpSynchronized;
    }

    timespec current{};
    if (::clock_gettime(CLOCK_REALTIME, &current) != 0) {
        LVA_LOGW("time", "cannot read system clock: %s", std::strerror(errno));
        return TimeSyncResult::Failed;
    }
    const double offset = static_cast<double>(epoch_seconds) -
                          static_cast<double>(current.tv_sec) - current.tv_nsec / 1e9;
    if (std::abs(offset) <= 2.0) return TimeSyncResult::Unchanged;

    const timespec target{static_cast<time_t>(epoch_seconds), 0};
    if (::clock_settime(CLOCK_REALTIME, &target) != 0) {
        LVA_LOGW("time", "cannot apply HA time: %s", std::strerror(errno));
        return TimeSyncResult::Failed;
    }
    LVA_LOGI("time", "clock synchronized from HA epoch=%u offset=%.3fs",
             epoch_seconds, offset);
    WriteRtc();
    return TimeSyncResult::Updated;
}

}  // namespace lva::tr
