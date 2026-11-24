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
void HostUseSystemHeap(int bOn);  // host/core/heap.c
int sce_midi_loop(void);          // src/ezmidi/midi_com.c
void HostHsynTick(void);          // src/ezmidi/midi_hsyn.c
void *HostHsynInput(void);        // src/ezmidi/midi_hsyn.c
long long HostFixedClockNs(void); // host/gfx-null/gfx.cpp: FREQ_CLOCK=fixed, or -1
}

namespace {

constexpr int kRate = 48000;
constexpr uint64_t kEeCyclesPerSample = 6144; // 294912 EE cycles per millisecond
constexpr uint64_t kIopCyclesPerSample = 768; // 36.864 MHz IOP system clock
constexpr int kBlock = 480;                   // frames per rendered block, 10 ms
constexpr int64_t kBlockNs = 10000000;
constexpr uint64_t kLeadCycles = 25 * 294912;        // 25 ms, a frame and a scheduler poll
constexpr int64_t kMaxLagNs = 200000000;             // a longer stall is waited out, not caught up
constexpr unsigned kMaxQueuedBytes = 4 * kRate / 10; // 100 ms of S16 stereo (times g_nChannels / 2)
int g_nChannels = 2; // 6 with FREQ_SURROUND=1 on a device that takes 5.1
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

// The movie player's PCM: sceSdBlockTrans() into core 1's sound data input, from a looping IOP
// ring of 1 KiB blocks, each 256 left then 256 right 16-bit samples at 48 kHz. It starts at the
// EE time the call came, as MIDI does. Positions count stream bytes from the start address: due by
// the clock, played, and written by the sample (the ring is full when it starts the transfer).
// On the console the sample always refills in time; a host that starves it for longer than the
// ring (128 ms) would loop stale blocks and play the rest of the movie a ring late, so the input
// plays silence rather than pass the writer, and skips ahead to the clock when data comes back.
struct BlockTrans {
    const uint8_t *pRing = nullptr;
    uint32_t nSize = 0; // 0: stopped
    uint32_t nFrom = 0;
    uint64_t nStart = 0;
    uint64_t nDue = 0;
    uint64_t nPlayed = 0;
    uint64_t nWritten = 0;
};
BlockTrans g_block;
// sceSdBlockTransStatus()'s answer, kept up to date by whoever moves g_block, so the movie player
// can poll it without waiting for the renderer's lock.
std::atomic<int> g_nBlockStatus{0};

// The IOP address the input's DMA fetches next. It has fetched the block playing, so it reports the
// next one: the sample refills up to 1 KiB short of this, never over the block that plays. A
// starved input has played all the sample wrote and reports where it waits, which the sample reads
// as empty.
void UpdateBlockStatus() {
    const uint64_t nBlock = g_block.nPlayed & ~uint64_t{0x3ff};
    const uint64_t nNext = g_block.nWritten > nBlock ? nBlock + 0x400 : nBlock;
    g_nBlockStatus.store(g_block.nSize != 0 ?
                             static_cast<int>(reinterpret_cast<uintptr_t>(g_block.pRing) +
                                              (g_block.nFrom + nNext) % g_block.nSize) :
                             0,
                         std::memory_order_relaxed);
}

SDL_AudioDeviceID g_nDevice = 0;

// The PS5 in 5.1: SDL's PS5 audio is stereo only, so the 5.1 frames go to the console's own audio
// port in its 8-channel format through a ring and a thread of their own (the two extra channels
// silent). Everything else queues on the SDL device as before.
bool g_bDirect = false;
std::mutex g_ringLock;
std::deque<int16_t> g_ring; // 6 samples a frame
#ifdef __PROSPERO__
extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int nUser, int nType, int nIndex, unsigned nLength, unsigned nRate, unsigned nFormat);
int sceAudioOutOutput(int nHandle, const void *pSamples);
}
constexpr unsigned kGrain = 256; // frames a call

void *RunSurroundOut(void *pHandle) {
    const int nHandle = static_cast<int>(reinterpret_cast<intptr_t>(pHandle));
    static int16_t s_aEight[8 * kGrain];
    for (;;) {
        memset(s_aEight, 0, sizeof(s_aEight));
        {
            std::lock_guard<std::mutex> lock(g_ringLock);
            const size_t nFrames = std::min<size_t>(kGrain, g_ring.size() / 6);
            for (size_t i = 0; i < nFrames; ++i) {
                for (int c = 0; c < 6; ++c) {
                    s_aEight[8 * i + c] = g_ring.front();
                    g_ring.pop_front();
                }
            }
        }
        // Waits for the port's previous grain; a port that fails returns at once, so the thread
        // sleeps a grain instead of spinning (and keeps draining the ring the game paces on).
        if (sceAudioOutOutput(nHandle, s_aEight) < 0) {
            usleep(kGrain * 1000000 / kRate);
        }
    }
    return nullptr;
}

bool OpenSurroundPort() {
    sceAudioOutInit();
    // The system user, the main port, 256-frame grains at 48 kHz, S16 8 channels (format 2).
    const int nHandle = sceAudioOutOpen(0xff, 0, 0, kGrain, kRate, 2);
    pthread_t thread;
    if (nHandle < 0 ||
        pthread_create(&thread, nullptr, RunSurroundOut, reinterpret_cast<void *>(static_cast<intptr_t>(nHandle))) != 0) {
        fprintf(stderr, "AUDIO_SURROUND_FAILED 0x%08x\n", static_cast<unsigned>(nHandle));
        return false;
    }
    pthread_detach(thread);
    return true;
}
#endif

Uint32 QueuedBytes() {
    if (g_bDirect) {
        std::lock_guard<std::mutex> lock(g_ringLock);
        return static_cast<Uint32>(g_ring.size() * sizeof(int16_t));
    }
    return SDL_GetQueuedAudioSize(g_nDevice);
}

void QueueOut(const int16_t *pSamples, Uint32 nBytes) {
    if (g_bDirect) {
        std::lock_guard<std::mutex> lock(g_ringLock);
        g_ring.insert(g_ring.end(), pSamples, pSamples + nBytes / sizeof(int16_t));
        return;
    }
    SDL_QueueAudio(g_nDevice, pSamples, nBytes);
}
FILE *g_pWav = nullptr;
uint64_t g_nWavFrames = 0;

// ---- Practice speed -----------------------------------------------------------------------------

// Game time per real time. Below 1 (practice, FREQ_PRACTICE_SPEED) the whole game runs on a slower
// clock: the renderer still makes the mix in game time, so every sample slice plays at its pitch
// and in time with the slowed MIDI, and Stretch lengthens that mix to real time without changing
// its pitch. 1 leaves the output exactly as without practice.
std::atomic<float> g_flSpeed{1.0f};

// WSOLA: frames of kFrame input samples, Hann-windowed and overlap-added kHop apart in the output.
// Frame k comes from within kSeek of input k * kHop * speed, where it best continues frame k - 1, so
// the output is input / speed long, give or take kSeek: the mix keeps the song clock's time. Its
// look-ahead delays the sound by kFrame + kSeek input samples (53 ms of game time); shorter frames
// smear the bass (on the attract demo, 2.0 dB rms log-spectrum error below 250 Hz at 1024, 0.8 at
// 2048).
// FREQ_TEST_PRACTICE_STRETCH=resample, a test control, plays the mix slowed instead (pitch drops).
class Stretch {
public:
    static constexpr int kFrame = 2048;
    static constexpr int kHop = kFrame / 2;
    static constexpr int kSeek = 512;

    void Reset() {
        m_in.clear();
        m_nBase = 0;
        m_nEnd = 0;
        m_flNominal = 0;
        m_nPrevious = -1;
        m_tail.assign(2 * kHop, 0.0f);
    }

    void Push(const int16_t *pIn, int nFrames, float flSpeed, std::vector<int16_t> &out) {
        m_in.insert(m_in.end(), pIn, pIn + 2 * nFrames);
        m_nEnd += nFrames;
        static const bool bResample = getenv("FREQ_TEST_PRACTICE_STRETCH") != nullptr;
        if (bResample) {
            for (int64_t i; (i = static_cast<int64_t>(m_flNominal)) + 1 < m_nEnd;
                 m_flNominal += flSpeed) {
                const float t = static_cast<float>(m_flNominal - i);
                for (int c = 0; c < 2; ++c) {
                    out.push_back(Clip(At(i, c) + t * (At(i + 1, c) - At(i, c))));
                }
            }
            Discard(static_cast<int64_t>(m_flNominal));
            return;
        }
        static const std::vector<float> s_window = [] {
            std::vector<float> window(kFrame);
            for (int j = 0; j < kFrame; ++j) {
                window[j] = 0.5f - 0.5f * cosf(6.2831853f * j / kFrame);
            }
            return window;
        }();
        for (int64_t a; (a = llround(m_flNominal)) + kSeek + kFrame <= m_nEnd;) {
            const int64_t x = m_nPrevious < 0 ? a : Seek(a, m_nPrevious + kHop);
            for (int j = 0; j < kHop; ++j) {
                for (int c = 0; c < 2; ++c) {
                    out.push_back(Clip(m_tail[2 * j + c] + s_window[j] * At(x + j, c)));
                    m_tail[2 * j + c] = s_window[kHop + j] * At(x + kHop + j, c);
                }
            }
            m_nPrevious = x;
            m_flNominal += kHop * static_cast<double>(flSpeed);
            Discard(std::min<int64_t>(llround(m_flNominal) - kSeek, x + kHop));
        }
    }

    // The input frame (from Reset()) of the newest output sample, or -1 before the first.
    int64_t Content() const {
        return m_nPrevious < 0 ? -1 : m_nPrevious + kHop - 1;
    }

private:
    float At(int64_t nFrame, int nChannel) const {
        return m_in[2 * (nFrame - m_nBase) + nChannel];
    }

    static int16_t Clip(float flValue) {
        return static_cast<int16_t>(std::clamp(lrintf(flValue), -32768L, 32767L));
    }

    // The start within kSeek of nNominal whose first half frame best matches the one at
    // nNatural, the continuation of the previous frame: normalised correlation of L + R, on every
    // fourth offset and second sample, then the three offsets around the best.
    int64_t Seek(int64_t nNominal, int64_t nNatural) const {
        const auto score = [&](int64_t x) {
            float flDot = 0, flEnergy = 1e-3f;
            for (int j = 0; j < kHop; j += 2) {
                const float v = At(x + j, 0) + At(x + j, 1);
                flDot += v * (At(nNatural + j, 0) + At(nNatural + j, 1));
                flEnergy += v * v;
            }
            return flDot / sqrtf(flEnergy);
        };
        const int64_t nLow = std::max(nNominal - kSeek, m_nBase);
        int64_t nBest = nNominal;
        float flBest = -1e30f;
        for (int64_t x = nLow; x <= nNominal + kSeek; x += 4) {
            if (const float s = score(x); s > flBest) {
                flBest = s;
                nBest = x;
            }
        }
        const int64_t nCoarse = nBest;
        for (int64_t x = std::max(nCoarse - 3, nLow); x <= std::min(nCoarse + 3, nNominal + kSeek);
             ++x) {
            if (const float s = score(x); s > flBest) {
                flBest = s;
                nBest = x;
            }
        }
        return nBest;
    }

    void Discard(int64_t nKeep) {
        if (nKeep - m_nBase > 4096) {
            m_in.erase(m_in.begin(), m_in.begin() + 2 * (nKeep - m_nBase));
            m_nBase = nKeep;
        }
    }

    std::vector<float> m_in; // stereo input from frame m_nBase to m_nEnd
    int64_t m_nBase = 0;
    int64_t m_nEnd = 0;
    double m_flNominal = 0; // where the next frame belongs in the input
    int64_t m_nPrevious = -1;
    std::vector<float> m_tail; // the second half of the last frame, still to be overlapped
};
Stretch g_stretch;
bool g_bStretching = false;
uint64_t g_nStretchBase = 0; // the rendered frame the stretch's input starts at

int64_t NowNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

// FREQ_AUDIO_OFFSET_MS: everything heard plays this much later against the game (negative:
// earlier). A positive offset stamps each sound that much after its due time. A negative one
// cannot play a sound before the game sends it: the picture and the judging (HostSongOffsetNs())
// wait that much instead, which puts the sound as much earlier against them. (Stamping earlier with
// a longer render lead, as before, moved the sound in the WAV but not against the picture: the
// lead kept the clock that much further ahead of the render.)
const int64_t g_nOffsetCycles = static_cast<int64_t>(
    atof(getenv("FREQ_AUDIO_OFFSET_MS") != nullptr ? getenv("FREQ_AUDIO_OFFSET_MS") : "0") * 294912);
const uint64_t g_nLeadCycles = kLeadCycles;
const int64_t g_nLateCycles = std::max<int64_t>(g_nOffsetCycles, 0);
const long long g_nEarlyNs = llround(std::max<int64_t>(-g_nOffsetCycles, 0) * 1e6 / 294912);

uint64_t Stamp(uint64_t nDue) {
    // The offset is wall time: at a practice speed s it spans s of it in song time.
    const int64_t nOffset = llround(g_nLateCycles * static_cast<double>(g_flSpeed.load()));
    return static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(nDue) + nOffset, 0));
}

// How much later than at normal speed the sound plays against the clock while a practice speed s
// stretches it, in samples of song time. The picture shows the song and live presses are judged as
// much later (HostSongOffsetNs()), so they keep with the sound as at normal speed. The terms:
//  - the stretch's look-ahead, from the newest input it holds to the newest sample it outputs;
//  - half a block: a frame is output on the block that brings its look-ahead in;
//  - less (1 - s) kHop / 2: a frame's kHop samples cover kHop s of song, its average is earlier;
//  - the render lead, g_nLeadCycles of song time, lasts 1 / s longer;
//  - s kHop / 2: frames reach the device queue kHop at a time, and the queue, held at
//    kMaxQueuedBytes, holds half a frame more (on the SDL dummy driver, 10 ms; a device that
//    keeps its queue short sees less).
// At 0.75 it is 2332 samples, 48.6 ms of song (64.8 ms of wall time). FREQ_TEST_PRACTICE_NO_COMP=1,
// a test control: 0.
double HostPracticeVideoLagSamples() {
    static const bool s_bOff = getenv("FREQ_TEST_PRACTICE_NO_COMP") != nullptr;
    const double s = g_flSpeed.load();
    if (s == 1.0 || s_bOff) {
        return 0;
    }
    return Stretch::kFrame + Stretch::kSeek - Stretch::kHop + kBlock / 2.0 -
           (1 - s) * Stretch::kHop / 2 + (1 - s) * g_nLeadCycles / kEeCyclesPerSample +
           s * Stretch::kHop / 2;
}

uint64_t EeClock() {
    // FREQ_CLOCK=fixed: the frame loop's clock, which HostAudioFixedTick() renders up to.
    if (const long long nFixedNs = HostFixedClockNs(); nFixedNs >= 0) {
        return static_cast<uint64_t>(nFixedNs) * 294912 / 1000000;
    }
    if (!g_bStarted) {
        return 0;
    }
    uint64_t nCycles;
    int64_t nAnchorNs;
    do {
        nCycles = g_nAnchorCycles.load();
        nAnchorNs = g_nAnchorNs.load();
    } while (nCycles != g_nAnchorCycles.load());
    // The EE moves through the block the renderer makes next, and no further, a lead ahead. A
    // practice speed below 1 makes that block last longer (RunAudio()).
    int64_t nSince = std::clamp<int64_t>(NowNs() - nAnchorNs, 0, kBlockNs);
    if (const float flSpeed = g_flSpeed.load(); flSpeed != 1.0f) {
        nSince = std::clamp<int64_t>((NowNs() - nAnchorNs) * flSpeed, 0, kBlockNs);
    }
    const uint64_t nNow = nCycles + g_nLeadCycles + static_cast<uint64_t>(nSince) * 294912 / 1000000;
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

// sceSdInit(): both cores reset and enabled, their effect work areas ending at the top of sound
// memory as libsd leaves them (EEA 0x1dffff for core 0, 0x1fffff for core 1). Sound memory keeps
// its contents. The movie player's cold init comes after the game has placed the work areas.
void SdInit() {
    for (Iop::CSpuBase *pCore : {&g_pSpu->core0, &g_pSpu->core1}) {
        pCore->Reset();
        pCore->SetDestinationSamplingRate(kRate);
        // FREQ_TEST_NO_REVERB=1, a test control: the mix without the effect processors.
        if (getenv("FREQ_TEST_NO_REVERB") != nullptr) {
            pCore->SetReverbEnabled(false);
        }
    }
    memset(g_pSpu->shadow, 0, sizeof(g_pSpu->shadow));
    g_pSpu->cache.Clear();
    g_abEffectSet[0] = g_abEffectSet[1] = false;
    for (int nCore = 0; nCore < 2; ++nCore) {
        WriteRegister(CoreRegister(nCore, kAttrRegister), kAttrEnable);
        WriteRegister(CoreRegister(nCore, 0x33c), 0xe + nCore);
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
        // Core 1's sound data input, at its BVOL, straight to its output (MMIX SINL/SINR).
        for (int i = 0; i < nRun && g_block.nSize != 0; ++i) {
            if ((g_nRendered + i) * kEeCyclesPerSample < g_block.nStart) {
                continue;
            }
            BlockTrans &block = g_block;
            block.nPlayed = std::min(std::max(block.nPlayed, block.nDue), block.nWritten);
            block.nDue += 4;
            if (block.nPlayed >= block.nWritten) {
                continue;
            }
            const uint32_t nRead =
                static_cast<uint32_t>((block.nFrom + block.nPlayed) % block.nSize);
            block.nPlayed += 4;
            const uint8_t *pBlock = block.pRing + (nRead & ~0x3ffu);
            const unsigned nSample = (nRead & 0x3ffu) / 4;
            int16_t aInput[2];
            memcpy(&aInput[0], pBlock + 2 * nSample, 2);
            memcpy(&aInput[1], pBlock + 0x200 + 2 * nSample, 2);
            Iop::CSpuBase::MixSamples(aInput[0], g_pSpu->core1.m_inputVolL, &aCore1[2 * i]);
            Iop::CSpuBase::MixSamples(aInput[1], g_pSpu->core1.m_inputVolR, &aCore1[2 * i + 1]);
        }
        UpdateBlockStatus();
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

extern "C" long long HostSongOffsetNs(long long nWallNs);

void RenderBlock(int nFrames) {
    int16_t aBlock[2 * kBlock];
    {
        std::lock_guard<std::mutex> lock(g_iop);
        RenderLocked(aBlock, nFrames);
    }
    g_nAnchorNs.store(NowNs());
    g_nAnchorCycles.store(g_nRendered * kEeCyclesPerSample);
    const int16_t *pOut = aBlock;
    if (const float flSpeed = g_flSpeed.load(); flSpeed != 1.0f || g_bStretching) {
        static std::vector<int16_t> s_stretched;
        s_stretched.clear();
        if (!g_bStretching) {
            g_stretch.Reset();
            g_nStretchBase = g_nRendered - nFrames;
        }
        if (g_bStretching != (flSpeed != 1.0f)) { // where the mix starts or stops stretching
            fprintf(stderr,
                    "AUDIO_STRETCH speed=%.3f rendered=%llu wav=%llu\n",
                    flSpeed,
                    static_cast<unsigned long long>(g_nRendered - nFrames),
                    static_cast<unsigned long long>(g_nWavFrames));
        }
        g_bStretching = flSpeed != 1.0f;
        if (g_bStretching) {
            g_stretch.Push(aBlock, nFrames, flSpeed, s_stretched);
            pOut = s_stretched.data();
            nFrames = static_cast<int>(s_stretched.size() / 2);
        }
    }
    const size_t nBytes = sizeof(int16_t) * 2 * nFrames;
    if (g_nDevice != 0 && nFrames > 0) {
        // A device slower than the wall clock loses a block now and then, rather than lagging.
        if (QueuedBytes() < kMaxQueuedBytes * g_nChannels / 2) {
            if (g_nChannels == 6) {
                // FREQ_SURROUND=1: a matrix upmix of the game's stereo, SDL's 5.1 order (front
                // left, right, centre, LFE, surround left, right). The centre carries what both
                // sides share, the surrounds what differs; the fronts stay as mixed.
                static std::vector<int16_t> s_six;
                s_six.resize(6 * static_cast<size_t>(nFrames));
                for (int i = 0; i < nFrames; ++i) {
                    const int nLeft = pOut[2 * i], nRight = pOut[2 * i + 1];
                    int16_t *pSix = &s_six[6 * static_cast<size_t>(i)];
                    pSix[0] = static_cast<int16_t>(nLeft);
                    pSix[1] = static_cast<int16_t>(nRight);
                    pSix[2] = static_cast<int16_t>((nLeft + nRight) * 181 / 512); // 0.707 of the mean
                    pSix[3] = 0;
                    pSix[4] = static_cast<int16_t>(std::clamp((nLeft - nRight) / 2, -32768, 32767));
                    pSix[5] = static_cast<int16_t>(std::clamp((nRight - nLeft) / 2, -32768, 32767));
                }
                QueueOut(s_six.data(), static_cast<Uint32>(nBytes * 3));
            } else {
                QueueOut(pOut, static_cast<Uint32>(nBytes));
            }
        }
    }
    // FREQ_TEST_AUDIO_LAG=1, a test measure: how far the song time on screen is ahead of the sound
    // heard, in ms, when the newest sample plays (after the device's queue); the median each second.
    // The screen shows the clock less the video offset (HostSongOffsetNs(), which holds the
    // practice lag); the sound in a rendered sample is the song time its stamp offset moved there.
    static const bool s_bLag = getenv("FREQ_TEST_AUDIO_LAG") != nullptr;
    static const long long s_nVideoNs = llround(
        atof(getenv("FREQ_VIDEO_OFFSET_MS") != nullptr ? getenv("FREQ_VIDEO_OFFSET_MS") : "0") * 1e6);
    if (s_bLag && g_nDevice != 0) {
        static std::vector<float> s_lags;
        const float flSpeed = g_bStretching ? g_flSpeed.load() : 1.0f;
        const int64_t nContent =
            (g_bStretching ? g_nStretchBase + g_stretch.Content() :
                             static_cast<int64_t>(g_nRendered) - 1) -
            llround(g_nLateCycles * static_cast<double>(g_flSpeed.load())) /
                static_cast<int64_t>(kEeCyclesPerSample);
        const double flQueued = QueuedBytes() / (2.0 * g_nChannels);
        // A stretch frame outputs kHop samples at once, each later against the clock than the
        // next: the frame's average sample is (1 - speed) kHop / 2 later than its newest.
        const double flShown = static_cast<double>(EeClock()) / kEeCyclesPerSample +
                               flSpeed * flQueued - HostSongOffsetNs(s_nVideoNs) * kRate / 1e9 +
                               (g_bStretching ? (1 - flSpeed) * Stretch::kHop / 2 : 0);
        s_lags.push_back(static_cast<float>((flShown - nContent) / (kRate / 1000.0) / flSpeed));
        if (s_lags.size() == 100) {
            std::nth_element(s_lags.begin(), s_lags.begin() + 50, s_lags.end());
            fprintf(stderr, "AUDIO_LAG speed=%.2f ms=%.1f queued=%.1f\n", flSpeed, s_lags[50],
                    flQueued / (kRate / 1000.0));
            s_lags.clear();
        }
    }
    if (g_pWav != nullptr) {
        fwrite(pOut, nBytes, 1, g_pWav);
        const bool bSecond = (g_nWavFrames + nFrames) / kRate != g_nWavFrames / kRate;
        g_nWavFrames += nFrames;
        if (bSecond) {
            WriteWavHeader();
        }
    }
}

void *RunAudio(void *) {
    HostUseSystemHeap(1);
    // The clock of the whole game runs here; ahead of the game's threads where that is allowed.
#ifndef __PROSPERO__ // ponytail: the console's thread priority is left as it is until a run shows the need
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), -15);
#endif
    int64_t nDeadline = NowNs();
    for (;;) {
        RenderBlock(kBlock);
        nDeadline += static_cast<int64_t>(kBlockNs / g_flSpeed.load());
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
    const char *pszSurround = getenv("FREQ_SURROUND");
    want.channels = pszSurround != nullptr && strcmp(pszSurround, "1") == 0 ? 6 : 2;
#ifdef __PROSPERO__
    if (want.channels == 6 && OpenSurroundPort()) {
        g_bDirect = true;
        g_nChannels = 6;
        g_nDevice = 1; // open: the queue goes to the ring
        const std::vector<int16_t> silence(2 * 6 * kBlock);
        QueueOut(silence.data(), static_cast<Uint32>(silence.size() * sizeof(int16_t)));
        fprintf(stderr, "AUDIO_OPEN ps5 audio port %d Hz 6 channels\n", kRate);
        return;
    }
#endif
    want.samples = kBlock;
    SDL_AudioSpec have;
    // Only the channel count may differ: a device without 5.1 opens in stereo, as before.
    g_nDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
    g_nChannels = have.channels == 6 ? 6 : 2;
    if (g_nDevice != 0 && have.channels != 6 && have.channels != 2) {
        SDL_CloseAudioDevice(g_nDevice); // neither: stereo, converted by SDL
        want.channels = 2;
        g_nDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        g_nChannels = 2;
    }
    if (g_nDevice == 0) {
        fprintf(stderr, "AUDIO_NO_DEVICE %s\n", SDL_GetError());
        return;
    }
    // Two blocks of silence ahead of the first, so the device never waits on the renderer.
    const std::vector<int16_t> silence(2 * g_nChannels * kBlock);
    SDL_QueueAudio(g_nDevice, silence.data(), silence.size() * sizeof(int16_t));
    SDL_PauseAudioDevice(g_nDevice, 0);
    fprintf(stderr, "AUDIO_OPEN %s %d Hz %d channels\n", SDL_GetCurrentAudioDriver(), have.freq, g_nChannels);
}

} // namespace

// ---- What the rest of the host calls ------------------------------------------------------------

extern "C" {

// Practice mode (host/frequency.ini.example): FREQ_PRACTICE=1, or either key below, makes every
// song a practice run. FREQ_PRACTICE_SPEED is the song's speed, 0.5 to 1 (or 50 to 100 percent).
// FREQ_PRACTICE_LOOP repeats a section until the player quits or restarts: "9-16" for bars 9 to
// 16, or "p3" / "p3-4" for phrases (the song's sections, the HUD's song-position blocks), each
// counted from 1. FREQ_TEST_PRACTICE_DEMO=1, a test control, also lets the attract demo practise.
namespace {
struct PracticeSettings {
    bool bOn = false;
    float flSpeed = 1.0f;
    int nLoopKind = 0; // 0 none, 1 bars, 2 phrases
    int nLoopFirst = 0;
    int nLoopEnd = 0; // one past the last, from 0
    PracticeSettings() {
        const char *pszSpeed = getenv("FREQ_PRACTICE_SPEED");
        const char *pszLoop = getenv("FREQ_PRACTICE_LOOP");
        const char *pszOn = getenv("FREQ_PRACTICE");
        bOn = (pszOn != nullptr && atoi(pszOn) != 0) || pszSpeed != nullptr || pszLoop != nullptr;
        if (pszSpeed != nullptr) {
            const float flValue = strtof(pszSpeed, nullptr);
            flSpeed = std::clamp(flValue > 1.0f ? flValue / 100.0f : flValue, 0.5f, 1.0f);
        }
        int nFirst = 0, nLast = 0;
        if (pszLoop != nullptr) {
            const bool bPhrases = *pszLoop == 'p' || *pszLoop == 'P';
            const int nRead = sscanf(pszLoop + bPhrases, "%d-%*[pP]%d", &nFirst, &nLast) == 2 ?
                                  2 :
                                  sscanf(pszLoop + bPhrases, "%d-%d", &nFirst, &nLast);
            if (nRead == 1) {
                nLast = nFirst;
            }
            if (nRead >= 1 && nFirst >= 1 && nLast >= nFirst) {
                nLoopKind = bPhrases ? 2 : 1;
                nLoopFirst = nFirst - 1;
                nLoopEnd = nLast;
            } else {
                fprintf(stderr, "host: FREQ_PRACTICE_LOOP=%s is not understood\n", pszLoop);
            }
        }
    }
};
const PracticeSettings &Practice() {
    static const PracticeSettings s_settings;
    return s_settings;
}
bool g_bPracticeArmed = false;
struct {
    int nKind, nFirst, nEnd;
} g_practiceLoop; // the armed song's loop: Practice()'s, until its phrases are bars
} // namespace

// GrooveWorld::PrepareLevel(): whether the song about to play is a practice run. The attract demo
// (a playback) is not, without FREQ_TEST_PRACTICE_DEMO.
int HostPracticeArm(int bPlayback) {
    g_bPracticeArmed =
        Practice().bOn && (bPlayback == 0 || getenv("FREQ_TEST_PRACTICE_DEMO") != nullptr);
    g_practiceLoop = {Practice().nLoopKind, Practice().nLoopFirst, Practice().nLoopEnd};
    if (g_bPracticeArmed) {
        fprintf(stderr,
                "PRACTICE_ARMED speed=%.2f loop=%d:%d-%d\n",
                Practice().flSpeed,
                Practice().nLoopKind,
                Practice().nLoopFirst,
                Practice().nLoopEnd);
    }
    return g_bPracticeArmed;
}

// GrooveWorld::StartPlay(): the song clock at the practice speed from now; bOn 0, normal speed.
void HostPracticeSpeed(int bOn) {
    g_flSpeed.store(bOn != 0 ? Practice().flSpeed : 1.0f);
}

// The song has ended: normal speed, and the next song is not practice until armed.
void HostPracticeEnd() {
    HostPracticeSpeed(0);
    g_bPracticeArmed = false;
}

int HostPracticeArmed() {
    return g_bPracticeArmed;
}

// The practice loop of the armed song: 1 for bars, 2 for phrases, [*pFirst, *pEnd) from 0; or 0.
int HostPracticeLoop(int *pFirst, int *pEnd) {
    if (!g_bPracticeArmed || g_practiceLoop.nKind == 0) {
        return 0;
    }
    *pFirst = g_practiceLoop.nFirst;
    *pEnd = g_practiceLoop.nEnd;
    return g_practiceLoop.nKind;
}

// The first bar a practice loop repeats, whose phrases come back uncaught (PhraseMgr), or INT_MAX.
// FREQ_TEST_PRACTICE_KEEP_PHRASES=1, a test control, keeps them, so a repeat sounds as the last pass.
int HostPracticeRepeatsFrom() {
    static const bool bKeep = getenv("FREQ_TEST_PRACTICE_KEEP_PHRASES") != nullptr;
    return g_bPracticeArmed && g_practiceLoop.nKind == 1 && !bKeep ? g_practiceLoop.nEnd : INT_MAX;
}

// The bars a loop of phrases spans, which PlayMapLinear works out from the song: a bar loop now.
void HostPracticeLoopBars(int nFirst, int nEnd) {
    fprintf(stderr, "PRACTICE_LOOP bars %d-%d\n", nFirst + 1, nEnd);
    g_practiceLoop = {1, nFirst, nEnd};
}

// Renderer (FREQ_VIDEO_OFFSET_MS) and Catcher (FREQ_INPUT_OFFSET_MS): the song time, in ns, that a
// latency offset of nWallNs wall time spans now (at a practice speed s, s of it), with a negative
// FREQ_AUDIO_OFFSET_MS's size added (the picture and judging wait for it), plus how much later a
// practice speed's stretch plays the sound (HostPracticeVideoLagSamples()). At normal speed and no
// negative audio offset, nWallNs itself.
long long HostSongOffsetNs(long long nWallNs) {
    return llround((nWallNs + g_nEarlyNs) * static_cast<double>(g_flSpeed.load()) +
                   HostPracticeVideoLagSamples() * 1e9 / kRate);
}

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
    g_midiDue.push_back({nOffset, Stamp(nNow - std::min(nLate, nNow))});
}

// A SIF DMA write into IOP memory, under the IOP lock. MIDI for the synth waits for its due time:
// the shipped stream (sceCslMidiStream) is split into messages, each stamped with the due time
// HostMidiQueued() recorded for its offset, or now.
void HostIopWrite(void *pDest, const void *pSource, unsigned nSize) {
    uint8_t *pInput = static_cast<uint8_t *>(HostHsynInput());
    uint8_t *pDestBytes = static_cast<uint8_t *>(pDest);
    if (g_block.nSize != 0 && pDestBytes >= g_block.pRing &&
        pDestBytes < g_block.pRing + g_block.nSize) {
        g_block.nWritten += nSize; // the movie's PCM, written in stream order
        UpdateBlockStatus();
    }
    if (!g_bTicking || pDestBytes < pInput ||
        pDestBytes >= pInput + kMidiBuffers * kMidiBufferSize) {
        memcpy(pDest, pSource, nSize);
        return;
    }
    const uint8_t *pData = static_cast<const uint8_t *>(pSource) + 2 * sizeof(uint32_t);
    const unsigned nData = nSize > 2 * sizeof(uint32_t) ? nSize - 2 * sizeof(uint32_t) : 0;
    const uint64_t nNow = EeClock();
    size_t nDue = 0;
    uint64_t nStamp = Stamp(nNow);
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

// FREQ_CLOCK=fixed: called by the blank thread at each tick of the fixed clock, while the game
// waits for it. Renders up to the clock, less the lead, so a run is the same at any host speed.
void HostAudioFixedTick() {
    if (!g_bStarted) {
        return;
    }
    const uint64_t nTarget = EeClock() > g_nLeadCycles ? EeClock() - g_nLeadCycles : 0;
    HostUseSystemHeap(1);
    while (g_nRendered * kEeCyclesPerSample < nTarget) {
        const uint64_t nFrames =
            (nTarget - g_nRendered * kEeCyclesPerSample + kEeCyclesPerSample - 1) /
            kEeCyclesPerSample;
        RenderBlock(static_cast<int>(std::min<uint64_t>(nFrames, kBlock)));
    }
    HostUseSystemHeap(0);
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
    // FREQ_CLOCK=fixed renders at the frame loop's ticks (HostAudioFixedTick()), to the file only.
    const bool bFixed = HostFixedClockNs() >= 0;
    if (!bFixed) {
        OpenDevice();
    }
    HostUseSystemHeap(0);
    g_nAnchorNs.store(NowNs());
    g_bStarted = true;
    if (bFixed) {
        return;
    }
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

// libsdr's commands for sceSdBlockTrans() and sceSdBlockTransStatus(), and its transfer modes.
constexpr int kSdBlockTrans = 0x80e0;
constexpr int kSdBlockTransStatus = 0x8100;
constexpr int kSdTransStop = 2;
constexpr int kSdTransWriteFrom = 3;

int sceSdRemote(int nWait, ...) {
    va_list arguments;
    va_start(arguments, nWait);
    const int nCommand = va_arg(arguments, int);
    if (nCommand == kSdBlockTransStatus) { // polled all through the movie: no lock
        va_end(arguments);
        return g_nBlockStatus.load(std::memory_order_relaxed);
    }
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
    case kSdBlockTrans: { // (core, mode, IOP ring, size, start address); only core 1's input
        va_arg(arguments, int);
        const int nMode = va_arg(arguments, int);
        const uintptr_t nRing = va_arg(arguments, unsigned);
        const uint32_t nSize = va_arg(arguments, unsigned) & ~0x3ffu;
        const uintptr_t nFrom = va_arg(arguments, unsigned);
        g_block = {};
        if ((nMode & 3) != kSdTransStop && nSize != 0) {
            g_block.pRing = reinterpret_cast<const uint8_t *>(nRing);
            g_block.nSize = nSize;
            g_block.nFrom =
                (nMode & 3) == kSdTransWriteFrom ? static_cast<uint32_t>(nFrom - nRing) : 0;
            g_block.nStart = Stamp(EeClock());
            g_block.nWritten = nSize;
        }
        UpdateBlockStatus();
        break;
    }
    default:
        // ponytail: sceSdVoiceTrans (0x80d0), which the movie player sends a block of zeros with
        // as it stops, is not served.
        break;
    }
    va_end(arguments);
    return nResult;
}

} // extern "C"
