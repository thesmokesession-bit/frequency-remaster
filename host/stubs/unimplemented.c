#include <stdio.h>

// Every stub in stubs.S calls this before it returns zero. The first call of each stub names it.
void HostUnimplemented(const char *pszName, unsigned char *pbReported) {
    if (__atomic_exchange_n(pbReported, 1, __ATOMIC_RELAXED) == 0) {
        fprintf(stderr, "UNIMPL %s\n", pszName);
    }
}
