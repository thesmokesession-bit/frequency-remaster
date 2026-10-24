// fopen() over the game's file layer. The game's C library on the console opened every stream
// through _open() in src/os/filelog.cpp, which finds the file in an archive or on the disc. The
// host C library opens host files instead, so this fopen() takes its place for the whole program.
// Host code that wants a host file uses open() and fdopen().

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
int _open(const char *pszPath, int nFlags, ...);
off_t _lseek(int nFile, off_t nOffset, int nOrigin);
}

namespace {

// The open() flags of the console's C library, which _open() reads.
constexpr int kReadOnly = 0;
constexpr int kWriteOnly = 1;
constexpr int kReadWrite = 2;
constexpr int kAppend = 0x8;
constexpr int kCreate = 0x200;
constexpr int kTruncate = 0x400;

int FileOf(void *pCookie) {
    return static_cast<int>(reinterpret_cast<intptr_t>(pCookie));
}

// read(), write(), and close() are the game's own, from src/os/filelog.cpp.
ssize_t ReadCookie(void *pCookie, char *pBuffer, size_t nLength) {
    return read(FileOf(pCookie), pBuffer, nLength);
}

ssize_t WriteCookie(void *pCookie, const char *pBuffer, size_t nLength) {
    return write(FileOf(pCookie), pBuffer, nLength);
}

int SeekCookie(void *pCookie, off64_t *pllPosition, int nOrigin) {
    const off_t nPosition = _lseek(FileOf(pCookie), *pllPosition, nOrigin);
    if (nPosition < 0) {
        return -1;
    }
    *pllPosition = nPosition;
    return 0;
}

int CloseCookie(void *pCookie) {
    return close(FileOf(pCookie));
}

} // namespace

extern "C" FILE *fopen(const char *pszPath, const char *pszMode) {
    int nFlags = kReadOnly;
    if (pszMode[0] == 'w') {
        nFlags = kWriteOnly | kCreate | kTruncate;
    } else if (pszMode[0] == 'a') {
        nFlags = kWriteOnly | kCreate | kAppend;
    }
    if (strchr(pszMode, '+') != nullptr) {
        nFlags = (nFlags & ~kWriteOnly) | kReadWrite;
    }
    const int nFile = _open(pszPath, nFlags, 0666);
    if (nFile < 0) {
        return nullptr;
    }
    if (nFlags != kReadOnly) {
        const cookie_io_functions_t functions = {ReadCookie, WriteCookie, SeekCookie, CloseCookie};
        return fopencookie(
            reinterpret_cast<void *>(static_cast<intptr_t>(nFile)), pszMode, functions);
    }

    // A stream to read is a copy of the file in a memory file, so that fileno() and fstat() work
    // on it as Python's import expects.
    const int nCopy = memfd_create("resonance-file", MFD_CLOEXEC);
    char abBuffer[0x10000];
    off_t nSize = 0;
    ssize_t nRead;
    while (nCopy >= 0 && (nRead = read(nFile, abBuffer, sizeof(abBuffer))) > 0 &&
           pwrite(nCopy, abBuffer, nRead, nSize) == nRead) {
        nSize += nRead;
    }
    close(nFile);
    // The console's C library reports a modification time of zero for every file.
    const timespec aTimes[2] = {{0, 0}, {0, 0}};
    if (nCopy >= 0) {
        futimens(nCopy, aTimes);
    }
    FILE *pFile = nCopy >= 0 ? fdopen(nCopy, pszMode) : nullptr;
    if (pFile == nullptr && nCopy >= 0) {
        syscall(SYS_close, nCopy);
    }
    return pFile;
}
