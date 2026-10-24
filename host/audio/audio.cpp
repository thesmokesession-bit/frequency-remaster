// The sound of the native build: the IOP around the game's own sound driver. host/kernel/iop.cpp
// serves the driver (src/ezmidi, EZMIDI.IRX on the disc) behind its SIF RPC server. This file gives
// the driver, and the EE's sceSdRemote() calls, Sony's libsd on Play!'s SPU2 (BSD-2, play/), runs
// the driver's 2083 us synth timer, and sends the mix to SDL2 audio and to FREQ_WAV.
//
// The audio is the clock. One thread renders 10 ms blocks at the wall-clock rate; the synth ticks
// and the SPU2 advance by rendered sample count, and the EE cycle counter (host/core/cycles.cpp)
// reads the rendered position plus a fixed lead, so the game's song clock follows the audio
// however slowly frames draw. Each MIDI message the EE sends is stamped with its due time (the
// scheduler command sending it may run late; on the console the game polls its scheduler all
// through a frame, here it can idle a frame at the swap) and reaches the synth at the tick that
// time falls in. The lead absorbs that lateness, so the music plays the lead behind the game.

#include <algorithm>
#include <atomic>
#include <deque>
#include <fcntl.h>
#include <limits.h>
#include <mutex>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <libsdr.h>

#include <SDL.h>

#include "Iop_Spu2.h"
#include "Iop_SpuBase.h"

extern "C" {
void HostUseSystemHeap(int bOn); // host/core/heap.c
int sce_midi_loop(void);         // src/ezmidi/midi_com.c
void HostHsynTick(void);         // src/ezmidi/midi_hsyn.c
void *HostHsynInput(void);       // src/ezmidi/midi_hsyn.c
}

namespace {

constexpr int kRate = 48000;
constexpr uint64_t kEeCyclesPerSample = 6144; // 294912 EE cycles per millisecond
constexpr uint64_t kIopCyclesPerSample = 768; // 36.864 MHz IOP system clock
constexpr int kBlock = 480;                   // frames per rendered block, 10 ms
constexpr int64_t kBlockNs = 10000000;
constexpr uint64_t kLeadCycles = 25 * 294912;        // 25 ms, a frame and a scheduler poll
constexpr int64_t kMaxLagNs = 200000000;             // a longer stall is waited out, not caught up
constexpr unsigned kMaxQueuedBytes = 4 * kRate / 10; // 100 ms of S16 stereo
constexpr uint32_t kSpuRamSize = 2 << 20;
constexpr uint32_t kRegisterBase = 0x1F900000;

// The driver's two MIDI stream buffers, sceCslMidiStream with their data.
constexpr unsigned kMidiBufferSize = 1024;
constexpr int kMidiBuffers = 2;

// Everything on the IOP side runs under this lock: RPC calls, SIF DMA into IOP memory, the
// remote libsd calls, and the renderer.
std::mutex g_iop;

struct Spu {
    uint8_t ram[kSpuRamSize] = {};
    Iop::CSpuSampleCache cache;
    Iop::CSpuIrqWatcher irq;
    Iop::CSpuBase core0{ram, kSpuRamSize, &cache, &irq, 0};
    Iop::CSpuBase core1{ram, kSpuRamSize, &cache, &irq, 1};
    Iop::CSpu2 spu2{core0, core1};
    uint16_t shadow[0x800 / 2] = {}; // the last value written to each register
};
Spu *g_pSpu;

// Set once SdSetEffectAttr() has placed a core's effect work area.
bool g_abEffectSet[2] = {};

// The synth timer of the driver, in IOP cycles, and how far the renderer is into its period.
bool g_bTicking = false;
uint64_t g_nTickCycles = 76787;
uint64_t g_nTickAccumulated = 0;

// Rendered frames, and the clock the EE reads: the cycle count at the end of the last block and
// the wall time it was rendered at.
uint64_t g_nRendered = 0;
std::atomic<uint64_t> g_nAnchorCycles{0};
std::atomic<int64_t> g_nAnchorNs{0};
std::atomic<uint64_t> g_nLastCycles{0};
bool g_bStarted = false;

// MIDI messages the EE has shipped that the synth has not been given, with their due times.
struct MidiMessage {
    uint64_t nStamp;
    uint8_t nLength;
    uint8_t aBytes[3];
};
std::deque<MidiMessage> g_midi;

// The due time of each message in the EE's stream buffer since it last shipped, by byte offset.
// Only game threads touch it, one at a time.
struct MidiDue {
    unsigned nOffset;
    uint64_t nStamp;
};
std::vector<MidiDue> g_midiDue;

SDL_AudioDeviceID g_nDevice = 0;
FILE *g_pWav = nullptr;
uint64_t g_nWavFrames = 0;

int64_t NowNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

uint64_t EeClock() {
    if (!g_bStarted) {
        return 0;
    }
    uint64_t nCycles;
    int64_t nAnchorNs;
    do {
        nCycles = g_nAnchorCycles.load();
        nAnchorNs = g_nAnchorNs.load();
    } while (nCycles != g_nAnchorCycles.load());
    // The EE moves through the block the renderer makes next, and no further, a lead ahead.
    const int64_t nSince = std::clamp<int64_t>(NowNs() - nAnchorNs, 0, kBlockNs);
    const uint64_t nNow = nCycles + kLeadCycles + static_cast<uint64_t>(nSince) * 294912 / 1000000;
    uint64_t nLast = g_nLastCycles.load();
    while (nNow > nLast && !g_nLastCycles.compare_exchange_weak(nLast, nNow)) {
    }
    return std::max(nNow, nLast);
}

// ---- SPU2 registers through libsd entry values --------------------------------------------------

// The register of core `nCore` whose core 0 offset is nOffset.
uint32_t CoreRegister(int nCore, uint32_t nOffset) {
    return nOffset >= 0x760 ? nOffset + nCore * 0x28 : nOffset + nCore * 0x400;
}

void WriteRegister(uint32_t nOffset, uint32_t nValue) {
    g_pSpu->shadow[nOffset / 2] = static_cast<uint16_t>(nValue);
    g_pSpu->spu2.WriteRegister(kRegisterBase + nOffset, nValue & 0xffff);
}

uint32_t ReadRegister(uint32_t nOffset) {
    return g_pSpu->spu2.ReadRegister(kRegisterBase + nOffset) & 0xffff;
}

int EntryCore(unsigned nEntry) {
    return nEntry & 1;
}

int EntryVoice(unsigned nEntry) {
    return (nEntry >> 1) & 0x1f;
}

unsigned EntryId(unsigned nEntry) {
    return (nEntry >> 8) & 0xff;
}

// SD_P_MMIX to SD_P_MVOLXR, and SD_S_PMON to SD_S_VMIXER.
constexpr uint32_t kParamRegisters[] = {
    0x198, 0x760, 0x762, 0x764, 0x766, 0x768, 0x76a, 0x76c, 0x76e, 0x770, 0x772};
constexpr uint32_t kSwitchRegisters[] = {
    0x180, 0x184, 0x1a0, 0x1a4, 0x340, 0x188, 0x18c, 0x190, 0x194};
constexpr unsigned kFirstParam = 0x08;
constexpr unsigned kFirstSwitch = 0x13;
constexpr unsigned kSwitchEndX = 0x17;
constexpr unsigned kVoiceParams = 8;
constexpr unsigned kVoiceEnvX = 5;
constexpr uint32_t kAttrRegister = 0x19a;

// The register an SD_VP_ or SD_P_ entry names, or 0.
uint32_t ParamRegister(unsigned nEntry) {
    const unsigned nId = EntryId(nEntry);
    if (nId < kVoiceParams) {
        return CoreRegister(EntryCore(nEntry), EntryVoice(nEntry) * 0x10 + nId * 2);
    }
    if (nId - kFirstParam < sizeof(kParamRegisters) / sizeof(kParamRegisters[0])) {
        return CoreRegister(EntryCore(nEntry), kParamRegisters[nId - kFirstParam]);
    }
    return 0;
}

uint32_t SwitchRegister(unsigned nEntry) {
    const unsigned nId = EntryId(nEntry);
    if (nId - kFirstSwitch < sizeof(kSwitchRegisters) / sizeof(kSwitchRegisters[0])) {
        return CoreRegister(EntryCore(nEntry), kSwitchRegisters[nId - kFirstSwitch]);
    }
    return 0;
}

// SD_A_ESA to SD_A_IRQA, then the voice addresses SSA, LSAX, and NAX.
uint32_t AddressRegister(unsigned nEntry) {
    static constexpr uint32_t kCoreAddresses[] = {0x2e0, 0x33c, 0x1a8, 0x19c};
    const unsigned nId = EntryId(nEntry);
    if (nId >= 0x1c && nId < 0x20) {
        return CoreRegister(EntryCore(nEntry), kCoreAddresses[nId - 0x1c]);
    }
    if (nId >= 0x20 && nId < 0x23) {
        return CoreRegister(EntryCore(nEntry), 0x1c0 + EntryVoice(nEntry) * 12 + (nId - 0x20) * 4);
    }
    return 0;
}

void SdSetParam(unsigned nEntry, unsigned nValue) {
    if (const uint32_t nRegister = ParamRegister(nEntry)) {
        WriteRegister(nRegister, nValue);
    }
}

unsigned SdGetParam(unsigned nEntry) {
    const uint32_t nRegister = ParamRegister(nEntry);
    if (nRegister == 0) {
        return 0;
    }
    // The envelope and the current volumes are the SPU2's; the rest read back what was written.
    if (EntryId(nEntry) >= kVoiceEnvX && EntryId(nEntry) < kVoiceParams) {
        return ReadRegister(nRegister);
    }
    return g_pSpu->shadow[nRegister / 2];
}

// A switch is a register pair: voices 0 to 15, then 16 to 23.
void SdSetSwitch(unsigned nEntry, unsigned nValue) {
    if (const uint32_t nRegister = SwitchRegister(nEntry)) {
        WriteRegister(nRegister, nValue & 0xffff);
        WriteRegister(nRegister + 2, (nValue >> 16) & 0xff);
    }
}

unsigned SdGetSwitch(unsigned nEntry) {
    const uint32_t nRegister = SwitchRegister(nEntry);
    if (nRegister == 0) {
        return 0;
    }
    if (EntryId(nEntry) == kSwitchEndX) {
        return ReadRegister(nRegister) | (ReadRegister(nRegister + 2) << 16);
    }
    return g_pSpu->shadow[nRegister / 2] | (g_pSpu->shadow[nRegister / 2 + 1] << 16);
}

// An address is a byte address in a register pair of halfword units, the high bits first.
void SdSetAddr(unsigned nEntry, unsigned nValue) {
    if (const uint32_t nRegister = AddressRegister(nEntry)) {
        WriteRegister(nRegister, (nValue >> 17) & 0xffff);
        WriteRegister(nRegister + 2, (nValue >> 1) & 0xffff);
    }
}

unsigned SdGetAddr(unsigned nEntry) {
    const uint32_t nRegister = AddressRegister(nEntry);
    if (nRegister == 0) {
        return 0;
    }
    return (ReadRegister(nRegister) << 17) | (ReadRegister(nRegister + 2) << 1);
}

// The core attribute bits SD_C_EFFECT_ENABLE, SD_C_IRQ_ENABLE, SD_C_MUTE_ENABLE, and the noise
// clock select.
constexpr uint16_t kAttrEnable = 0x8000;
constexpr uint16_t kAttrEffect = 0x80;

unsigned SdGetCoreAttr(unsigned nEntry);

void SdSetCoreAttr(unsigned nEntry, unsigned nValue) {
    const int nCore = EntryCore(nEntry);
    const uint32_t nRegister = CoreRegister(nCore, kAttrRegister);
    uint16_t nAttr = g_pSpu->shadow[nRegister / 2];
    const auto set = [&](uint16_t nBits) {
        nAttr = static_cast<uint16_t>(nValue != 0 ? (nAttr | nBits) : (nAttr & ~nBits));
    };
    switch ((nEntry >> 1) & 0x7) {
    case 1:
        set(kAttrEffect);
        break;
    case 2:
        set(0x40);
        break;
    case 3:
        set(0x4000);
        break;
    case 4:
        nAttr = static_cast<uint16_t>((nAttr & ~0x3f00) | ((nValue & 0x3f) << 8));
        break;
    default:
        return;
    }
    // The effect processor stays off until SdSetEffectAttr() has given the core its work area;
    // on without one, it would write over sound memory.
    const uint16_t nUsable = g_abEffectSet[nCore] ? nAttr : nAttr & ~kAttrEffect;
    g_pSpu->spu2.WriteRegister(kRegisterBase + nRegister, nUsable);
    g_pSpu->shadow[nRegister / 2] = nAttr;
}

// sceSdSetEffectAttr(): libsd's reverb presets. The tables are LIBSD.IRX's (the disc's, version
// 2310): per mode, the work area in 8-byte units, and the 32 reverb registers in the PSX order
// dAPF1, dAPF2, vIIR, vCOMB1-4, vWALL, vAPF1-2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2,
// dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1,
// mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN, with addresses in 8-byte units. Modes: off, room, studio
// A-C, hall, space, echo, delay, pipe.
constexpr uint16_t kEffectAreaSize[10] = {
    0x2, 0x4d8, 0x3e8, 0x908, 0xdfc, 0x15bc, 0x1ed8, 0x3008, 0x3008, 0x780};
constexpr uint16_t kEffectPresets[10][32] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
     0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x007d, 0x005b, 0x6d80, 0x54b8, 0xbed0, 0x0000, 0x0000, 0xba80, 0x5800, 0x5300, 0x04d6,
     0x0333, 0x03f0, 0x0227, 0x0374, 0x01ef, 0x0336, 0x01b7, 0x0335, 0x01b6, 0x0334, 0x01b5,
     0x0334, 0x01b5, 0x0334, 0x01b5, 0x01b4, 0x0136, 0x00b8, 0x005c, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70f0, 0x4fa8, 0xbce0, 0x4410, 0xc0f0, 0x9c00, 0x5280, 0x4ec0, 0x03e4,
     0x031b, 0x03a4, 0x02af, 0x0372, 0x0266, 0x031c, 0x025d, 0x025c, 0x018e, 0x022f, 0x0135,
     0x01d2, 0x00b7, 0x018f, 0x00b5, 0x00b4, 0x0080, 0x004c, 0x0026, 0x8000, 0x8000},
    {0x00b1, 0x007f, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xb4c0, 0x5280, 0x4ec0, 0x0904,
     0x076b, 0x0824, 0x065f, 0x07a2, 0x0616, 0x076c, 0x05ed, 0x05ec, 0x042e, 0x050f, 0x0305,
     0x0462, 0x02b7, 0x042f, 0x0265, 0x0264, 0x01b2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00e3, 0x00a9, 0x6f60, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xa680, 0x5680, 0x52c0, 0x0dfb,
     0x0b58, 0x0d09, 0x0a3c, 0x0bd9, 0x0973, 0x0b59, 0x08da, 0x08d9, 0x05e9, 0x07ec, 0x04b0,
     0x06ef, 0x03d2, 0x05ea, 0x031d, 0x031c, 0x0238, 0x0154, 0x00aa, 0x8000, 0x8000},
    {0x01a5, 0x0139, 0x6000, 0x5000, 0x4c00, 0xb800, 0xbc00, 0xc000, 0x6000, 0x5c00, 0x15ba,
     0x11bb, 0x14c2, 0x10bd, 0x11bc, 0x0dc1, 0x11c0, 0x0dc3, 0x0dc0, 0x09c1, 0x0bc4, 0x07c1,
     0x0a00, 0x06cd, 0x09c2, 0x05c1, 0x05c0, 0x041a, 0x0274, 0x013a, 0x8000, 0x8000},
    {0x033d, 0x0231, 0x7e00, 0x5000, 0xb400, 0xb000, 0x4c00, 0xb000, 0x6000, 0x5400, 0x1ed6,
     0x1a31, 0x1d14, 0x183b, 0x1bc2, 0x16b2, 0x1a32, 0x15ef, 0x15ee, 0x1055, 0x1334, 0x0f2d,
     0x11f6, 0x0c5d, 0x1056, 0x0ae1, 0x0ae0, 0x07a2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7fff, 0x7fff, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1ffd,
     0x0ffd, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1fff, 0x1fff, 0x1ffe, 0x1ffe,
     0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7fff, 0x7fff, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1ffd,
     0x0ffd, 0x1009, 0x0009, 0x0000, 0x0000, 0x1009, 0x0009, 0x1fff, 0x1fff, 0x1ffe, 0x1ffe,
     0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0x8500, 0x5f80, 0x54c0, 0x0371,
     0x02af, 0x02e5, 0x01df, 0x02b0, 0x01d7, 0x0358, 0x026a, 0x01d6, 0x011e, 0x012d, 0x00b1,
     0x011f, 0x0059, 0x01a0, 0x00e3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};
constexpr int kEffectModeEcho = 7;
constexpr int kEffectModeDelay = 8;
constexpr unsigned kEffectModeClear = 0x100;

struct EffectAttr { // sceSdEffectAttr
    int nCore;
    int nMode;
    short nDepthL;
    short nDepthR;
    int nDelay;
    int nFeedback;
};

void SdSetEffectAttr(int nCore, const EffectAttr &attr) {
    const int nMode = attr.nMode & 0xff;
    if (nMode >= 10) {
        return;
    }
    uint16_t aRegisters[32];
    memcpy(aRegisters, kEffectPresets[nMode], sizeof(aRegisters));
    // Echo and delay lengthen their lines by the delay (0-127) and feed back by the feedback, as
    // LIBSD.IRX does; their tables hold delay 127 and feedback 128 (echo) or 0 (delay).
    if (nMode == kEffectModeEcho || nMode == kEffectModeDelay) {
        const int nStep = (attr.nDelay + 1) << 5;
        aRegisters[10] = static_cast<uint16_t>(2 * nStep - aRegisters[0]);
        aRegisters[11] = static_cast<uint16_t>(nStep - aRegisters[1]);
        aRegisters[16] = static_cast<uint16_t>(aRegisters[17] + nStep);
        aRegisters[12] = static_cast<uint16_t>(aRegisters[13] + nStep);
        aRegisters[27] = static_cast<uint16_t>(aRegisters[29] + nStep);
        aRegisters[26] = static_cast<uint16_t>(aRegisters[28] + nStep);
        aRegisters[7] = static_cast<uint16_t>(attr.nFeedback * 258);
    }
    // The work area ends at the core's EEA, which the game set, and spans the mode's size.
    const uint32_t nEnd = (ReadRegister(CoreRegister(nCore, 0x33c)) << 17) | 0x1ffff;
    const uint32_t nStart = nEnd - (kEffectAreaSize[nMode] * 8u - 2);
    WriteRegister(CoreRegister(nCore, 0x2e0), (nStart >> 17) & 0xffff);
    WriteRegister(CoreRegister(nCore, 0x2e2), (nStart >> 1) & 0xffff);
    static constexpr uint32_t kCoefficients[32] = {
        0, 0, 0x774, 0x776, 0x778, 0x77a, 0x77c, 0x77e, 0x780, 0x782, 0, 0, 0, 0, 0,     0,
        0, 0, 0,     0,     0,     0,     0,     0,     0,     0,     0, 0, 0, 0, 0x784, 0x786};
    int nAddress = 0;
    for (int i = 0; i < 32; ++i) {
        if (kCoefficients[i] != 0) {
            WriteRegister(CoreRegister(nCore, kCoefficients[i]), aRegisters[i]);
        } else {
            const uint32_t nHalfwords = aRegisters[i] * 4u; // 8-byte units to SPU2 halfwords
            const uint32_t nRegister = CoreRegister(nCore, 0x2e4 + 4 * nAddress++);
            WriteRegister(nRegister, nHalfwords >> 16);
            WriteRegister(nRegister + 2, nHalfwords & 0xffff);
        }
    }
    WriteRegister(CoreRegister(nCore, 0x764), static_cast<uint16_t>(attr.nDepthL));
    WriteRegister(CoreRegister(nCore, 0x766), static_cast<uint16_t>(attr.nDepthR));
    if ((attr.nMode & kEffectModeClear) != 0 && nEnd < kSpuRamSize) {
        memset(g_pSpu->ram + nStart, 0, nEnd + 1 - nStart);
        g_pSpu->cache.ClearRange(nStart, nEnd + 1 - nStart);
    }
    g_abEffectSet[nCore] = true;
    SdSetCoreAttr(SD_C_EFFECT_ENABLE | nCore, SdGetCoreAttr(SD_C_EFFECT_ENABLE | nCore));
}

unsigned SdGetCoreAttr(unsigned nEntry) {
    const uint16_t nAttr = g_pSpu->shadow[CoreRegister(EntryCore(nEntry), kAttrRegister) / 2];
    switch ((nEntry >> 1) & 0x7) {
    case 1:
        return (nAttr & kAttrEffect) != 0;
    case 2:
        return (nAttr & 0x40) != 0;
    case 3:
        return (nAttr & 0x4000) != 0;
    case 4:
        return (nAttr >> 8) & 0x3f;
    default:
        return 0;
    }
}

// sceSdInit(): both cores reset and enabled. Sound memory keeps its contents.
void SdInit() {
    for (Iop::CSpuBase *pCore : {&g_pSpu->core0, &g_pSpu->core1}) {
        pCore->Reset();
        pCore->SetDestinationSamplingRate(kRate);
    }
    memset(g_pSpu->shadow, 0, sizeof(g_pSpu->shadow));
    g_pSpu->cache.Clear();
    g_abEffectSet[0] = g_abEffectSet[1] = false;
    for (int nCore = 0; nCore < 2; ++nCore) {
        WriteRegister(CoreRegister(nCore, kAttrRegister), kAttrEnable);
    }
}

// ---- The renderer -------------------------------------------------------------------------------

// Hands the synth the MIDI due by this tick, in order, as full as its two buffers hold.
void ReleaseMidi() {
    auto *pInput = static_cast<uint8_t *>(HostHsynInput());
    const uint64_t nNow = g_nRendered * kEeCyclesPerSample;
    for (int nBuffer = 0; nBuffer < kMidiBuffers; ++nBuffer) {
        uint8_t *pStream = pInput + nBuffer * kMidiBufferSize;
        uint32_t aHeader[2]; // sceCslMidiStream: buffer size, valid size
        memcpy(aHeader, pStream, sizeof(aHeader));
        if (aHeader[1] != 0) {
            continue;
        }
        uint32_t nValid = 0;
        while (!g_midi.empty() && g_midi.front().nStamp <= nNow &&
               nValid + g_midi.front().nLength <= kMidiBufferSize - sizeof(aHeader)) {
            memcpy(
                pStream + sizeof(aHeader) + nValid, g_midi.front().aBytes, g_midi.front().nLength);
            nValid += g_midi.front().nLength;
            g_midi.pop_front();
        }
        aHeader[0] = kMidiBufferSize;
        aHeader[1] = nValid;
        memcpy(pStream, aHeader, sizeof(aHeader));
    }
}

void RenderLocked(int16_t *pOut, int nFrames) {
    int16_t aCore0[2 * kBlock];
    int16_t aCore1[2 * kBlock];
    while (nFrames > 0) {
        int nRun = std::min(nFrames, kBlock);
        if (g_bTicking) {
            if (g_nTickAccumulated >= g_nTickCycles) {
                g_nTickAccumulated -= g_nTickCycles;
                ReleaseMidi();
                HostHsynTick();
            }
            const uint64_t nToTick = g_nTickCycles - g_nTickAccumulated;
            nRun = std::min<int>(
                nRun, static_cast<int>((nToTick + kIopCyclesPerSample - 1) / kIopCyclesPerSample));
            g_nTickAccumulated += nRun * kIopCyclesPerSample;
        }
        g_pSpu->core0.Render(aCore0, 2 * nRun);
        g_pSpu->core1.Render(aCore1, 2 * nRun);
        // Core 0's output is core 1's external input, at core 1's AVOL (as in Play!'s PS2VM).
        for (int i = 0; i < 2 * nRun; ++i) {
            const int32_t nVolume =
                (i & 1) ? g_pSpu->core1.m_extInputVolR : g_pSpu->core1.m_extInputVolL;
            Iop::CSpuBase::MixSamples(aCore0[i], nVolume, &aCore1[i]);
        }
        memcpy(pOut, aCore1, sizeof(int16_t) * 2 * nRun);
        pOut += 2 * nRun;
        nFrames -= nRun;
        g_nRendered += nRun;
    }
}

void WriteWavHeader() {
    const uint32_t nData = static_cast<uint32_t>(g_nWavFrames * 4);
    const uint32_t aHeader[] = {0x46464952,
                                36 + nData,
                                0x45564157,
                                0x20746d66,
                                16,
                                0x00020001,
                                kRate,
                                kRate * 4,
                                0x00100004,
                                0x61746164,
                                nData};
    fseek(g_pWav, 0, SEEK_SET);
    fwrite(aHeader, sizeof(aHeader), 1, g_pWav);
    fseek(g_pWav, 0, SEEK_END);
    fflush(g_pWav);
}

void RenderBlock() {
    int16_t aBlock[2 * kBlock];
    {
        std::lock_guard<std::mutex> lock(g_iop);
        RenderLocked(aBlock, kBlock);
    }
    g_nAnchorNs.store(NowNs());
    g_nAnchorCycles.store(g_nRendered * kEeCyclesPerSample);
    if (g_nDevice != 0) {
        // A device slower than the wall clock loses a block now and then, rather than lagging.
        if (SDL_GetQueuedAudioSize(g_nDevice) < kMaxQueuedBytes) {
            SDL_QueueAudio(g_nDevice, aBlock, sizeof(aBlock));
        }
    }
    if (g_pWav != nullptr) {
        fwrite(aBlock, sizeof(aBlock), 1, g_pWav);
        g_nWavFrames += kBlock;
        if (g_nWavFrames % kRate == 0) {
            WriteWavHeader();
        }
    }
}

void *RunAudio(void *) {
    HostUseSystemHeap(1);
    // The clock of the whole game runs here; ahead of the game's threads where that is allowed.
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -15);
    int64_t nDeadline = NowNs();
    for (;;) {
        RenderBlock();
        nDeadline += kBlockNs;
        const int64_t nNow = NowNs();
        if (nNow - nDeadline > kMaxLagNs) {
            nDeadline = nNow; // starved: the song waits rather than jumps
        }
        const timespec until = {static_cast<time_t>(nDeadline / 1000000000),
                                static_cast<long>(nDeadline % 1000000000)};
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr);
    }
    return nullptr;
}

void OpenDevice() {
    // Without a session bus, D-Bus would try to start one for SDL2 and wait on it for ever.
    setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", 0);
    // SIGINT and SIGTERM end the process, as before there was sound, not post an SDL_QUIT.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "AUDIO_NO_DEVICE %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want = {};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = kBlock;
    SDL_AudioSpec have;
    g_nDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_nDevice == 0) {
        fprintf(stderr, "AUDIO_NO_DEVICE %s\n", SDL_GetError());
        return;
    }
    // Two blocks of silence ahead of the first, so the device never waits on the renderer.
    const std::vector<int16_t> silence(4 * kBlock);
    SDL_QueueAudio(g_nDevice, silence.data(), silence.size() * sizeof(int16_t));
    SDL_PauseAudioDevice(g_nDevice, 0);
    fprintf(stderr, "AUDIO_OPEN %s %d Hz\n", SDL_GetCurrentAudioDriver(), have.freq);
}

} // namespace

// ---- What the rest of the host calls ------------------------------------------------------------

extern "C" {

// The EE cycle counter, on the audio clock.
uint64_t HostAudioCycles() {
    return EeClock();
}

void HostIopLock() {
    g_iop.lock();
}

void HostIopUnlock() {
    g_iop.unlock();
}

// src/synth/midi_main.cpp: the EE puts a message at nOffset of its stream buffer, due nLateNs ago.
void HostMidiQueued(unsigned nOffset, long long nLateNs) {
    const uint64_t nLate = static_cast<uint64_t>(std::max(nLateNs, 0LL)) * 294912 / 1000000;
    const uint64_t nNow = EeClock();
    g_midiDue.push_back({nOffset, nNow - std::min(nLate, nNow)});
}

// A SIF DMA write into IOP memory, under the IOP lock. MIDI for the synth waits for its due time:
// the shipped stream (sceCslMidiStream) is split into messages, each stamped with the due time
// HostMidiQueued() recorded for its offset, or now.
void HostIopWrite(void *pDest, const void *pSource, unsigned nSize) {
    uint8_t *pInput = static_cast<uint8_t *>(HostHsynInput());
    uint8_t *pDestBytes = static_cast<uint8_t *>(pDest);
    if (!g_bTicking || pDestBytes < pInput ||
        pDestBytes >= pInput + kMidiBuffers * kMidiBufferSize) {
        memcpy(pDest, pSource, nSize);
        return;
    }
    const uint8_t *pData = static_cast<const uint8_t *>(pSource) + 2 * sizeof(uint32_t);
    const unsigned nData = nSize > 2 * sizeof(uint32_t) ? nSize - 2 * sizeof(uint32_t) : 0;
    const uint64_t nNow = EeClock();
    size_t nDue = 0;
    uint64_t nStamp = nNow;
    for (unsigned nOffset = 0; nOffset < nData;) {
        while (nDue < g_midiDue.size() && g_midiDue[nDue].nOffset <= nOffset) {
            nStamp = g_midiDue[nDue++].nStamp;
        }
        // As msin queues them: note off, program change, and channel pressure are two bytes.
        const unsigned nType = pData[nOffset] & 0xf0;
        MidiMessage message = {
            nStamp,
            static_cast<uint8_t>(nType == 0x80 || nType == 0xc0 || nType == 0xd0 ? 2 : 3),
            {}};
        message.nLength =
            static_cast<uint8_t>(std::min<unsigned>(message.nLength, nData - nOffset));
        memcpy(message.aBytes, pData + nOffset, message.nLength);
        g_midi.push_back(message);
        nOffset += message.nLength;
    }
    g_midiDue.clear();
}

// EZMIDI.IRX's start: its server thread's body, which registers the server and returns here.
void HostLoadEzmidi() {
    std::lock_guard<std::mutex> lock(g_iop);
    sce_midi_loop();
}

void HostAudioStart() {
    HostUseSystemHeap(1);
    g_pSpu = new Spu;
    SdInit();
    if (const char *pszWav = getenv("FREQ_WAV")) {
        // The game defines its own fopen() and write() (src/os/filelog.cpp), so the file is
        // opened with open() and written through the C library's stream.
        const int nFile = open(pszWav, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        g_pWav = nFile >= 0 ? fdopen(nFile, "wb") : nullptr;
        if (g_pWav == nullptr) {
            fprintf(stderr, "host: cannot write FREQ_WAV=%s\n", pszWav);
            exit(70);
        }
        WriteWavHeader();
    }
    OpenDevice();
    HostUseSystemHeap(0);
    g_nAnchorNs.store(NowNs());
    g_bStarted = true;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, RunAudio, nullptr) != 0) {
        fprintf(stderr, "host: cannot start the audio thread\n");
        exit(70);
    }
    pthread_detach(thread);
}

// ---- libsd, as the driver calls it on the IOP --------------------------------------------------

void sceSdSetParam(unsigned short nEntry, unsigned short nValue) {
    SdSetParam(nEntry, nValue);
}

unsigned short sceSdGetParam(unsigned short nEntry) {
    return static_cast<unsigned short>(SdGetParam(nEntry));
}

void sceSdSetSwitch(unsigned short nEntry, unsigned int nValue) {
    SdSetSwitch(nEntry, nValue);
}

unsigned int sceSdGetSwitch(unsigned short nEntry) {
    return SdGetSwitch(nEntry);
}

void sceSdSetAddr(unsigned short nEntry, unsigned int nValue) {
    SdSetAddr(nEntry, nValue);
}

// 0x1000 plays the sample at its recorded rate; fine is in 128ths of a semitone.
unsigned short sceSdNote2Pitch(unsigned short nCenterNote,
                               unsigned short nCenterFine,
                               unsigned short nNote,
                               short nFine) {
    const int nOffset = (nNote - nCenterNote) * 128 + (nFine - nCenterFine);
    const double flPitch = 4096.0 * exp2(nOffset / (12.0 * 128.0));
    return static_cast<unsigned short>(std::min(flPitch + 0.5, 65535.0));
}

int sceSdVoiceTrans(
    short, unsigned short nMode, unsigned char *pIop, unsigned int nSpu, unsigned int nSize) {
    if (nSpu >= kSpuRamSize) {
        return -1;
    }
    nSize = std::min(nSize, kSpuRamSize - nSpu);
    if (nMode & 1) {
        memcpy(pIop, g_pSpu->ram + nSpu, nSize);
    } else {
        memcpy(g_pSpu->ram + nSpu, pIop, nSize);
        g_pSpu->cache.ClearRange(nSpu, nSize);
    }
    return static_cast<int>(nSize);
}

unsigned int sceSdVoiceTransStatus(short, short) {
    return 1; // the copy is done when sceSdVoiceTrans() returns
}

// ---- The IOP kernel the driver calls ------------------------------------------------------------

struct IopSysClock {
    unsigned low;
    unsigned hi;
};

int IopCpuEnableIntr() {
    return 0;
}

int IopEnableIntr(int) {
    return 0;
}

// The synth's tick thread never runs: RenderLocked() runs its loop's body instead.
int IopCreateThread(void *) {
    return 1;
}

int IopStartThread(int, unsigned long) {
    return 0;
}

int IopGetThreadId() {
    return 1;
}

int IopSleepThread() {
    return 0;
}

int IopiWakeupThread(int) {
    return 0;
}

void IopGetSystemTime(IopSysClock *pClock) {
    const uint64_t nCycles = g_nRendered * kIopCyclesPerSample;
    pClock->low = static_cast<unsigned>(nCycles);
    pClock->hi = static_cast<unsigned>(nCycles >> 32);
}

void IopUSec2SysClock(unsigned nMicroseconds, IopSysClock *pClock) {
    const uint64_t nCycles = static_cast<uint64_t>(nMicroseconds) * 36864 / 1000;
    pClock->low = static_cast<unsigned>(nCycles);
    pClock->hi = static_cast<unsigned>(nCycles >> 32);
}

int IopAllocHardTimer(int, int, int) {
    return 1;
}

int IopFreeHardTimer(int) {
    return 0;
}

int IopSetTimerHandler(int, unsigned long nCompare, void *, void *) {
    g_nTickCycles = nCompare;
    return 0;
}

int IopSetupHardTimer(int, int, int, int) {
    return 0;
}

int IopStartHardTimer(int) {
    g_nTickAccumulated = 0;
    g_bTicking = true;
    return 0;
}

int IopStopHardTimer(int) {
    g_bTicking = false;
    return 0;
}

// ---- libsdr: the EE's remote libsd calls --------------------------------------------------------

int sceSdRemoteInit() {
    return 0;
}

int sceSdRemote(int nWait, ...) {
    va_list arguments;
    va_start(arguments, nWait);
    const int nCommand = va_arg(arguments, int);
    std::lock_guard<std::mutex> lock(g_iop);
    int nResult = 0;
    switch (nCommand) {
    case rSdInit:
        SdInit();
        break;
    case rSdSetParam: {
        const unsigned nEntry = va_arg(arguments, unsigned);
        SdSetParam(nEntry, va_arg(arguments, unsigned));
        break;
    }
    case rSdGetParam:
        nResult = static_cast<int>(SdGetParam(va_arg(arguments, unsigned)));
        break;
    case rSdSetSwitch: {
        const unsigned nEntry = va_arg(arguments, unsigned);
        SdSetSwitch(nEntry, va_arg(arguments, unsigned));
        break;
    }
    case rSdGetSwitch:
        nResult = static_cast<int>(SdGetSwitch(va_arg(arguments, unsigned)));
        break;
    case rSdSetAddr: {
        const unsigned nEntry = va_arg(arguments, unsigned);
        SdSetAddr(nEntry, va_arg(arguments, unsigned));
        break;
    }
    case rSdGetAddr:
        nResult = static_cast<int>(SdGetAddr(va_arg(arguments, unsigned)));
        break;
    case rSdSetCoreAttr: {
        const unsigned nEntry = va_arg(arguments, unsigned);
        SdSetCoreAttr(nEntry, va_arg(arguments, unsigned));
        break;
    }
    case rSdGetCoreAttr:
        nResult = static_cast<int>(SdGetCoreAttr(va_arg(arguments, unsigned)));
        break;
    case rSdSetEffectAttr: {
        const int nCore = va_arg(arguments, int);
        SdSetEffectAttr(nCore & 1, *va_arg(arguments, const EffectAttr *));
        break;
    }
    default:
        // ponytail: the block transfers of the movie player's PCM (0x80d0, 0x80e0, 0x8100) are
        // not served; they would feed Play!'s core input reader (CSpuBase's block reader).
        break;
    }
    va_end(arguments);
    return nResult;
}

} // extern "C"
