// glibc's <malloc.h> for the PS5 title, whose C library has none (FreeBSD's is an #error): the
// low heap's statistics and the allocation calls host/core/heap.c defines over dlmalloc.
#ifndef _MALLOC_H
#define _MALLOC_H

#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mallinfo {
    int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
};

struct mallinfo mallinfo(void);
int mallopt(int nParameter, int nValue);
int malloc_trim(size_t nPad);
void malloc_stats(void);
void *memalign(size_t nAlignment, size_t nSize);
void *valloc(size_t nSize);
void *pvalloc(size_t nSize);

#ifdef __cplusplus
}
#endif

#endif
