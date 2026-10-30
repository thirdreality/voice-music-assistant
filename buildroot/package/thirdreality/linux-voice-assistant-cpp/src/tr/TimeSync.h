#pragma once

#include <cstdint>

namespace lva::tr {

enum class TimeSyncResult { Invalid, Failed, NtpSynchronized, Unchanged, Updated };

// HA is a fallback when the kernel clock is not synchronized by NTP.
// epoch_seconds is UTC; timezone changes are handled separately.
TimeSyncResult ApplyHaTime(std::uint32_t epoch_seconds);

}  // namespace lva::tr
