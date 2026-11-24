// This header computes the light bar's colour for one frame for host/io/pad.cpp, apart from SDL, so
// that host/io/lightbar-test.cpp can check it.

#pragma once

#include <math.h>

// The light bar pulses on each quarter note, which is this many song ticks.
constexpr float kLightBarBeatTicks = 480.0f;
// This is the brightness between beats, as a share of the player's colour.
constexpr float kLightBarOffBeat = 0.2f;
// A capture's flash fades over this many milliseconds.
constexpr unsigned kLightBarFlashMs = 600;

// Returns the flash's weight nElapsedMs after a capture, which falls from 1 at once to 0 at
// kLightBarFlashMs.
inline float LightBarFlash(unsigned nElapsedMs) {
    if (nElapsedMs >= kLightBarFlashMs) {
        return 0.0f;
    }
    return 1.0f - static_cast<float>(nElapsedMs) / kLightBarFlashMs;
}

// Computes the light bar's RGB for a player colour (components 0 to 1) at song tick flTick. It is
// the full colour on each beat, decaying to kLightBarOffBeat before the next (a tick before the
// first beat counts back from it), or the full colour throughout with bSteady. A capture's flash
// colour lies over it with weight flFlash.
inline void LightBarRgb(const float *pColor,
                        float flTick,
                        bool bSteady,
                        const float *pFlash,
                        float flFlash,
                        unsigned char *pRgb) {
    float flLevel = 1.0f;
    if (!bSteady) {
        const float flBeats = flTick / kLightBarBeatTicks;
        const float flDecay = 1.0f - (flBeats - floorf(flBeats));
        flLevel = kLightBarOffBeat + (1.0f - kLightBarOffBeat) * flDecay * flDecay * flDecay;
    }
    flFlash = flFlash < 0.0f ? 0.0f : flFlash > 1.0f ? 1.0f : flFlash;
    for (int c = 0; c < 3; ++c) {
        float flValue = pColor[c] * flLevel * (1.0f - flFlash) + pFlash[c] * flFlash;
        flValue = flValue < 0.0f ? 0.0f : flValue > 1.0f ? 1.0f : flValue;
        pRgb[c] = static_cast<unsigned char>(flValue * 255.0f + 0.5f);
    }
}
