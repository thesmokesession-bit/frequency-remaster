// The OpenGL graphics backend. libps2gfx runs the VIF1 and GIF transfers through its VU1 and GS,
// and draws each blank's frame in an SDL2 window.
//
// FREQ_GFX=null draws nothing, as the null backend does. Without FREQ_GFX, a missing display also
// leads to the null backend.
//
// FREQ_SHOT=<directory> and FREQ_SHOT_AT=<BREADCRUMB>:<blank>[,<blank>...] save the displayed frame
// as <directory>/<BREADCRUMB>+<blank>.png, where blank 0 is the first blank after the breadcrumb.

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

#include "../core/lowmem.h"
#include "../gfx-null/gfxnull.h"
#include "ps2gfx.h"

extern "C" void HostUseSystemHeap(int bOn); // host/core/heap.c

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

// The display registers the game writes to the privileged page each frame (PMODE, SMODE2, DISPFB1,
// DISPLAY1, DISPFB2, DISPLAY2, and BGCOLOR). The GS reads them at the blank.
constexpr uint32_t kDisplayRegisters[] = {
    0x12000000, 0x12000020, 0x12000070, 0x12000080, 0x12000090, 0x120000a0, 0x120000e0};

// ps2gfx takes one call at a time. The game's threads send transfers and the present thread
// presents.
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// The blank thread only records the blank for the present thread, so a slow present never delays
// the blanks, and the blank count stays with the clock.
pthread_mutex_t g_presentLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_presentSignal = PTHREAD_COND_INITIALIZER;
unsigned g_nPendingVblank = 0;
pthread_once_t g_startOnce = PTHREAD_ONCE_INIT;
bool g_bDrawing = false;
SDL_Window *g_pWindow = nullptr;
SDL_GLContext g_context = nullptr;

const char *g_pszShotDirectory = nullptr;
std::string g_shotBreadcrumb;
std::vector<unsigned> g_shotOffsets;
// The blank count at the shot breadcrumb.
bool g_bShotAnchored = false;
unsigned g_nShotAnchor = 0;
timespec g_shotAnchorTime;
unsigned g_nShotAnchorPresents;
// The blank of the next shot. The blank thread takes it itself, so the shot shows what the GS
// displays at that blank rather than at the present thread's later turn.
std::atomic<unsigned> g_nNextShotVblank{~0U};
// The frames presented, counted on the GS thread.
std::atomic<unsigned> g_nPresents{0};

void TakeShots(unsigned nVblank);

// Called with g_lock held.
void PresentLocked(unsigned nVblank) {
    for (uint32_t nRegister : kDisplayRegisters) {
        ps2gfx_priv_write(
            nRegister, *reinterpret_cast<volatile uint64_t *>(static_cast<uintptr_t>(nRegister)));
    }
    ps2gfx_vsync();
    TakeShots(nVblank);
}

[[noreturn]] void Fail(const char *pszWhat) {
    fprintf(stderr, "host: %s: %s\n", pszWhat, SDL_GetError());
    _exit(70);
}

// The GS thread of ps2gfx runs both callbacks. The context is current there from the first.
void MakeCurrent(void *) {
    HostUseSystemHeap(1);
    SDL_GL_MakeCurrent(g_pWindow, g_context);
}

void Present(void *) {
    SDL_GL_SwapWindow(g_pWindow);
    ++g_nPresents;
    SDL_PumpEvents();
    if (SDL_PeepEvents(nullptr, 1, SDL_PEEKEVENT, SDL_QUIT, SDL_QUIT) > 0) {
        _exit(0);
    }
}

void *RunPresents(void *) {
    HostUseSystemHeap(1);
    unsigned nPresented = 0;
    for (;;) {
        pthread_mutex_lock(&g_presentLock);
        while (g_nPendingVblank == nPresented) {
            pthread_cond_wait(&g_presentSignal, &g_presentLock);
        }
        nPresented = g_nPendingVblank;
        pthread_mutex_unlock(&g_presentLock);

        pthread_mutex_lock(&g_lock);
        PresentLocked(nPresented);
        pthread_mutex_unlock(&g_lock);
    }
    return nullptr;
}

void ParseShots() {
    g_pszShotDirectory = getenv("FREQ_SHOT");
    const char *pszAt = getenv("FREQ_SHOT_AT");
    if (g_pszShotDirectory == nullptr || pszAt == nullptr) {
        g_pszShotDirectory = nullptr;
        return;
    }
    const char *pszColon = strchr(pszAt, ':');
    if (pszColon == nullptr) {
        fprintf(stderr, "host: FREQ_SHOT_AT is not <BREADCRUMB>:<blank>[,<blank>...]\n");
        _exit(70);
    }
    g_shotBreadcrumb.assign(pszAt, pszColon);
    for (const char *p = pszColon + 1; *p != '\0';) {
        char *pszEnd;
        g_shotOffsets.push_back(static_cast<unsigned>(strtoul(p, &pszEnd, 10)));
        p = (*pszEnd == ',') ? pszEnd + 1 : pszEnd + strlen(pszEnd);
    }
}

void Start() {
    const char *pszMode = getenv("FREQ_GFX");
    if (pszMode != nullptr && strcmp(pszMode, "null") == 0) {
        return;
    }
    if (pszMode != nullptr && strcmp(pszMode, "ps2gfx") != 0) {
        fprintf(stderr, "host: FREQ_GFX=%s is neither null nor ps2gfx\n", pszMode);
        _exit(70);
    }
    const SystemHeap systemHeap;
    // Without a session bus, D-Bus would try to start one for SDL2 and wait on it for ever.
    setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", 0);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        if (pszMode == nullptr) {
            fprintf(stderr, "host: no display (%s), so nothing is drawn\n", SDL_GetError());
            return;
        }
        Fail("cannot open the display");
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    g_pWindow = SDL_CreateWindow("FreQuency",
                                 SDL_WINDOWPOS_UNDEFINED,
                                 SDL_WINDOWPOS_UNDEFINED,
                                 kScreenWidth,
                                 kScreenHeight,
                                 SDL_WINDOW_OPENGL);
    if (g_pWindow == nullptr || (g_context = SDL_GL_CreateContext(g_pWindow)) == nullptr ||
        SDL_GL_MakeCurrent(g_pWindow, nullptr) != 0) {
        Fail("cannot create the OpenGL window");
    }

    ps2gfx_config config = {};
    config.spr_base = reinterpret_cast<void *>(kHostScratchpad);
    config.renderer = PS2GFX_RENDERER_OPENGL;
    config.make_current = MakeCurrent;
    config.present = Present;
    config.window_width = kScreenWidth;
    config.window_height = kScreenHeight;
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
    for (unsigned nOffset : g_shotOffsets) {
        nNext = std::min(nNext, g_nShotAnchor + nOffset + 1);
    }
    g_nNextShotVblank = nNext;
}

void TakeShots(unsigned nVblank) {
    if (!g_bShotAnchored) {
        return;
    }
    for (auto it = g_shotOffsets.begin(); it != g_shotOffsets.end();) {
        const unsigned nOffset = *it;
        if (nVblank - g_nShotAnchor < nOffset + 1) {
            ++it;
            continue;
        }
        char szPath[4096];
        snprintf(szPath,
                 sizeof(szPath),
                 "%s/%s+%03u.png",
                 g_pszShotDirectory,
                 g_shotBreadcrumb.c_str(),
                 nOffset);
        const int nResult = ps2gfx_screenshot(szPath);
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double flSeconds = static_cast<double>(now.tv_sec - g_shotAnchorTime.tv_sec) +
                                 static_cast<double>(now.tv_nsec - g_shotAnchorTime.tv_nsec) / 1e9;
        fprintf(stderr,
                "SHOT %s%s (blank %u, %.2f s and %u frames after the breadcrumb)\n",
                szPath,
                nResult == 0 ? "" : " FAILED",
                nVblank - g_nShotAnchor - 1,
                flSeconds,
                g_nPresents - g_nShotAnchorPresents);
        it = g_shotOffsets.erase(it);
    }
    ScheduleNextShot();
}

} // namespace

extern "C" {

// The link wraps HostBreadcrumb() of host/io/pad.cpp, whose blank count anchors the shots.
void __real__Z14HostBreadcrumbPKc(const char *pszName);

void __wrap__Z14HostBreadcrumbPKc(const char *pszName) {
    if (Drawing() && g_pszShotDirectory != nullptr && g_shotBreadcrumb == pszName) {
        const unsigned nVblank = HostVblankCount();
        pthread_mutex_lock(&g_lock);
        if (!g_bShotAnchored) {
            g_nShotAnchor = nVblank;
            clock_gettime(CLOCK_MONOTONIC, &g_shotAnchorTime);
            g_nShotAnchorPresents = g_nPresents;
            g_bShotAnchored = true;
            ScheduleNextShot();
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

void HostGfxVsync() {
    if (!Drawing()) {
        return;
    }
    const unsigned nVblank = HostVblankCount() + 1;
    if (nVblank >= g_nNextShotVblank) {
        pthread_mutex_lock(&g_lock);
        const SystemHeap systemHeap;
        PresentLocked(nVblank);
        pthread_mutex_unlock(&g_lock);
        return;
    }
    pthread_mutex_lock(&g_presentLock);
    g_nPendingVblank = nVblank;
    pthread_cond_signal(&g_presentSignal);
    pthread_mutex_unlock(&g_presentLock);
}

} // extern "C"
