#pragma once

// Diagnostic logging, compiled in only when the build defines LVA_DIAG_LOG
// (cmake -DLVA_DIAG_LOG=ON, or BR2_PACKAGE_LINUX_VOICE_ASSISTANT_CPP_DIAG_LOG).
// Release builds compile these out, so they cost nothing at runtime.

#include "util/Log.h"

#ifdef LVA_DIAG_LOG
#define LVA_DIAG_LOGI(...) LVA_LOGI(__VA_ARGS__)
#define LVA_DIAG(stmt) stmt
#else
#define LVA_DIAG_LOGI(...) ((void)0)
#define LVA_DIAG(stmt)
#endif
