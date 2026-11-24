// Checks the light bar's beat pulse and capture flash (host/io/lightbar.h). Run it from the root:
//
//     g++ -std=gnu++17 host/io/lightbar-test.cpp -o lightbar-test && ./lightbar-test

#include <stdio.h>
#include <stdlib.h>

#include "lightbar.h"

namespace {

int g_nFailures = 0;

void Expect(bool bOk, const char *pszWhat) {
    if (!bOk) {
        fprintf(stderr, "lightbar-test: %s\n", pszWhat);
        ++g_nFailures;
    }
}

bool Rgb(const unsigned char *pRgb, int nRed, int nGreen, int nBlue) {
    return pRgb[0] == nRed && pRgb[1] == nGreen && pRgb[2] == nBlue;
}

} // namespace

int main() {
    const float afPurple[3] = {0.65f, 0.0f, 1.0f};
    const float afBlue[3] = {0.0f, 0.0f, 1.0f};
    unsigned char abRgb[3];

    LightBarRgb(afPurple, 1920.0f, false, afBlue, 0.0f, abRgb);
    Expect(Rgb(abRgb, 166, 0, 255), "a beat is the full colour");
    LightBarRgb(afPurple, 1920.0f + 479.0f, false, afBlue, 0.0f, abRgb);
    Expect(abRgb[2] == 51 && abRgb[0] == 33, "the tick before a beat is the off-beat level");
    LightBarRgb(afPurple, 240.0f, false, afBlue, 0.0f, abRgb);
    Expect(abRgb[2] > 51 && abRgb[2] < 128, "half a beat on lies between the levels, low");

    unsigned char abEarly[3];
    LightBarRgb(afPurple, -240.0f, false, afBlue, 0.0f, abEarly);
    Expect(Rgb(abEarly, abRgb[0], abRgb[1], abRgb[2]), "a negative tick wraps to its beat");

    LightBarRgb(afPurple, 300.0f, true, afBlue, 0.0f, abRgb);
    Expect(Rgb(abRgb, 166, 0, 255), "steady is the full colour off the beat");

    LightBarRgb(afPurple, 300.0f, false, afBlue, LightBarFlash(0), abRgb);
    Expect(Rgb(abRgb, 0, 0, 255), "a fresh flash is the flash colour");
    LightBarRgb(afPurple, 479.0f, false, afBlue, LightBarFlash(kLightBarFlashMs / 2), abRgb);
    Expect(abRgb[0] == 17 && abRgb[2] == 153, "half way the flash is half over the pulse");
    LightBarRgb(afPurple, 479.0f, false, afBlue, LightBarFlash(kLightBarFlashMs), abRgb);
    Expect(Rgb(abRgb, 33, 0, 51), "a flash has faded by its end");
    Expect(LightBarFlash(100000) == 0.0f, "a long-ago flash weighs nothing");

    const float afOver[3] = {2.0f, -1.0f, 0.5f};
    LightBarRgb(afOver, 0.0f, true, afBlue, 3.0f, abRgb);
    Expect(Rgb(abRgb, 0, 0, 255), "a weight past 1 is clamped");
    LightBarRgb(afOver, 0.0f, true, afBlue, 0.0f, abRgb);
    Expect(Rgb(abRgb, 255, 0, 128), "components outside 0 to 1 are clamped");

    if (g_nFailures != 0) {
        return EXIT_FAILURE;
    }
    printf("GATE_LIGHTBAR_OK\n");
    return EXIT_SUCCESS;
}
