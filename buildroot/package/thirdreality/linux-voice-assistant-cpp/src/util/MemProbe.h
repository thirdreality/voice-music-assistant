#pragma once

namespace lva::memprobe {

// Starts a detached 1 s sampler thread. It logs one "[memprobe]" line only when
// something moved: RssAnon changed by >= 1 MB, or the heap's in-use bytes
// changed by >= 1 MB, or every 60 s as a keep-alive. Lines carry the glibc
// malloc counters (allocated vs. free vs. releasable) and the threads that
// used >= 30 CPU ticks over the last 3 s, so a jump can be attributed to a
// thread even when it started just before the jump. Call once, early in main().
void Start();

// RssAnon of this process in kB, or -1 if unavailable.
long ReadRssAnonKb();

}  // namespace lva::memprobe
