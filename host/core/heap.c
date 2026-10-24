// The C library allocator of the native build: dlmalloc over the heap HostMoreCore() maps below
// kHostDmaLimit, in place of the C library's own. Every malloc() of the game, its interpreter, and
// the host C and C++ libraries comes from it.
//
// The graphics libraries (Mesa, LLVM, SDL2, and libps2gfx) need more than that heap has to spare.
// A thread allocates from the C library's own heap while HostUseSystemHeap() is on for it, and
// free() and realloc() find a block's heap by its address.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

void *HostMoreCore(ptrdiff_t nIncrement);

#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE HostMoreCore
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 1
#define MALLOC_ALIGNMENT 16
#define HAVE_USR_INCLUDE_MALLOC_H 1
#define USE_DL_PREFIX 1

#include "dlmalloc.c"

// kHostHeapBase and kHostDmaLimit of host/core/lowmem.h.
#define HOST_HEAP_BASE 0x08000000U
#define HOST_DMA_LIMIT 0x10000000U

static __thread int t_nSystemHeapDepth;
static pthread_once_t s_systemHeapOnce = PTHREAD_ONCE_INIT;
static void *(*s_pfnSystemMalloc)(size_t);
static void *(*s_pfnSystemCalloc)(size_t, size_t);
static void *(*s_pfnSystemRealloc)(void *, size_t);
static void *(*s_pfnSystemMemalign)(size_t, size_t);
static void (*s_pfnSystemFree)(void *);
static size_t (*s_pfnSystemUsableSize)(void *);

// The lookups allocate from the low heap, as the thread has not switched yet.
static void FindSystemHeap(void) {
    s_pfnSystemMalloc = dlsym(RTLD_NEXT, "malloc");
    s_pfnSystemCalloc = dlsym(RTLD_NEXT, "calloc");
    s_pfnSystemRealloc = dlsym(RTLD_NEXT, "realloc");
    s_pfnSystemMemalign = dlsym(RTLD_NEXT, "memalign");
    s_pfnSystemFree = dlsym(RTLD_NEXT, "free");
    s_pfnSystemUsableSize = dlsym(RTLD_NEXT, "malloc_usable_size");
}

/** Switch the calling thread to the C library's heap (nonzero) or back. The calls nest. */
void HostUseSystemHeap(int bOn) {
    pthread_once(&s_systemHeapOnce, FindSystemHeap);
    t_nSystemHeapDepth += bOn ? 1 : -1;
}

static int IsLowBlock(const void *pBlock) {
    return (uintptr_t)pBlock >= HOST_HEAP_BASE && (uintptr_t)pBlock < HOST_DMA_LIMIT;
}

void *malloc(size_t nSize) {
    return t_nSystemHeapDepth > 0 ? s_pfnSystemMalloc(nSize) : dlmalloc(nSize);
}

void *calloc(size_t nCount, size_t nSize) {
    return t_nSystemHeapDepth > 0 ? s_pfnSystemCalloc(nCount, nSize) : dlcalloc(nCount, nSize);
}

void *memalign(size_t nAlignment, size_t nSize) {
    return t_nSystemHeapDepth > 0 ? s_pfnSystemMemalign(nAlignment, nSize)
                                  : dlmemalign(nAlignment, nSize);
}

void free(void *pBlock) {
    if (pBlock == NULL || IsLowBlock(pBlock)) {
        dlfree(pBlock);
    } else {
        s_pfnSystemFree(pBlock);
    }
}

void *realloc(void *pBlock, size_t nSize) {
    if (pBlock == NULL) {
        return malloc(nSize);
    }
    return IsLowBlock(pBlock) ? dlrealloc(pBlock, nSize) : s_pfnSystemRealloc(pBlock, nSize);
}

size_t malloc_usable_size(void *pBlock) {
    if (pBlock == NULL || IsLowBlock(pBlock)) {
        return dlmalloc_usable_size(pBlock);
    }
    return s_pfnSystemUsableSize(pBlock);
}

int posix_memalign(void **ppBlock, size_t nAlignment, size_t nSize) {
    if (nAlignment % sizeof(void *) != 0 || (nAlignment & (nAlignment - 1)) != 0) {
        return EINVAL;
    }
    void *pBlock = memalign(nAlignment, nSize);
    if (pBlock == NULL) {
        return ENOMEM;
    }
    *ppBlock = pBlock;
    return 0;
}

// C11 and C++17 aligned allocation, which dlmalloc predates.
void *aligned_alloc(size_t nAlignment, size_t nSize) {
    return memalign(nAlignment, nSize);
}

void *valloc(size_t nSize) {
    return memalign(4096, nSize);
}

void *pvalloc(size_t nSize) {
    return memalign(4096, (nSize + 4095) & ~(size_t)4095);
}

// The statistics and tuning describe and change the low heap.
struct mallinfo mallinfo(void) {
    return dlmallinfo();
}

int mallopt(int nParameter, int nValue) {
    return dlmallopt(nParameter, nValue);
}

int malloc_trim(size_t nPad) {
    return dlmalloc_trim(nPad);
}

void malloc_stats(void) {
    dlmalloc_stats();
}
