#include "os/cycles.h"

#include <stdint.h>
#include <time.h>

#include "../gfx-null/gfxnull.h"

// The monotonic clock in Emotion Engine cycles, which wraps at 32 bits as the Count register does.
// FREQ_CLOCK=fixed reads the frame loop's ticks instead (host/gfx-null/gfx.cpp).
unsigned ReadCycleCount() {
    const long long nFixedNs = HostFixedClockNs();
    if (nFixedNs >= 0) {
        return static_cast<unsigned>(static_cast<uint64_t>(nFixedNs) * kCyclesPerMillisecond /
                                     1000000);
    }
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<unsigned>(static_cast<uint64_t>(now.tv_sec) * kCyclesPerMillisecond * 1000 +
                                 static_cast<uint64_t>(now.tv_nsec) * kCyclesPerMillisecond /
                                     1000000);
}
