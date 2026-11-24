// The OpenGL graphics backend. libps2gfx runs the VIF1 and GIF transfers through its VU1 and GS,
// and draws each blank's frame in an SDL2 window.
//
// FREQ_GFX=null draws nothing, as the null backend does. Without FREQ_GFX, a missing display also
// leads to the null backend. FREQ_GFX=vulkan draws with libps2gfx's Vulkan GS in an SDL2 Vulkan
// window instead (a build with -DPS2GFX_VULKAN=ON); the PS5 always does, on the TV, and there
// FREQ_SCALE defaults to the smallest scale whose lines cover the TV's (5 on a 4K set).
//
// FREQ_SHOT=<directory> and FREQ_SHOT_AT=<BREADCRUMB>:<blank>[,<blank>...] save the displayed frame
// as <directory>/<BREADCRUMB>+<blank>.png, where blank 0 is the first blank after the breadcrumb.
// Blanks are fields (59.94 Hz) at any FREQ_FPS. Groups for more breadcrumbs follow after ';'.
//
// FREQ_FRAMEHASH=<BREADCRUMB>:<fields> logs "FRAMEHASH <tick> <ns> <hash>" for each frame presented
// in that many fields from the breadcrumb: a hash of the displayed frame's pixels, and the fixed
// clock's time (FREQ_CLOCK=fixed presents each tick's frame before the game goes on).
//
// FREQ_SCALE=<n> draws the GS at n times its resolution, FREQ_OUTPUT=<width>x<height> sizes the
// window, and FREQ_WIDESCREEN=1 presents 16:9. With any of them, the picture keeps a TV's 4:3 (or
// 16:9) inside the window, the window defaults to 448n pixels high, and a shot is the whole window
// (with Vulkan the swapchain image, and the GS framebuffer beside it as <shot>.gs.png).
//
// FREQ_TEX_DUMP=<directory> and FREQ_TEX_REPLACE=<directory> dump the textures the GS draws and draw
// HD replacements in their place (host/gfx/ps2gfx/src/TexReplace.h).

#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <SDL.h>
#ifndef __PROSPERO__
#include <SDL_vulkan.h>
#endif

#include "../core/lowmem.h"
#include "../gfx-null/gfxnull.h"
#include "ps2gfx.h"

extern "C" void HostUseSystemHeap(int bOn); // host/core/heap.c
#ifdef __PROSPERO__
extern "C" int sceSystemServiceHideSplashScreen(void);
#endif
void RunLongOperationPollProc();            // src/app/longop.cpp: MainLoop::PumpTimers()

namespace {

// The libraries allocate from the C library's heap, as the low heap is the game's.
struct SystemHeap {
    SystemHeap() {
        HostUseSystemHeap(1);
    }
    ~SystemHeap() {
        HostUseSystemHeap(0);
    }
};

constexpr int kScreenWidth = 640;
constexpr int kScreenHeight = 448;
constexpr int kVif1Channel = 1;
constexpr int kGifChannel = 2;
// How often a game held at its swap pumps its timers, as its drawing would.
constexpr long kPumpNs = 2000000;

// The display registers the game writes to the privileged page each frame (PMODE, SMODE2, DISPFB1,
// DISPLAY1, DISPFB2, DISPLAY2, and BGCOLOR). The GS reads them at the blank.
constexpr uint32_t kDisplayRegisters[] = {
    0x12000000, 0x12000020, 0x12000070, 0x12000080, 0x12000090, 0x120000a0, 0x120000e0};

// ps2gfx takes one call at a time. The game's threads send transfers and the present thread
// presents. A present first waits for the GS thread to draw the frame before, outside the lock
// (ps2gfx_wait_frames()), so the lock is never held across a draw and a transfer waits at most
// for the present's own short work (display registers, flush, flip queued).
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// The blank thread only records the blank for the present thread, so a slow present never delays
// the blanks, and the blank count stays with the clock.
pthread_mutex_t g_presentLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_presentSignal = PTHREAD_COND_INITIALIZER;
unsigned g_nPendingVblank = 0;
// Under g_presentLock: the tick of the last frame presented, signalled to HostGfxWaitPresented().
unsigned g_nPresentedTick = 0;
pthread_cond_t g_presentedSignal = PTHREAD_COND_INITIALIZER;
pthread_once_t g_startOnce = PTHREAD_ONCE_INIT;
bool g_bDrawing = false;
SDL_Window *g_pWindow = nullptr;
SDL_GLContext g_context = nullptr;
bool g_bVulkan = false;

const char *g_pszShotDirectory = nullptr;
// Whether shots save the window rather than the GS framebuffer.
bool g_bOutputShots = false;
// The shots of one breadcrumb, and the blank count at that breadcrumb.
struct ShotGroup {
    std::string breadcrumb;
    std::vector<unsigned> offsets;
    bool bAnchored = false;
    unsigned nAnchor = 0;
    timespec anchorTime;
    unsigned nAnchorPresents = 0;
};
std::vector<ShotGroup> g_shotGroups;
// The blank of the next shot. The blank thread takes it itself, so the shot shows what the GS
// displays at that blank rather than at the present thread's later turn.
std::atomic<unsigned> g_nNextShotVblank{~0U};
// The frames presented, counted on the GS thread.
std::atomic<unsigned> g_nPresents{0};

std::string g_hashBreadcrumb;
unsigned g_nHashFields = 0;
bool g_bHashAnchored = false;
unsigned g_nHashAnchor = 0;

void HashFrame(unsigned nTick) {
    uint64_t nHash = 0;
    const int nResult = ps2gfx_frame_hash(&nHash);
    fprintf(stderr,
            "FRAMEHASH %u %lld %016llx%s\n",
            nTick,
            HostFixedClockNs(),
            static_cast<unsigned long long>(nHash),
            nResult == 0 ? "" : " FAILED");
}

void TakeShots(unsigned nVblank);

// Called with g_lock held, for the frame loop's tick nTick (see HostFrameTick()).
void PresentLocked(unsigned nTick) {
    for (uint32_t nRegister : kDisplayRegisters) {
        ps2gfx_priv_write(
            nRegister, *reinterpret_cast<volatile uint64_t *>(static_cast<uintptr_t>(nRegister)));
    }
    ps2gfx_vsync();
    pthread_mutex_lock(&g_presentLock);
    g_nPresentedTick = std::max(g_nPresentedTick, nTick);
    pthread_cond_broadcast(&g_presentedSignal);
    pthread_mutex_unlock(&g_presentLock);
    const unsigned nVblank = nTick / HostTicksPerField();
    if (g_bHashAnchored && nVblank - g_nHashAnchor < g_nHashFields) {
        HashFrame(nTick);
    }
    TakeShots(nVblank);
}

[[noreturn]] void Fail(const char *pszWhat) {
    fprintf(stderr, "host: %s: %s\n", pszWhat, SDL_GetError());
    _exit(70);
}

// The GS thread of ps2gfx runs both callbacks. It allocates from the C library's heap, and the
// OpenGL context is current there, from the first.
void MakeCurrent(void *) {
    HostUseSystemHeap(1);
    if (g_context != nullptr) {
        SDL_GL_MakeCurrent(g_pWindow, g_context);
    }
}

#ifdef __PROSPERO__
struct PresentStats {
    unsigned nSkipped;
    uint64_t nWaitNs, nWaitMaxNs, nPresentNs, nPresentMaxNs;
} g_presentStats;

uint64_t HostNowNs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000000000u + static_cast<uint64_t>(now.tv_nsec);
}
#endif

void Present(void *) {
    if (g_context != nullptr) {
        SDL_GL_SwapWindow(g_pWindow);
    }
    ++g_nPresents;
#ifdef __PROSPERO__
    // The system's splash screen stays up until the first picture (as Perfect Eye does), then the
    // rate the TV shows frames at, every 600.
    static timespec start;
    if (g_nPresents == 1) {
        sceSystemServiceHideSplashScreen();
        clock_gettime(CLOCK_MONOTONIC, &start);
        fprintf(stderr, "gfx: first frame on the TV, splash screen down\n");
    } else if (g_nPresents % 600 == 1) {
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double flSeconds = static_cast<double>(now.tv_sec - start.tv_sec) +
                                 static_cast<double>(now.tv_nsec - start.tv_nsec) / 1e9;
        // With the game's own fields over the same time: fewer frames than fields is frames the
        // presents dropped, fewer fields than the TV's 59.94 is the game's clock running slow.
        static unsigned s_nStartTick;
        const double flFields =
            static_cast<double>(g_nPresentedTick - s_nStartTick) / HostTicksPerField();
        s_nStartTick = g_nPresentedTick;
        fprintf(stderr, "gfx: 600 frames in %.2f s (%.2f a second), game %.1f fields (%.2f a second); "
                "%u ticks skipped, GS wait %.1f ms avg %.1f max, present %.1f avg %.1f max\n",
                flSeconds, 600 / flSeconds, flFields, flFields / flSeconds, g_presentStats.nSkipped,
                g_presentStats.nWaitNs / 600e6, g_presentStats.nWaitMaxNs / 1e6,
                g_presentStats.nPresentNs / 600e6, g_presentStats.nPresentMaxNs / 1e6);
        g_presentStats = PresentStats{};
        start = now;
    }
#else
    SDL_PumpEvents();
    if (SDL_PeepEvents(nullptr, 1, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) {
        _exit(0);
    }
#endif
}

#ifndef __PROSPERO__
// The Vulkan GS's surface: the SDL2 window's (ps2gfx_config.vk_surface, on the GS thread).
uint64_t CreateSurface(void *, void *pInstance) {
    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(g_pWindow, static_cast<VkInstance>(pInstance), &surface)) {
        Fail("cannot create the Vulkan surface");
    }
    return (uint64_t)surface;
}
#endif

void *RunPresents(void *) {
    HostUseSystemHeap(1);
    unsigned nPresented = 0;
    for (;;) {
        pthread_mutex_lock(&g_presentLock);
        while (g_nPendingVblank == nPresented) {
            pthread_cond_wait(&g_presentSignal, &g_presentLock);
        }
        pthread_mutex_unlock(&g_presentLock);

        // The GS draws the last frame without g_lock held, so the game's transfers go on. The
        // blank presented is the latest one after that wait.
#ifdef __PROSPERO__
        const uint64_t nWaitStart = HostNowNs();
#endif
        ps2gfx_wait_frames();
        pthread_mutex_lock(&g_presentLock);
        const unsigned nPrevious = nPresented;
        nPresented = g_nPendingVblank;
        pthread_mutex_unlock(&g_presentLock);
#ifdef __PROSPERO__
        const uint64_t nPresentStart = HostNowNs();
#endif
        pthread_mutex_lock(&g_lock);
        PresentLocked(nPresented);
        pthread_mutex_unlock(&g_lock);
#ifdef __PROSPERO__
        // Where a present's time goes, for the frame line: the GS wait, the present, and the
        // game ticks it skipped.
        const uint64_t nEnd = HostNowNs();
        g_presentStats.nSkipped += nPresented - nPrevious > 1 ? nPresented - nPrevious - 1 : 0;
        g_presentStats.nWaitMaxNs = std::max(g_presentStats.nWaitMaxNs, nPresentStart - nWaitStart);
        g_presentStats.nPresentMaxNs = std::max(g_presentStats.nPresentMaxNs, nEnd - nPresentStart);
        g_presentStats.nWaitNs += nPresentStart - nWaitStart;
        g_presentStats.nPresentNs += nEnd - nPresentStart;
#endif
    }
    return nullptr;
}

void ParseShots() {
    if (const char *pszHash = getenv("FREQ_FRAMEHASH")) {
        const char *pszColon = strchr(pszHash, ':');
        if (pszColon == nullptr) {
            fprintf(stderr, "host: FREQ_FRAMEHASH is not <BREADCRUMB>:<fields>\n");
            _exit(70);
        }
        g_hashBreadcrumb.assign(pszHash, pszColon);
        g_nHashFields = static_cast<unsigned>(strtoul(pszColon + 1, nullptr, 10));
    }
    g_pszShotDirectory = getenv("FREQ_SHOT");
    const char *pszAt = getenv("FREQ_SHOT_AT");
    if (g_pszShotDirectory == nullptr || pszAt == nullptr) {
        g_pszShotDirectory = nullptr;
        return;
    }
    for (const char *p = pszAt; *p != '\0';) {
        const char *pszColon = strchr(p, ':');
        if (pszColon == nullptr) {
            fprintf(stderr, "host: FREQ_SHOT_AT is not <BREADCRUMB>:<blank>[,<blank>...][;...]\n");
            _exit(70);
        }
        ShotGroup &group = g_shotGroups.emplace_back();
        group.breadcrumb.assign(p, pszColon);
        for (p = pszColon + 1; *p != '\0' && *p != ';';) {
            char *pszEnd;
            group.offsets.push_back(static_cast<unsigned>(strtoul(p, &pszEnd, 10)));
            p = (*pszEnd == ',') ? pszEnd + 1 : pszEnd + strcspn(pszEnd, ";");
        }
        p += *p == ';';
    }
}

void Start() {
    const char *pszMode = getenv("FREQ_GFX");
    if (pszMode != nullptr && strcmp(pszMode, "null") == 0) {
        return;
    }
    g_bVulkan = pszMode != nullptr && strcmp(pszMode, "vulkan") == 0;
    if (pszMode != nullptr && strcmp(pszMode, "ps2gfx") != 0 && !g_bVulkan) {
        fprintf(stderr, "host: FREQ_GFX=%s is not null, ps2gfx or vulkan\n", pszMode);
        _exit(70);
    }
    const SystemHeap systemHeap;
#ifndef __PROSPERO__
    // Without a session bus, D-Bus would try to start one for SDL2 and wait on it for ever.
    setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", 0);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        if (pszMode == nullptr) {
            fprintf(stderr, "host: no display (%s), so nothing is drawn\n", SDL_GetError());
            return;
        }
        Fail("cannot open the display");
    }
#endif
    const char *pszScale = getenv("FREQ_SCALE");
    const char *pszOutput = getenv("FREQ_OUTPUT");
    const int nScale = pszScale != nullptr ? std::max(atoi(pszScale), 1) : 1;
    const bool bWidescreen = HostWidescreen() != 0;
    g_bOutputShots = nScale > 1 || pszOutput != nullptr || bWidescreen;
#ifdef __PROSPERO__
    g_bVulkan = g_bOutputShots = true; // a TV, 4:3 or 16:9
#endif
    int nWindowHeight = kScreenHeight * nScale;
    int nWindowWidth = g_bOutputShots ?
                           nWindowHeight * (bWidescreen ? 16 : 4) / (bWidescreen ? 9 : 3) :
                           kScreenWidth;
    if (pszOutput != nullptr && (sscanf(pszOutput, "%dx%d", &nWindowWidth, &nWindowHeight) != 2 ||
                                 nWindowWidth <= 0 || nWindowHeight <= 0)) {
        fprintf(stderr, "host: FREQ_OUTPUT is not <width>x<height>\n");
        _exit(70);
    }

    ps2gfx_config config = {};
    config.make_current = MakeCurrent;
    config.present = Present;
#ifdef __PROSPERO__
    // The PS5 has no OpenGL: the Vulkan GS on the static RADV driver presents to the TV itself.
    config.renderer = PS2GFX_RENDERER_VULKAN;
#else
    static std::vector<const char *> extensions;
    if (g_bVulkan) {
        g_pWindow = SDL_CreateWindow("FreQuency",
                                     SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED,
                                     nWindowWidth,
                                     nWindowHeight,
                                     SDL_WINDOW_VULKAN);
        unsigned nExtensions = 0;
        if (g_pWindow == nullptr ||
            !SDL_Vulkan_GetInstanceExtensions(g_pWindow, &nExtensions, nullptr)) {
            Fail("cannot create the Vulkan window");
        }
        extensions.resize(nExtensions);
        SDL_Vulkan_GetInstanceExtensions(g_pWindow, &nExtensions, extensions.data());
        config.renderer = PS2GFX_RENDERER_VULKAN;
        config.vk_surface = CreateSurface;
        config.vk_extensions = extensions.data();
        config.vk_extension_count = nExtensions;
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
        g_pWindow = SDL_CreateWindow("FreQuency",
                                     SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED,
                                     nWindowWidth,
                                     nWindowHeight,
                                     SDL_WINDOW_OPENGL);
        if (g_pWindow == nullptr || (g_context = SDL_GL_CreateContext(g_pWindow)) == nullptr ||
            SDL_GL_MakeCurrent(g_pWindow, nullptr) != 0) {
            Fail("cannot create the OpenGL window");
        }

        config.renderer = PS2GFX_RENDERER_OPENGL;
    }
#endif
    config.spr_base = reinterpret_cast<void *>(kHostScratchpad);
    config.window_width = nWindowWidth;
    config.window_height = nWindowHeight;
    config.res_factor = nScale;
#ifdef __PROSPERO__
    if (pszScale == nullptr) {
        config.res_factor = -1; // the TV's lines
    }
#endif
    if (g_bOutputShots) {
        config.aspect = bWidescreen ? PS2GFX_ASPECT_16_9 : PS2GFX_ASPECT_4_3;
    }
    if (ps2gfx_init(&config) != 0) {
        Fail("cannot start ps2gfx");
    }
    ParseShots();
    pthread_t thread;
    if (pthread_create(&thread, nullptr, RunPresents, nullptr) != 0) {
        Fail("cannot start the present thread");
    }
    pthread_detach(thread);
    g_bDrawing = true;
}

bool Drawing() {
    pthread_once(&g_startOnce, Start);
    return g_bDrawing;
}

// Called with g_lock held, after the blank that brings the count to nVblank. A shot whose blank
// passed before the breadcrumb was recorded is taken at once.
// Called with g_lock held.
void ScheduleNextShot() {
    unsigned nNext = ~0U;
    for (const ShotGroup &group : g_shotGroups) {
        for (unsigned nOffset : group.offsets) {
            if (group.bAnchored) {
                nNext = std::min(nNext, group.nAnchor + nOffset + 1);
            }
        }
    }
    g_nNextShotVblank = nNext;
}

void TakeShots(unsigned nVblank) {
    for (ShotGroup &group : g_shotGroups) {
        if (!group.bAnchored) {
            continue;
        }
        for (auto it = group.offsets.begin(); it != group.offsets.end();) {
            const unsigned nOffset = *it;
            if (nVblank - group.nAnchor < nOffset + 1) {
                ++it;
                continue;
            }
            char szPath[4096];
            snprintf(szPath,
                     sizeof(szPath),
                     "%s/%s+%03u.png",
                     g_pszShotDirectory,
                     group.breadcrumb.c_str(),
                     nOffset);
            const int nResult =
                g_bOutputShots ? ps2gfx_screenshot_output(szPath) : ps2gfx_screenshot(szPath);
            if (g_bOutputShots && g_bVulkan) {
                std::string gsPath = szPath;
                gsPath.replace(gsPath.size() - 4, 4, ".gs.png");
                ps2gfx_screenshot(gsPath.c_str());
            }
            timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const double flSeconds =
                static_cast<double>(now.tv_sec - group.anchorTime.tv_sec) +
                static_cast<double>(now.tv_nsec - group.anchorTime.tv_nsec) / 1e9;
            fprintf(stderr,
                    "SHOT %s%s (blank %u, %.2f s and %u frames after the breadcrumb)\n",
                    szPath,
                    nResult == 0 ? "" : " FAILED",
                    nVblank - group.nAnchor - 1,
                    flSeconds,
                    g_nPresents - group.nAnchorPresents);
            it = group.offsets.erase(it);
        }
    }
    ScheduleNextShot();
}

} // namespace

// PsTex::UnlockMipBitmap(): the game drew this texture level itself, so it is not dumped or replaced.
void HostTexGenerated(
    const char *pszName, int nPsm, int nWidth, int nHeight, const void *pPixels, int nStride) {
    if (Drawing()) {
        ps2gfx_tex_generated(pszName, nPsm, nWidth, nHeight, pPixels, nStride);
    }
}

extern "C" {

// The link wraps HostBreadcrumb() of host/io/pad.cpp, whose blank count anchors the shots.
void __real__Z14HostBreadcrumbPKc(const char *pszName);

void __wrap__Z14HostBreadcrumbPKc(const char *pszName) {
    if (Drawing() && g_nHashFields != 0 && g_hashBreadcrumb == pszName) {
        const unsigned nVblank = HostVblankCount();
        pthread_mutex_lock(&g_lock);
        if (!g_bHashAnchored) {
            g_nHashAnchor = nVblank;
            g_bHashAnchored = true;
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (Drawing() && g_pszShotDirectory != nullptr) {
        const unsigned nVblank = HostVblankCount();
        pthread_mutex_lock(&g_lock);
        for (ShotGroup &group : g_shotGroups) {
            if (!group.bAnchored && group.breadcrumb == pszName) {
                group.nAnchor = nVblank;
                clock_gettime(CLOCK_MONOTONIC, &group.anchorTime);
                group.nAnchorPresents = g_nPresents;
                group.bAnchored = true;
                ScheduleNextShot();
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    __real__Z14HostBreadcrumbPKc(pszName);
}

void HostGfxDma(int nChannel, uint32_t nMadr, uint32_t nQwc, uint32_t nTadr, uint32_t nChcr) {
    if ((nChannel != kVif1Channel && nChannel != kGifChannel) || !Drawing()) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    const SystemHeap systemHeap;
    ps2gfx_dma(nChannel, nMadr, nQwc, nTadr, nChcr);
    pthread_mutex_unlock(&g_lock);
}

void HostGfxReadImage(void *pDest, unsigned nBytes) {
    if (!Drawing()) {
        return;
    }
    pthread_mutex_lock(&g_lock);
    const SystemHeap systemHeap;
    ps2gfx_read_image(pDest, nBytes);
    pthread_mutex_unlock(&g_lock);
}

// The game's thread at its swap: wait for the present of the frame swapped at nTick, so the GS
// is never more than a frame behind, and pump the game's timers (as its drawing does) so that the
// scheduler and the music keep their time while the renderer is slow.
void HostGfxWaitPresented(unsigned nTick) {
    if (!g_bDrawing || HostFixedClockNs() >= 0) {
        return; // the fixed clock presents each tick before the game goes on
    }
    pthread_mutex_lock(&g_presentLock);
    while (g_nPresentedTick < nTick && g_nPendingVblank >= nTick) {
        pthread_mutex_unlock(&g_presentLock);
        RunLongOperationPollProc();
        pthread_mutex_lock(&g_presentLock);
        timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += kPumpNs;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_nsec -= 1000000000L;
            ++until.tv_sec;
        }
        pthread_cond_timedwait(&g_presentedSignal, &g_presentLock, &until);
    }
    pthread_mutex_unlock(&g_presentLock);
}

void HostGfxVsync() {
    if (!Drawing()) {
        return;
    }
    const unsigned nTick = HostFrameTick() + 1;
    if (nTick / HostTicksPerField() >= g_nNextShotVblank || HostFixedClockNs() >= 0) {
        ps2gfx_wait_frames();
        pthread_mutex_lock(&g_lock);
        const SystemHeap systemHeap;
        PresentLocked(nTick);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_lock(&g_presentLock);
    g_nPendingVblank = nTick;
    pthread_cond_signal(&g_presentSignal);
    pthread_mutex_unlock(&g_presentLock);
}

} // extern "C"
