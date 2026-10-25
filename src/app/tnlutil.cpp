#include "app/tnlutil.h"

#include "math/color.h"
#include "os/hxstr.h"

#ifdef RESONANCE_HOST
// FREQ_GEM_PALETTE=cb wraps these three (end of file); the bodies below are the originals.
#define TnlColorFromName TnlColorFromNameOriginal
#define TnlDimColorFromName TnlDimColorFromNameOriginal
#define TnlLaneColorFromName TnlLaneColorFromNameOriginal
#endif
#ifndef RESONANCE_HOST
#line 4
#endif

namespace {

// The lane floor components TnlLaneColorFromName() scales, and the purple and null blends.
constexpr float kLaneComponent = 0.5f;
constexpr float kLanePurpleRed = 0.325f;
constexpr float kLaneNullBlue = 0.25f;

// The components TnlColorFromName() and TnlDimColorFromName() give purple and the dim primaries.
constexpr float kPurpleRed = 0.65f;
constexpr float kDimComponent = 0.7f;
constexpr float kDimPurpleRed = 0.5f;

enum ColorIndex {
    kColorIndexNull = 0,
    kColorIndexGreen = 1,
    kColorIndexRed = 2,
    kColorIndexYellow = 3,
    kColorIndexPurple = 4,
};

inline Color MakeColor(float flR, float flG, float flB) {
    return Color{flR, flG, flB, 1.0f};
}

} // namespace

// NTSC-U/C: 0x006e42a4, PAL: 0x00727bc4
int g_nAppTunnelTutorial;

// NTSC-U/C: 0x006e42b0, PAL: 0x00727bd0
float g_flTunnelBrightness = 1.0f;

Color TnlColorFromName(const HxStr &name) {
    if (name == "green") {
        return MakeColor(0.0f, 1.0f, 0.0f);
    }
    if (name == "red") {
        return MakeColor(1.0f, 0.0f, 0.0f);
    }
    if (name == "yellow") {
        return MakeColor(1.0f, 1.0f, 0.0f);
    }
    if (name == "purple") {
        return MakeColor(kPurpleRed, 0.0f, 1.0f);
    }
    if (name == "null") {
        return MakeColor(1.0f, 1.0f, 1.0f);
    }
    return MakeColor(0.0f, 1.0f, 1.0f);
}

Color TnlDimColorFromName(HxStr name) {
    if (name == "green") {
        return MakeColor(0.0f, kDimComponent, 0.0f);
    }
    if (name == "red") {
        return MakeColor(kDimComponent, 0.0f, 0.0f);
    }
    if (name == "yellow") {
        return MakeColor(kDimComponent, kDimComponent, 0.0f);
    }
    if (name == "purple") {
        return MakeColor(kDimPurpleRed, 0.0f, kDimComponent);
    }
    if (name == "null") {
        return MakeColor(kDimComponent, kDimComponent, kDimComponent);
    }
    return MakeColor(0.0f, 1.0f, 1.0f);
}

Color TnlLaneColorFromName(HxStr name) {
    Color base;
    if (name == "green") {
        base = MakeColor(0.0f, kLaneComponent, 0.0f);
    } else if (name == "red") {
        base = MakeColor(kLaneComponent, 0.0f, 0.0f);
    } else if (name == "yellow") {
        base = MakeColor(kLaneComponent, kLaneComponent, 0.0f);
    } else if (name == "purple") {
        base = MakeColor(kLanePurpleRed, 0.0f, kLaneComponent);
    } else if (name == "null") {
        base = MakeColor(0.0f, 0.0f, kLaneNullBlue);
    } else {
        return MakeColor(1.0f, 1.0f, 1.0f);
    }
    Color result;
    ScaleColor(base, g_flTunnelBrightness, result);
    return result;
}

int TnlColorIndexFromName(HxStr name) {
    if (name == "null") {
        return kColorIndexNull;
    }
    if (name == "green") {
        return kColorIndexGreen;
    }
    if (name == "purple") {
        return kColorIndexPurple;
    }
    if (name == "red") {
        return kColorIndexRed;
    }
    if (name == "yellow") {
        return kColorIndexYellow;
    }
    return kColorIndexNull;
}

#ifdef RESONANCE_HOST
#include <algorithm>
#include <cmath>

namespace {

// FREQ_GEM_PALETTE=cb per colour index (null, green, red, yellow, purple): the OKLab hue (degrees),
// the factor on chroma, and a gamma on L (below 1 lightens, above darkens; black and white stay).
// The hues are the Okabe-Ito set: null sky blue, green bluish green, red vermilion, yellow yellow,
// purple blue. The factors were fitted on the rendered gems (P/p5/visual-out/cbsearch2.py) so the
// five stay apart under deuteranopia and protanopia by hue and lightness.
struct CbTarget {
    float flHue;
    float flChroma;
    float flGamma;
};
constexpr CbTarget kCbTargets[5] = {
    {230.0f, 0.98f, 0.30f},
    {170.0f, 0.74f, 2.0f},
    {40.0f, 1.3f, 1.0f},
    {100.0f, 1.6f, 0.627f},
    {255.0f, 0.82f, 1.0f},
};

float ToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float ToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

void ToOklab(const float *pRgb, float *pLab) {
    const float r = ToLinear(pRgb[0]), g = ToLinear(pRgb[1]), b = ToLinear(pRgb[2]);
    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
    pLab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    pLab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    pLab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

// OKLab to linear RGB; true when every component is inside 0..1.
bool FromOklab(float flL, float flA, float flB, float *pLin) {
    const float l = flL + 0.3963377774f * flA + 0.2158037573f * flB;
    const float m = flL - 0.1055613458f * flA - 0.0638541728f * flB;
    const float s = flL - 0.0894841775f * flA - 1.2914855480f * flB;
    const float l3 = l * l * l, m3 = m * m * m, s3 = s * s * s;
    pLin[0] = 4.0767416621f * l3 - 3.3077115913f * m3 + 0.2309699292f * s3;
    pLin[1] = -1.2684380046f * l3 + 2.6097574011f * m3 - 0.3413193965f * s3;
    pLin[2] = -0.0041960863f * l3 - 0.7034186147f * m3 + 1.7076147010f * s3;
    for (int c = 0; c < 3; ++c) {
        if (pLin[c] < -1e-4f || pLin[c] > 1.0001f) {
            return false;
        }
    }
    return true;
}

} // namespace

#undef TnlColorFromName
#undef TnlDimColorFromName
#undef TnlLaneColorFromName

int TnlCbIndex(const HxStr &name) {
    return name == "null" || name == "green" || name == "red" || name == "yellow" || name == "purple"
               ? TnlColorIndexFromName(name)
               : -1;
}

Color TnlCbColor(const Color &color, int nColor) {
    float rgb[3] = {color.r, color.g, color.b};
    if (HostGemPalette() != 0 && nColor >= 0 && nColor < 5) {
        TnlCbRecolour(rgb, nColor);
    }
    return Color{rgb[0], rgb[1], rgb[2], color.a};
}

Color TnlColorFromName(const HxStr &name) {
    return TnlCbColor(TnlColorFromNameOriginal(name), TnlCbIndex(name));
}

Color TnlDimColorFromName(HxStr name) {
    return TnlCbColor(TnlDimColorFromNameOriginal(name), TnlCbIndex(name));
}

Color TnlLaneColorFromName(HxStr name) {
    return TnlCbColor(TnlLaneColorFromNameOriginal(name), TnlCbIndex(name));
}

void TnlCbRecolour(float *pRgb, int nColor) {
    const CbTarget &target = kCbTargets[nColor];
    float lab[3];
    ToOklab(pRgb, lab);
    lab[0] = std::pow(std::max(0.0f, lab[0]), target.flGamma);
    const float flHue = target.flHue * 3.14159265f / 180.0f;
    const float flChroma = std::hypot(lab[1], lab[2]) * target.flChroma;
    float lin[3];
    // Bisect the chroma down into gamut, keeping L and hue.
    if (!FromOklab(lab[0], flChroma * std::cos(flHue), flChroma * std::sin(flHue), lin)) {
        float flLo = 0.0f, flHi = flChroma;
        for (int i = 0; i < 20; ++i) {
            const float flMid = 0.5f * (flLo + flHi);
            (FromOklab(lab[0], flMid * std::cos(flHue), flMid * std::sin(flHue), lin) ? flLo : flHi) = flMid;
        }
        FromOklab(lab[0], flLo * std::cos(flHue), flLo * std::sin(flHue), lin);
    }
    for (int c = 0; c < 3; ++c) {
        pRgb[c] = ToSrgb(std::min(1.0f, std::max(0.0f, lin[c])));
    }
}
#endif
