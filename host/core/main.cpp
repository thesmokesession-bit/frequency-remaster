#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "lowmem.h"

// The game's main() in src/main.cpp, which host/CMakeLists.txt compiles under this name.
int GameMain();

extern "C" void HostAudioStart(); // host/audio/audio.cpp

extern "C" {
// The start of the executable's writable data, from the C start-up file, and its end.
extern char __data_start[];
extern char _end[];

// The main thread's stack, and its size as a symbol address, which MemOpenLog() reports and
// paints as it does the Emotion Engine's. The thread library keeps the thread's control block and
// its thread-local data at the top of the stack it is given, so the thread gets 64 KiB above
// _stack_size that the paint never reaches.
alignas(64) char _stack[HOST_GAME_STACK_SIZE + 0x10000];

// The chains host/core/vumicrocode.S embeds.
extern char g_vu0MicrocodeChain[];
extern char g_vu1MicrocodeChain[];
}

#define HOST_STRINGIFY(value) #value
#define HOST_DEFINE_ABSOLUTE(name, value)                                                          \
    asm(".globl " #name "\n.set " #name ", " HOST_STRINGIFY(value) "\n")
HOST_DEFINE_ABSOLUTE(_stack_size, HOST_GAME_STACK_SIZE);
// The global pointer a thread starts with. x86-64 has no such register.
HOST_DEFINE_ABSOLUTE(_gp, 0);

namespace {

constexpr uintptr_t kPageSize = 0x1000;

// The heap's break, zero until the first allocation maps the heap.
uintptr_t g_heapBreak = 0;

// Reports a failure that leaves no usable memory. It allocates nothing, because it may run inside
// the allocator.
[[noreturn]] void Die(const char *pszMessage) {
    syscall(SYS_write, STDERR_FILENO, pszMessage, strlen(pszMessage));
    _exit(70);
}

#ifdef __PROSPERO__
extern "C" int HostMapGameMemory(uintptr_t nBase, size_t nSize, const void *pContents, int bAlias);
#endif

// Maps nSize bytes of one shared memory object at nBase and again at the uncached alias above it.
// With pContents, the object takes the bytes at pContents and replaces the mapping at nBase.
void MapAliased(uintptr_t nBase, size_t nSize, const void *pContents) {
#ifdef __PROSPERO__ // direct memory, mapped twice (host/ps5/ps5.c)
    if (HostMapGameMemory(nBase, nSize, pContents, 1) != 0) {
        Die("host: the low address range is taken\n");
    }
#else
    const int fd = memfd_create("resonance-ee-memory", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(nSize)) != 0) {
        Die("host: cannot create the low memory object\n");
    }
    if (pContents != nullptr && pwrite(fd, pContents, nSize, 0) != static_cast<ssize_t>(nSize)) {
        Die("host: cannot copy the static data\n");
    }
    const int nPlacement = (pContents != nullptr) ? MAP_FIXED : MAP_FIXED_NOREPLACE;
    void *pAddress = reinterpret_cast<void *>(nBase);
    void *pAlias = reinterpret_cast<void *>(nBase | kHostUncachedAlias);
    if (mmap(pAddress, nSize, PROT_READ | PROT_WRITE, MAP_SHARED | nPlacement, fd, 0) != pAddress ||
        mmap(pAlias, nSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0) !=
            pAlias) {
        Die("host: the low address range is taken\n");
    }
    syscall(SYS_close, fd);
#endif
}

bool IsLow(const void *pAddress) {
    return reinterpret_cast<uintptr_t>(pAddress) < kHostDmaLimit;
}

void Expect(bool bHolds, const char *pszWhat) {
    if (!bHolds) {
        fprintf(stderr, "LOWMEM_FAIL %s\n", pszWhat);
        exit(1);
    }
}

// Maps the scratchpad, and gives the static data its uncached alias. The data is copied into the
// shared object while this thread is the only one.
void MapEmotionEngineMemory() {
    const uintptr_t nDataStart = reinterpret_cast<uintptr_t>(__data_start) & ~(kPageSize - 1);
    const uintptr_t nDataEnd =
        (reinterpret_cast<uintptr_t>(_end) + kPageSize - 1) & ~(kPageSize - 1);
    if (nDataEnd > kHostHeapBase) {
        Die("host: the static data reaches the heap\n");
    }
    MapAliased(nDataStart, nDataEnd - nDataStart, reinterpret_cast<const void *>(nDataStart));

    void *pScratchpad = reinterpret_cast<void *>(kHostScratchpad);
#ifdef __PROSPERO__
    if (HostMapGameMemory(kHostScratchpad, kHostScratchpadSize, nullptr, 0) != 0) {
        Die("host: the scratchpad address is taken\n");
    }
    (void)pScratchpad;
#else
    if (mmap(pScratchpad,
             kHostScratchpadSize,
             PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1,
             0) != pScratchpad) {
        Die("host: the scratchpad address is taken\n");
    }
#endif
}

// FREQ_SELFTEST=lowmem. Every kind of allocation the game makes, its static data, and this
// thread's stack must be DMA addresses with a coherent uncached alias.
int SelfTestLowMemory() {
    int nLocal = 0;
    static int s_nStatic = 0;
    void *pSmall = malloc(16);
    void *pLarge = malloc(32 << 20);
    void *pZeroed = calloc(1024, 16);
    void *pAligned = aligned_alloc(64, 4096);
    void *pMemalign = nullptr;
    int *pNew = new int(1);
    Expect(posix_memalign(&pMemalign, 128, 100) == 0, "posix_memalign");
    void *pGrown = realloc(malloc(8), 1 << 20);

    const void *const apChecked[] = {pSmall, pLarge, pZeroed, pAligned, pMemalign, pNew, pGrown};
    for (const void *pBlock : apChecked) {
        Expect(pBlock != nullptr && IsLow(pBlock) &&
                   reinterpret_cast<uintptr_t>(pBlock) >= kHostHeapBase,
               "heap block outside the heap");
    }
    Expect(IsLow(&nLocal) && &nLocal > reinterpret_cast<int *>(_stack) &&
               &nLocal < reinterpret_cast<int *>(_stack + sizeof(_stack)),
           "thread stack");
    Expect(IsLow(&s_nStatic) && IsLow(_end) && IsLow(g_vu1MicrocodeChain), "static data");

    static_cast<volatile char *>(pLarge)[12345] = 0x5a;
    s_nStatic = 0x1234;
    nLocal = 0x4321;
    Expect(reinterpret_cast<volatile char *>(reinterpret_cast<uintptr_t>(pLarge) |
                                             kHostUncachedAlias)[12345] == 0x5a,
           "heap alias");
    Expect(*reinterpret_cast<volatile int *>(reinterpret_cast<uintptr_t>(&s_nStatic) |
                                             kHostUncachedAlias) == 0x1234,
           "static alias");
    Expect(*reinterpret_cast<volatile int *>(reinterpret_cast<uintptr_t>(&nLocal) |
                                             kHostUncachedAlias) == 0x4321,
           "stack alias");
    reinterpret_cast<volatile char *>(kHostScratchpad)[kHostScratchpadSize - 1] = 1;
    Expect(memcmp(g_vu0MicrocodeChain, "\x05\x00\x00\x10", 4) == 0, "VU0 chain");

    printf("LOWMEM_OK heap=%p..%p stack=%p data_end=%p\n",
           reinterpret_cast<void *>(kHostHeapBase),
           reinterpret_cast<void *>(kHostDmaLimit),
           static_cast<void *>(&nLocal),
           static_cast<void *>(_end));
    return 0;
}

void *RunGame(void *pSelfTest) {
    if (pSelfTest != nullptr) {
        return reinterpret_cast<void *>(static_cast<intptr_t>(SelfTestLowMemory()));
    }
    fprintf(stderr, "GAME_MAIN_REACHED\n");
    return reinterpret_cast<void *>(static_cast<intptr_t>(GameMain()));
}

} // namespace

// The heap's MORECORE for host/core/dlmalloc.c, which calls it under the allocator lock. The heap
// only grows, as the Emotion Engine's does.
extern "C" void *HostMoreCore(ptrdiff_t nIncrement) {
    if (g_heapBreak == 0) {
        MapAliased(kHostHeapBase, kHostDmaLimit - kHostHeapBase, nullptr);
        g_heapBreak = kHostHeapBase;
    }
    if (nIncrement < 0 || static_cast<uintptr_t>(nIncrement) > kHostDmaLimit - g_heapBreak) {
        return reinterpret_cast<void *>(-1);
    }
    void *pPrevious = reinterpret_cast<void *>(g_heapBreak);
    g_heapBreak += static_cast<uintptr_t>(nIncrement);
    return pPrevious;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    MapEmotionEngineMemory();

    const char *pszSelfTest = getenv("FREQ_SELFTEST");
    const bool bSelfTest = pszSelfTest != nullptr && strcmp(pszSelfTest, "lowmem") == 0;
    if (!bSelfTest) {
        HostAudioStart(); // the clock the game reads from here on
    }

    // The game runs on _stack, as the Emotion Engine's main thread does.
    pthread_attr_t attributes;
    pthread_t thread;
    void *pResult = nullptr;
    if (pthread_attr_init(&attributes) != 0 ||
        pthread_attr_setstack(&attributes, _stack, sizeof(_stack)) != 0 ||
        pthread_create(&thread, &attributes, RunGame, bSelfTest ? &attributes : nullptr) != 0 ||
        pthread_join(thread, &pResult) != 0) {
        Die("host: cannot start the game thread\n");
    }
    return static_cast<int>(reinterpret_cast<intptr_t>(pResult));
}
