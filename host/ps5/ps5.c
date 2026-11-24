// The PS5 title's platform layer (cmake/ps5-toolchain.cmake): the log and the crash report in the
// title folder, the settings, disc image and memory card from the title folder, the Emotion Engine
// memory at its fixed low addresses, and the system calls the Linux host makes by number.
//
// The title folder is /app0 (/data/homebrew/PPSA64010 on the console). frequency.ini there is read
// as on Linux; FREQ_DISC and FREQ_MC are relative to it, and default to the first .iso there and to
// memcard/.

#include <dirent.h>
#include <fnmatch.h>
#include <ftw.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include <ps5platform/kernel.h>

#define TITLE_DIR "/app0"
#define LOG_PATH TITLE_DIR "/frequency.log"

int sceKernelRead(int nFile, void *pBuffer, size_t nLength);
int sceKernelWrite(int nFile, const void *pBuffer, size_t nLength);
int sceKernelClose(int nFile);
off_t sceKernelLseek(int nFile, off_t nOffset, int nOrigin);
int main(void);

// ---- the Emotion Engine memory ----

// sceKernelReserveVirtualRange: exactly at the address, and never over anything mapped.
#define RESERVE_FIXED 0x10
#define RESERVE_NO_OVERWRITE 0x80
// kHostUncachedAlias of host/core/lowmem.h.
#define UNCACHED_ALIAS 0x20000000u

static size_t RoundUp(size_t nSize, size_t nUnit) {
    return (nSize + nUnit - 1) & ~(nUnit - 1);
}

// Maps direct memory at exactly nAddress: over a reservation made here first, or with bReplace over
// what is mapped there (the image's own pages).
static int32_t MapAt(uintptr_t nAddress, size_t nSize, int64_t nPhysical, int bReplace) {
    void *pAddress = (void *)nAddress;
    if (!bReplace) {
        const int32_t nReserve = sceKernelReserveVirtualRange(
            &pAddress, nSize, RESERVE_FIXED | RESERVE_NO_OVERWRITE, PS5_KERNEL_PAGE_SIZE);
        if (nReserve != 0 || pAddress != (void *)nAddress) {
            return nReserve != 0 ? nReserve : -1;
        }
    }
    const int32_t nMap = sceKernelMapDirectMemory(&pAddress,
                                                  nSize,
                                                  PS5_KERNEL_PROT_CPU_READ |
                                                      PS5_KERNEL_PROT_CPU_WRITE,
                                                  PS5_KERNEL_MAP_FIXED,
                                                  nPhysical,
                                                  PS5_KERNEL_DIRECT_ALIGNMENT);
    return nMap != 0 ? nMap : (pAddress == (void *)nAddress ? 0 : -1);
}

// Every fixed low mapping of the native build: nSize bytes of read-write memory at nBase, and with
// bAlias the same memory again at the uncached alias nBase + 0x20000000. With pContents the memory
// starts with those bytes and replaces the pages at nBase (the image's static data, which then gets
// its alias); without, nothing may be mapped at nBase yet. One direct memory allocation backs both
// views, which a 64 KiB unit rounds up, and 16 KiB pages map. Returns 0 or the kernel's result.
// ponytail: the T0 design of leaf 5.0 (P/p8/probe), unproven on 13.60 until the probe's console run.
int HostMapGameMemory(uintptr_t nBase, size_t nSize, const void *pContents, int bAlias) {
    if ((nBase & (PS5_KERNEL_PAGE_SIZE - 1)) != 0) {
        return -1;
    }
    const size_t nMapped = RoundUp(nSize, PS5_KERNEL_PAGE_SIZE);
    int64_t nPhysical = 0;
    int32_t nResult = sceKernelAllocateDirectMemory(0,
                                                   sceKernelGetDirectMemorySize(),
                                                   RoundUp(nMapped, PS5_KERNEL_DIRECT_ALIGNMENT),
                                                   PS5_KERNEL_DIRECT_ALIGNMENT,
                                                   PS5_KERNEL_DIRECT_TYPE_CPU,
                                                   &nPhysical);
    if (nResult != 0) {
        return nResult;
    }
    // The alias first: the contents go in through it while the image's pages are still in place.
    if (bAlias && (nResult = MapAt(nBase + UNCACHED_ALIAS, nMapped, nPhysical, 0)) != 0) {
        return nResult;
    }
    if (pContents != NULL) {
        memcpy((void *)(nBase + UNCACHED_ALIAS), pContents, nMapped);
    }
    return MapAt(nBase, nMapped, nPhysical, pContents != NULL);
}

// ---- the system calls the Linux host makes by number ----

// The console's libkernel runs these; its results are 0x8002xxxx with the errno in the low bits.
static long KernelResult(long nResult) {
    if (nResult < 0) {
        errno = (int)(nResult & 0xffff);
        return -1;
    }
    return nResult;
}

int syscall(int nNumber, ...) {
    va_list args;
    va_start(args, nNumber);
    const int nFile = va_arg(args, int);
    long nResult;
    if (nNumber == SYS_read) {
        void *pBuffer = va_arg(args, void *);
        nResult = sceKernelRead(nFile, pBuffer, va_arg(args, size_t));
    } else if (nNumber == SYS_write) {
        const void *pBuffer = va_arg(args, const void *);
        nResult = sceKernelWrite(nFile, pBuffer, va_arg(args, size_t));
    } else if (nNumber == SYS_close) {
        nResult = sceKernelClose(nFile);
    } else if (nNumber == SYS_lseek) {
        const off_t nOffset = va_arg(args, off_t);
        nResult = sceKernelLseek(nFile, nOffset, va_arg(args, int));
    } else {
        nResult = -(long)ENOSYS | 0x80020000L;
    }
    va_end(args);
    return (int)KernelResult(nResult);
}

// ---- C library functions the console's lacks ----

// libiconv's multibyte width: the C locale is the only one in a title.
int ___mb_cur_max(void) {
    return 1;
}

// Imports that only a stub module gives a title, null at run time (host/ps5/check-imports.sh):
// FFmpeg's isatty and mkstemp, RADV's mkstemp and readlink, SDL2's strcasestr, libc++'s
// std::filesystem link, symlink and pathconf. No terminal, links or temporary names here.
int isatty(int nFile) {
    (void)nFile;
    errno = ENOTTY;
    return 0;
}

int mkstemps(char *pszTemplate, int nSuffix);
int mkstemp(char *pszTemplate) {
    return mkstemps(pszTemplate, 0);
}

ssize_t readlink(const char *pszPath, char *pBuffer, size_t nSize) {
    (void)pszPath, (void)pBuffer, (void)nSize;
    errno = EINVAL;
    return -1;
}

int link(const char *pszFrom, const char *pszTo) {
    (void)pszFrom, (void)pszTo;
    errno = EPERM;
    return -1;
}

int symlink(const char *pszFrom, const char *pszTo) {
    (void)pszFrom, (void)pszTo;
    errno = EPERM;
    return -1;
}

long pathconf(const char *pszPath, int nName) {
    (void)pszPath, (void)nName;
    return 255;
}

char *strcasestr(const char *pszText, const char *pszFind) {
    const size_t nFind = strlen(pszFind);
    for (; *pszText != '\0'; ++pszText) {
        if (strncasecmp(pszText, pszFind, nFind) == 0) {
            return (char *)pszText;
        }
    }
    return nFind == 0 ? (char *)pszText : NULL;
}

// host/io/memcard.cpp's directory patterns: * and ? and plain characters, and with FNM_PERIOD a
// leading dot only matched by a dot.
static int Match(const char *pszPattern, const char *pszName) {
    if (*pszPattern == '*') {
        for (;; ++pszName) {
            if (Match(pszPattern + 1, pszName) == 0) {
                return 0;
            }
            if (*pszName == '\0') {
                return FNM_NOMATCH;
            }
        }
    }
    if (*pszPattern == '\0' || *pszName == '\0') {
        return *pszPattern == *pszName ? 0 : FNM_NOMATCH;
    }
    return (*pszPattern == '?' || *pszPattern == *pszName) ? Match(pszPattern + 1, pszName + 1)
                                                            : FNM_NOMATCH;
}

int fnmatch(const char *pszPattern, const char *pszName, int nFlags) {
    if ((nFlags & FNM_PERIOD) != 0 && pszName[0] == '.' && pszPattern[0] != '.') {
        return FNM_NOMATCH;
    }
    return Match(pszPattern, pszName);
}

// nftw() as host/io/memcard.cpp walks the card: each entry with its status and depth, directories
// before their contents. ponytail: nFds and the flags are ignored (no links on the card).
static int Walk(char *pszPath, size_t nLength, int nLevel,
                int (*pfnVisit)(const char *, const struct stat *, int, struct FTW *)) {
    struct stat status;
    if (stat(pszPath, &status) != 0) {
        return 0;
    }
    const int bDirectory = S_ISDIR(status.st_mode);
    const char *pszSlash = strrchr(pszPath, '/');
    struct FTW walk = {(int)(pszSlash != NULL ? pszSlash + 1 - pszPath : 0), nLevel};
    int nResult = pfnVisit(pszPath, &status, bDirectory ? FTW_D : FTW_F, &walk);
    DIR *pDirectory = bDirectory && nResult == 0 ? opendir(pszPath) : NULL;
    for (struct dirent *pEntry; pDirectory != NULL && nResult == 0 && (pEntry = readdir(pDirectory)) != NULL;) {
        if (strcmp(pEntry->d_name, ".") == 0 || strcmp(pEntry->d_name, "..") == 0 ||
            nLength + strlen(pEntry->d_name) + 2 > PATH_MAX) {
            continue;
        }
        snprintf(pszPath + nLength, PATH_MAX - nLength, "/%s", pEntry->d_name);
        nResult = Walk(pszPath, strlen(pszPath), nLevel + 1, pfnVisit);
        pszPath[nLength] = '\0';
    }
    if (pDirectory != NULL) {
        closedir(pDirectory);
    }
    return nResult;
}

int nftw(const char *pszRoot, int (*pfnVisit)(const char *, const struct stat *, int, struct FTW *),
         int nFds, int nFlags) {
    (void)nFds;
    (void)nFlags;
    char szPath[PATH_MAX];
    snprintf(szPath, sizeof(szPath), "%s", pszRoot);
    return Walk(szPath, strlen(szPath), 0, pfnVisit);
}

// ---- the log ----

// The game's output goes to frequency.log through a pipe, which a thread of its own writes to the
// file: a write to the title folder can stall, and a game thread would otherwise wait on it.
static int g_nLog = -1;
static int g_nPipe = -1;

static void *WriteLog(void *pUnused) {
    (void)pUnused;
    static char s_abChunk[16384];
    for (;;) {
        const ssize_t nRead = sceKernelRead(g_nPipe, s_abChunk, sizeof(s_abChunk));
        if (nRead <= 0) {
            return NULL;
        }
        sceKernelWrite(g_nLog, s_abChunk, (size_t)nRead);
    }
}

// The crash report goes to the file itself: the writer thread may never get to a pipe's last lines.
static void Report(const char *pszFormat, ...) {
    char szLine[256];
    va_list args;
    va_start(args, pszFormat);
    const int nLength = vsnprintf(szLine, sizeof(szLine), pszFormat, args);
    va_end(args);
    if (nLength > 0) {
        sceKernelWrite(g_nLog, szLine, (size_t)nLength < sizeof(szLine) ? (size_t)nLength : sizeof(szLine) - 1);
    }
}

// The faulting instruction and the code addresses on the stack, with main's address, so that they
// match the unsigned ELF (eboot.bin.elf): address - main here + main there.
static void OnCrash(int nSignal, siginfo_t *pInfo, void *pContext) {
    const mcontext_t *pMachine = &((ucontext_t *)pContext)->uc_mcontext;
    const uintptr_t nCodeStart = (uintptr_t)&main & ~(uintptr_t)0x3fffff;
    const uintptr_t nCodeEnd = nCodeStart + 0x4000000;
    Report("CRASH signal %d code %d address %p rip %p rsp %p main %p\n",
           nSignal,
           pInfo != NULL ? pInfo->si_code : 0,
           pInfo != NULL ? pInfo->si_addr : NULL,
           (void *)pMachine->mc_rip,
           (void *)pMachine->mc_rsp,
           (void *)&main);
    const uintptr_t *pStack = (const uintptr_t *)pMachine->mc_rsp;
    for (int i = 0, nFound = 0; i < 2048 && nFound < 32; ++i) {
        if (pStack[i] >= nCodeStart && pStack[i] < nCodeEnd) {
            Report("  stack[%d] %p\n", i, (void *)pStack[i]);
            ++nFound;
        }
    }
    _Exit(128 + nSignal);
}

// On exit() the writer thread is killed with the pipe's last lines unwritten (often the reason for
// the exit): close the pipe's write ends so it reads to the end, and wait for it.
static pthread_t g_writer;
static void DrainLog(void) {
    fflush(NULL);
    close(fileno(stdout));
    pthread_join(g_writer, NULL);
}

__attribute__((constructor(100))) static void StartTitle(void) {
    g_nLog = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    Report("PS5: title started\n");
    // The console's stdio writes to no file descriptor until reopened onto a file (as Perfect
    // Eye's open_log does). stderr (errors and the host's few lines) stays on the file itself,
    // unbuffered, so the last line before the system kills the title is not lost; stdout then
    // moves onto the pipe.
    if (freopen(LOG_PATH, "a", stderr) != NULL) {
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    // host/core/settings.c reads it next (priority 101).
    setenv("FREQ_CONFIG", TITLE_DIR "/frequency.ini", 0);
}

// The rest waits for main (host/core/main.cpp), as Perfect Eye's ps5PlatformInit does: a thread
// or a signal handler set up from a constructor ended the title before main (2026-11-23).
void HostStartTitle(void) {
    int anPipe[2];
    if (freopen(LOG_PATH, "a", stdout) != NULL && pipe(anPipe) == 0) {
        g_nPipe = anPipe[0];
        if (pthread_create(&g_writer, NULL, WriteLog, NULL) == 0) {
            dup2(anPipe[1], fileno(stdout));
            atexit(DrainLog);
        }
        close(anPipe[1]);
    }

    static char s_abAltStack[64 * 1024];
    const stack_t altStack = {.ss_sp = s_abAltStack, .ss_size = sizeof(s_abAltStack)};
    sigaltstack(&altStack, NULL);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    const int anSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS};
    for (size_t i = 0; i < sizeof(anSignals) / sizeof(anSignals[0]); ++i) {
        sigaction(anSignals[i], &action, NULL);
    }

    fprintf(stderr, "PS5: logging and crash report set up\n");
}

// ---- the disc image and the memory card ----

// A relative setting is a file in the title folder.
static void InTitleFolder(const char *pszKey, const char *pszDefault) {
    const char *pszValue = getenv(pszKey);
    if (pszValue == NULL) {
        pszValue = pszDefault;
    }
    if (pszValue != NULL && pszValue[0] != '/') {
        char szPath[1024];
        snprintf(szPath, sizeof(szPath), "%s/%s", TITLE_DIR, pszValue);
        setenv(pszKey, szPath, 1);
    }
}

__attribute__((constructor(102))) static void FindGameFiles(void) {
    static char s_szIso[256];
    DIR *pDirectory = opendir(TITLE_DIR);
    for (struct dirent *pEntry; pDirectory != NULL && (pEntry = readdir(pDirectory)) != NULL;) {
        const size_t nLength = strlen(pEntry->d_name);
        if (nLength > 4 && nLength < sizeof(s_szIso) &&
            strcasecmp(pEntry->d_name + nLength - 4, ".iso") == 0) {
            strcpy(s_szIso, pEntry->d_name);
            break;
        }
    }
    if (pDirectory != NULL) {
        closedir(pDirectory);
    }
    InTitleFolder("FREQ_DISC", s_szIso[0] != '\0' ? s_szIso : NULL);
    InTitleFolder("FREQ_MC", "memcard");
    fprintf(stderr, "PS5: disc %s, memory card %s\n", getenv("FREQ_DISC"), getenv("FREQ_MC"));
}
