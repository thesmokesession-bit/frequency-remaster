// The C library allocator of the native build: dlmalloc over the heap HostMoreCore() maps below
// kHostDmaLimit, in place of the C library's own. Every malloc() of the game, its interpreter, and
// the host C and C++ libraries comes from it.

#include <stddef.h>

void *HostMoreCore(ptrdiff_t nIncrement);

#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define MORECORE HostMoreCore
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define USE_LOCKS 1
#define MALLOC_ALIGNMENT 16
#define HAVE_USR_INCLUDE_MALLOC_H 1

#include "dlmalloc.c"

// C11 and C++17 aligned allocation, which dlmalloc predates.
void *aligned_alloc(size_t nAlignment, size_t nSize) {
    return memalign(nAlignment, nSize);
}
