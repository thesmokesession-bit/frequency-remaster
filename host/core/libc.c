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

// The Emotion Engine runtime's three-way compare, which reports an unordered pair as greater.
int freq_compare_double(double a, double b) {
    if (a < b) {
        return -1;
    }
    return (a == b) ? 0 : 1;
}
