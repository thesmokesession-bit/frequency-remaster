// The null graphics backend: the hardware register pages as plain memory, libdma with transfers
// that finish at once, and the 59.94 Hz vertical blank.
//
// HostGfxDma() and HostGfxVsync() are where a renderer takes the transfers and presents frames.
// Here they do nothing.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <eekernel.h>
#include <libdev.h>
#include <libdma.h>
#include <malloc.h>

#include "../kernel/kernel.h"
#include "gfxnull.h"

namespace {

// The EE's I/O registers (DMA, VIF, GIF, IPU, INTC) and the GS privileged registers.
constexpr uintptr_t kIoRegisters = 0x10000000;
constexpr size_t kIoRegistersSize = 0x10000;
constexpr uintptr_t kGsRegisters = 0x12000000;
constexpr size_t kGsRegistersSize = 0x2000;

// The CSR identifies a GS of revision 0x1b, as the console reports.
constexpr unsigned long long kGsCsrRevision = 0x551b0000ULL;

constexpr uintptr_t kDmacCtrl = 0x1000e000;
constexpr uintptr_t kDmacStat = 0x1000e010;
constexpr int kDmaChannelCount = 10;
constexpr uintptr_t kDmaChannelBase[kDmaChannelCount] = {0x10008000,
                                                         0x10009000,
                                                         0x1000a000,
                                                         0x1000b000,
                                                         0x1000b400,
                                                         0x1000c000,
                                                         0x1000c400,
                                                         0x1000c800,
                                                         0x1000d000,
                                                         0x1000d400};
// The reset leaves the three SIF channels alone, as the IOP link runs over them.
constexpr bool kDmaChannelResettable[kDmaChannelCount] = {
    true, true, true, true, true, false, false, false, true, true};
constexpr uintptr_t kDmaResetRegisters[] = {
    kHostDmaChcr, kHostDmaMadr, kHostDmaQwc, kHostDmaTadr, 0x40, 0x50, 0x80};

// CHCR: MOD (2-3) chooses the mode, DIR (0) the direction.
constexpr uint32_t kChcrModeMask = 0xc;
constexpr uint32_t kChcrNormalFromMemory = 0x101;
constexpr uint32_t kChcrChainFromMemory = 0x105;

// NTSC: 60000 / 1001 fields a second.
constexpr long kVblankPeriodNs = 1001000000L / 60;
constexpr size_t kVblankStackSize = 0x80000;

pthread_mutex_t g_vblankLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_vblankSignal = PTHREAD_COND_INITIALIZER;
pthread_once_t g_vblankOnce = PTHREAD_ONCE_INIT;
unsigned g_nVblankCount = 0;

sceDmaEnv g_dmaEnv;

volatile uint32_t &Register(uintptr_t nAddress) {
    return *reinterpret_cast<volatile uint32_t *>(nAddress);
}

// The registers are plain memory from before the game's first instruction.
__attribute__((constructor)) void MapRegisterPages() {
    const struct {
        uintptr_t nBase;
        size_t nSize;
    } aPages[] = {{kIoRegisters, kIoRegistersSize}, {kGsRegisters, kGsRegistersSize}};
    for (const auto &page : aPages) {
        void *pAddress = reinterpret_cast<void *>(page.nBase);
        if (mmap(pAddress,
                 page.nSize,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                 -1,
                 0) != pAddress) {
            static const char kMessage[] = "host: the hardware register range is taken\n";
            syscall(SYS_write, STDERR_FILENO, kMessage, sizeof(kMessage) - 1);
            _exit(70);
        }
    }
    *HOST_GS_CSR = kGsCsrRevision;
}

void *RunVblanks(void *) {
    timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += kVblankPeriodNs;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);

        // A blank that comes late does not make up the ones it missed.
        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) {
            next = now;
        }

        // The frame is presented while the game still waits for the blank, so the present and
        // the game's next transfers do not overlap.
        HostGfxVsync();

        pthread_mutex_lock(&g_vblankLock);
        *HOST_GS_CSR = (*HOST_GS_CSR ^ HOST_GS_CSR_FIELD) | HOST_GS_CSR_VSYNC;
        ++g_nVblankCount;
        pthread_cond_broadcast(&g_vblankSignal);
        pthread_mutex_unlock(&g_vblankLock);

        HostRaiseIntc(INTC_VBLANK_S);
        HostRaiseIntc(INTC_VBLANK_E);
    }
    return nullptr;
}

// Handlers run on this thread's stack, so it comes from the low heap like a game thread's.
void StartVblanks() {
    pthread_attr_t attributes;
    pthread_t thread;
    void *pStack = memalign(64, kVblankStackSize);
    if (pStack == nullptr || pthread_attr_init(&attributes) != 0 ||
        pthread_attr_setstack(&attributes, pStack, kVblankStackSize) != 0 ||
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0 ||
        pthread_create(&thread, &attributes, RunVblanks, nullptr) != 0) {
        fprintf(stderr, "host: cannot start the vertical blank thread\n");
        _exit(70);
    }
    pthread_attr_destroy(&attributes);
}

int ChannelOf(const sceDmaChan *pChannel) {
    for (int i = 0; i < kDmaChannelCount; ++i) {
        if (reinterpret_cast<uintptr_t>(pChannel) == kDmaChannelBase[i]) {
            return i;
        }
    }
    return -1;
}

// Starts a transfer with the registers written as libdma writes them, and finishes it.
void RunTransfer(sceDmaChan *pChannel, uint32_t nChcrBits) {
    const uintptr_t nBase = reinterpret_cast<uintptr_t>(pChannel);
    const uint32_t nChcr = (Register(nBase + kHostDmaChcr) & ~kChcrModeMask) | nChcrBits;
    Register(nBase + kHostDmaChcr) = nChcr;
    HostGfxDma(ChannelOf(pChannel),
               Register(nBase + kHostDmaMadr),
               Register(nBase + kHostDmaQwc),
               Register(nBase + kHostDmaTadr),
               nChcr);
    Register(nBase + kHostDmaQwc) = 0;
    Register(nBase + kHostDmaChcr) = nChcr & ~kHostDmaStart;
}

} // namespace

extern "C" {

// host/gfx/gfx.cpp replaces both when the build includes it.
__attribute__((weak)) void HostGfxDma(int, uint32_t, uint32_t, uint32_t, uint32_t) {
}

__attribute__((weak)) void HostGfxVsync() {
}

unsigned HostVblankCount() {
    pthread_once(&g_vblankOnce, StartVblanks);
    pthread_mutex_lock(&g_vblankLock);
    const unsigned nCount = g_nVblankCount;
    pthread_mutex_unlock(&g_vblankLock);
    return nCount;
}

int HostWaitVblank() {
    pthread_once(&g_vblankOnce, StartVblanks);
    pthread_mutex_lock(&g_vblankLock);
    const unsigned nStart = g_nVblankCount;
    while (g_nVblankCount == nStart) {
        pthread_cond_wait(&g_vblankSignal, &g_vblankLock);
    }
    const int nField = (*HOST_GS_CSR & HOST_GS_CSR_FIELD) != 0;
    pthread_mutex_unlock(&g_vblankLock);
    return nField;
}

sceDmaChan *sceDmaGetChan(int nChannel) {
    if (nChannel < 0 || nChannel >= kDmaChannelCount) {
        return nullptr;
    }
    return reinterpret_cast<sceDmaChan *>(kDmaChannelBase[nChannel]);
}

int sceDmaReset(int nMode) {
    const uint32_t nPrevious = Register(kDmacCtrl);
    for (int i = 0; i < kDmaChannelCount; ++i) {
        if (kDmaChannelResettable[i]) {
            for (uintptr_t nOffset : kDmaResetRegisters) {
                Register(kDmaChannelBase[i] + nOffset) = 0;
            }
        }
    }
    Register(kDmacStat) = 0;
    memset(&g_dmaEnv, 0, sizeof(g_dmaEnv));
    if (nMode == 1) {
        Register(kDmacCtrl) |= 1;
    }
    return static_cast<int>(nPrevious & 1);
}

// ponytail: the environment (stall control, MFIFO) is kept but not applied. Nothing the null
// backend runs stalls.
int sceDmaPutEnv(sceDmaEnv *pEnv) {
    g_dmaEnv = *pEnv;
    return 0;
}

sceDmaEnv *sceDmaGetEnv(sceDmaEnv *pEnv) {
    *pEnv = g_dmaEnv;
    return pEnv;
}

void sceDmaSend(sceDmaChan *pChannel, void *pTag) {
    const uintptr_t nBase = reinterpret_cast<uintptr_t>(pChannel);
    Register(nBase + kHostDmaTadr) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pTag));
    Register(nBase + kHostDmaQwc) = 0;
    RunTransfer(pChannel, kChcrChainFromMemory);
}

void sceDmaSendN(sceDmaChan *pChannel, void *pAddress, int nQuadwords) {
    const uintptr_t nBase = reinterpret_cast<uintptr_t>(pChannel);
    Register(nBase + kHostDmaMadr) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pAddress));
    Register(nBase + kHostDmaQwc) = static_cast<uint32_t>(nQuadwords);
    RunTransfer(pChannel, kChcrNormalFromMemory);
}

int sceDmaSync(sceDmaChan *pChannel, int nMode, int) {
    if (nMode == 1) {
        return (pChannel->chcr & kHostDmaStart) != 0;
    }
    return 0;
}

// The VU0 and its VIF reset with nothing to reset, and the debug console draws nothing.
void sceDevVif0Reset() {
}

void sceDevVu0Reset() {
}

void sceDevConsInit() {
}

int sceDevConsOpen(unsigned, unsigned, unsigned, unsigned) {
    return 0;
}

void sceDevConsClear(int) {
}

} // extern "C"
