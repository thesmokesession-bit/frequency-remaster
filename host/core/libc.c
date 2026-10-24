// The console routines of the Sony C library and the runtime's double compare, over the host.
//
// src/os/filelog.cpp defines read(), write(), and close() for the whole executable and hands a
// plain descriptor to these routines, so they reach the kernel directly rather than through those
// names.

#define _GNU_SOURCE

#include <sys/syscall.h>
#include <unistd.h>

#include <libcconsole.h>
#include <termios.h>

int reax(int nFile, void *pBuffer, int nLength) {
    return (int)syscall(SYS_read, nFile, pBuffer, (size_t)nLength);
}

int writx(int nFile, const void *pBuffer, int nLength) {
    return (int)syscall(SYS_write, nFile, pBuffer, (size_t)nLength);
}

int LibcConsoleClose(int nFile) {
    return (int)syscall(SYS_close, nFile);
}

int LibcConsoleLseek(int nFile, int nOffset, int nOrigin) {
    return (int)syscall(SYS_lseek, nFile, (off_t)nOffset, nOrigin);
}

int LibcConsoleIsatty(int nFile) {
    struct termios settings;
    return tcgetattr(nFile, &settings) == 0;
}

// The console C library's rand() (newlib's, in SCUS_971.25 at 0x0053aa78): a 32-bit linear
// congruential step from 1, never reseeded. The game's draws (the computer player's catches) must
// follow it, not the host's rand(), which is another generator that the host's libraries share.
int HostRand(void) {
    static unsigned nNext = 1;
    nNext = nNext * 0x41c64e6du + 12345u;
    return (int)(nNext & 0x7fffffffu);
}

// The Emotion Engine runtime's three-way compare, which reports an unordered pair as greater.
int freq_compare_double(double a, double b) {
    if (a < b) {
        return -1;
    }
    return (a == b) ? 0 : 1;
}
