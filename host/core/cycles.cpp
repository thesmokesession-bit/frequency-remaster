#include "os/cycles.h"

#include <stdint.h>
#include <time.h>

// The monotonic clock in Emotion Engine cycles, which wraps at 32 bits as the Count register does.
unsigned ReadCycleCount() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<unsigned>(static_cast<uint64_t>(now.tv_sec) * kCyclesPerMillisecond * 1000 +
                                 static_cast<uint64_t>(now.tv_nsec) * kCyclesPerMillisecond /
                                     1000000);
}
