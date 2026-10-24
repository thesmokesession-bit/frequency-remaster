#include "os/cycles.h"

#include <stdint.h>

extern "C" uint64_t HostAudioCycles(); // host/audio/audio.cpp

// The free-running cycle counter, which wraps at 32 bits as the Count register does. It runs on
// the audio clock, so that the song clock and everything else timed by it follow the sound. With
// FREQ_CLOCK=fixed the audio clock is the frame loop's fixed clock (host/gfx-null/gfx.cpp), and
// the audio renders up to it at each tick.
unsigned ReadCycleCount() {
    return static_cast<unsigned>(HostAudioCycles());
}
