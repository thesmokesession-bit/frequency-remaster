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
#include <time.h>
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

// libcurl and OpenSSL (the updater) name these; as Perfect Eye's ps5/ps5-shims.c defines them.
// curl's socketpair fallback:
int pipe2(int anFds[2], int nFlags) {
    if (pipe(anFds) != 0) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (nFlags & O_NONBLOCK) {
            fcntl(anFds[i], F_SETFL, fcntl(anFds[i], F_GETFL) | O_NONBLOCK);
        }
        if (nFlags & O_CLOEXEC) {
            fcntl(anFds[i], F_SETFD, FD_CLOEXEC);
        }
    }
    return 0;
}
// curl: an IPv6 link-local scope by interface name.
unsigned int if_nametoindex(const char *pszName) {
    (void)pszName;
    errno = ENXIO;
    return 0;
}
// OpenSSL's DTLS batching, which falls back to one datagram at a time on ENOSYS.
struct mmsghdr;
ssize_t sendmmsg(int nSocket, struct mmsghdr *pVector, size_t nCount, int nFlags) {
    (void)nSocket, (void)pVector, (void)nCount, (void)nFlags;
    errno = ENOSYS;
    return -1;
}
ssize_t recvmmsg(int nSocket, struct mmsghdr *pVector, size_t nCount, int nFlags, const struct timespec *pTimeout) {
    (void)nSocket, (void)pVector, (void)nCount, (void)nFlags, (void)pTimeout;
    errno = ENOSYS;
    return -1;
}
// OpenSSL's async jobs switch stacks with these; nothing here starts one.
int _setjmp(void *pEnv) {
    (void)pEnv;
    return 0;
}
void _longjmp(void *pEnv, int nValue) {
    (void)pEnv, (void)nValue;
    abort();
}
// libzstd's tracing hooks, weak imports the title converter refuses: no tracing.
unsigned long long ZSTD_trace_compress_begin(const void *pContext) {
    (void)pContext;
    return 0;
}
void ZSTD_trace_compress_end(unsigned long long nContext, const void *pTrace) {
    (void)nContext, (void)pTrace;
}
unsigned long long ZSTD_trace_decompress_begin(const void *pContext) {
    (void)pContext;
    return 0;
}
void ZSTD_trace_decompress_end(unsigned long long nContext, const void *pTrace) {
    (void)nContext, (void)pTrace;
}

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
// The console calls it the old BSD way even with SA_SIGINFO: the second argument is the fault code
// (1), not a siginfo, and reading it as one faulted inside this handler (2026-11-24 klog: fault
// address 9 = 1 + si_code's offset), which hid the real crash. The third is the context either way.
static void OnCrash(int nSignal, siginfo_t *pInfo, void *pContext) {
    if ((uintptr_t)pInfo < 0x1000) {
        pInfo = NULL;
    }
    if (pContext == NULL) {
        Report("CRASH signal %d, no context\n", nSignal);
        _Exit(128 + nSignal);
    }
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
    close(fileno(stderr));
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

// ---- auto-pause: the system UI over the game, and rest mode ----

// The PS4 SDK's SceSystemServiceStatus (an event count, then a byte per flag) as the PS5's
// libSceSystemService is taken to fill it; abRest leaves room for a longer struct. The first bytes
// are logged at the first poll and at each change, so the console's log can confirm the layout.
typedef struct {
    int32_t nEventCount;
    uint8_t bSystemUiOverlaid;
    uint8_t bInBackground;
    uint8_t abRest[250];
} SystemServiceStatus;
int sceSystemServiceGetStatus(SystemServiceStatus *pStatus);
void HostAutoPause(int nPlayer); // src/game/inputpollerps2.cpp: FREQ_AUTOPAUSE

static long long MonotonicNs(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

// Ten times a second. A song pauses when the system UI comes over the game or the title goes to
// the background (once per change, so a flag read wrongly pauses once at most), and after more
// than two seconds between two polls: rest mode froze the title. The game's clocks take such a
// gap as one frame (host/audio/audio.cpp RunAudio(), host/gfx-null/gfx.cpp RunVblanks()).
static void *WatchSystem(void *pUnused) {
    (void)pUnused;
    unsigned char abLast[8] = {0};
    int nLastResult = 1;
    int bCovered = 0;
    long long nLastNs = MonotonicNs();
    for (;;) {
        usleep(100000);
        const long long nNowNs = MonotonicNs();
        if (nNowNs - nLastNs > 2000000000LL) {
            fprintf(stderr,
                    "PS5: %lld s between two polls (rest mode?): the song pauses\n",
                    (nNowNs - nLastNs) / 1000000000LL);
            HostAutoPause(0);
        }
        nLastNs = nNowNs;
        SystemServiceStatus status;
        memset(&status, 0, sizeof(status));
        const int nResult = sceSystemServiceGetStatus(&status);
        if (nResult != nLastResult || memcmp(abLast, &status, sizeof(abLast)) != 0) {
            memcpy(abLast, &status, sizeof(abLast));
            nLastResult = nResult;
            char szBytes[3 * sizeof(abLast) + 1];
            for (size_t i = 0; i < sizeof(abLast); ++i) {
                snprintf(szBytes + 3 * i, 4, " %02x", abLast[i]);
            }
            fprintf(stderr,
                    "PS5: system status 0x%x, bytes%s (events 0-3, UI overlaid 4, background 5)\n",
                    (unsigned)nResult,
                    szBytes);
        }
        const int bNow = nResult == 0 && (status.bSystemUiOverlaid || status.bInBackground);
        if (bNow && !bCovered) {
            HostAutoPause(0);
        }
        bCovered = bNow;
    }
    return NULL;
}

// The rest waits for main (host/core/main.cpp), as Perfect Eye's ps5PlatformInit does: a thread
// or a signal handler set up from a constructor ended the title before main (2026-11-23).
static void StartUpdater(void);

void HostStartTitle(void) {
    int anPipe[2];
    if (freopen(LOG_PATH, "a", stdout) != NULL && pipe(anPipe) == 0) {
        g_nPipe = anPipe[0];
        if (pthread_create(&g_writer, NULL, WriteLog, NULL) == 0) {
            // stderr too, from here on: a line written to the title folder can take tens of ms,
            // and RADV prints its submission line holding the submit lock, which held the GS
            // ~95 ms every 10 s (2026-11-23; Perfect Eye's open_log, "stutters, 2026-10-18").
            // The crash report still writes the file itself.
            fflush(stderr);
            dup2(anPipe[1], fileno(stdout));
            dup2(anPipe[1], fileno(stderr));
            setvbuf(stderr, NULL, _IOLBF, 0);
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
    pthread_t watcher;
    if (pthread_create(&watcher, NULL, WatchSystem, NULL) == 0) {
        pthread_detach(watcher);
    }
    StartUpdater();
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
    InTitleFolder("FREQ_SONGS", "songs");
    fprintf(stderr, "PS5: disc %s, memory card %s\n", getenv("FREQ_DISC"), getenv("FREQ_MC"));
    // At 120 fps the display mode is taken on the console's word (radv-videoout-120hz.patch): the
    // frame line's presents a second show the rate the TV really runs at.
    const char *pszFps = getenv("FREQ_FPS");
    if (pszFps != NULL && atoi(pszFps) == 120) {
        setenv("VIDEOOUT_TRUST_HIGH_FRAME_RATE", "1", 0);
    }
}

// ---- the network, for the updater (as Perfect Eye's ps5/ps5_platform.c and ps5-shims.c) ----
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

// getaddrinfo (link-title.sh binds the name here): numbers as they are, names through the
// console's resolver in libSceNet, since the SDK's own only fails. IPv4 answers; an IPv6 request
// gets them mapped. Each import is read through a volatile pointer, so one the console leaves
// unresolved is a failed lookup rather than a call to address 0.
int sceNetPoolCreate(const char *pszName, int nSize, int nFlags);
int sceNetResolverCreate(const char *pszName, int nMemory, int nFlags);
int sceNetResolverStartNtoa(int nResolver, const char *pszHost, uint32_t *pnAddress, int nTimeout, int nRetry,
                            int nFlags);
int sceNetResolverDestroy(int nResolver);

static int ResolveName(const char *pszName, struct in_addr *pOut) {
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    static int s_nPool = -1;
    if (strcasecmp(pszName, "localhost") == 0) {
        pOut->s_addr = htonl(INADDR_LOOPBACK);
        return 0;
    }
    int (*volatile pfnPoolCreate)(const char *, int, int) = sceNetPoolCreate;
    int (*volatile pfnCreate)(const char *, int, int) = sceNetResolverCreate;
    int (*volatile pfnNtoa)(int, const char *, uint32_t *, int, int, int) = sceNetResolverStartNtoa;
    int (*volatile pfnDestroy)(int) = sceNetResolverDestroy;
    if (pfnPoolCreate == NULL || pfnCreate == NULL || pfnNtoa == NULL || pfnDestroy == NULL) {
        return -1;
    }
    pthread_mutex_lock(&s_lock);
    if (s_nPool < 0) {
        s_nPool = pfnPoolCreate("freq_resolver", 64 * 1024, 0);
    }
    const int nPool = s_nPool;
    pthread_mutex_unlock(&s_lock);
    const int nResolver = pfnCreate("freq_resolver", nPool, 0);
    if (nResolver < 0) {
        return -1;
    }
    uint32_t nAddress = 0;
    const int nResult = pfnNtoa(nResolver, pszName, &nAddress, 0, 0, 0);
    pfnDestroy(nResolver);
    if (nResult < 0 || nAddress == 0) {
        fprintf(stderr, "PS5: %s did not resolve: 0x%08x\n", pszName, (unsigned)nResult);
        return -1;
    }
    pOut->s_addr = nAddress;
    return 0;
}

int freq_ps5_getaddrinfo(const char *pszNode, const char *pszService, const struct addrinfo *pHints,
                         struct addrinfo **ppResult) {
    const int nFamily = pHints != NULL ? pHints->ai_family : AF_UNSPEC;
    const int nFlags = pHints != NULL ? pHints->ai_flags : 0;
    int nPort = 0, bSix = 0;
    struct in_addr v4;
    struct in6_addr v6;
    if (ppResult == NULL) {
        return EAI_FAIL;
    }
    *ppResult = NULL;
    if (nFamily != AF_UNSPEC && nFamily != AF_INET && nFamily != AF_INET6) {
        return EAI_FAMILY;
    }
    if (pszService != NULL && *pszService != '\0') {
        char *pszEnd;
        const long n = strtol(pszService, &pszEnd, 10);
        if (*pszEnd == '\0' && n >= 0 && n < 65536) {
            nPort = (int)n;
        } else if (strcasecmp(pszService, "https") == 0) {
            nPort = 443;
        } else if (strcasecmp(pszService, "http") == 0) {
            nPort = 80;
        } else {
            return EAI_SERVICE;
        }
    }
    if (pszNode == NULL) {
        if (nFamily == AF_INET6) {
            bSix = 1;
            v6 = (nFlags & AI_PASSIVE) ? in6addr_any : in6addr_loopback;
        } else {
            v4.s_addr = htonl((nFlags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
        }
    } else if (inet_pton(AF_INET6, pszNode, &v6) == 1) {
        if (nFamily == AF_INET) {
            return EAI_NONAME;
        }
        bSix = 1;
    } else if (inet_pton(AF_INET, pszNode, &v4) != 1) {
        if ((nFlags & AI_NUMERICHOST) || ResolveName(pszNode, &v4) != 0) {
            return EAI_NONAME;
        }
    }
    if (!bSix && nFamily == AF_INET6) { // v4-mapped, ::ffff:a.b.c.d
        memset(&v6, 0, sizeof(v6));
        v6.s6_addr[10] = 0xff;
        v6.s6_addr[11] = 0xff;
        memcpy(&v6.s6_addr[12], &v4, 4);
        bSix = 1;
    }
    struct Block {
        struct addrinfo info;
        struct sockaddr_storage address;
    } *pBlock = calloc(1, sizeof(*pBlock));
    if (pBlock == NULL) {
        return EAI_MEMORY;
    }
    pBlock->info.ai_flags = nFlags;
    pBlock->info.ai_socktype = pHints != NULL ? pHints->ai_socktype : 0;
    pBlock->info.ai_protocol = pHints != NULL ? pHints->ai_protocol : 0;
    pBlock->info.ai_addr = (struct sockaddr *)&pBlock->address;
    if (bSix) {
        struct sockaddr_in6 *pIn6 = (struct sockaddr_in6 *)&pBlock->address;
        pIn6->sin6_len = sizeof(*pIn6);
        pIn6->sin6_family = AF_INET6;
        pIn6->sin6_port = htons((uint16_t)nPort);
        pIn6->sin6_addr = v6;
        pBlock->info.ai_family = AF_INET6;
        pBlock->info.ai_addrlen = sizeof(*pIn6);
    } else {
        struct sockaddr_in *pIn = (struct sockaddr_in *)&pBlock->address;
        pIn->sin_len = sizeof(*pIn);
        pIn->sin_family = AF_INET;
        pIn->sin_port = htons((uint16_t)nPort);
        pIn->sin_addr = v4;
        pBlock->info.ai_family = AF_INET;
        pBlock->info.ai_addrlen = sizeof(*pIn);
    }
    *ppResult = &pBlock->info;
    return 0;
}

void freq_ps5_freeaddrinfo(struct addrinfo *pInfo) {
    while (pInfo != NULL) {
        struct addrinfo *pNext = pInfo->ai_next;
        free(pInfo); // one block each, the addrinfo first
        pInfo = pNext;
    }
}

// Only libScePosixForWebKit's stub defines these, so they are null at run time: curl and OpenSSL's
// few uses, numbers only.
const char *gai_strerror(int nCode) {
    (void)nCode;
    return "name resolution failed";
}
struct hostent *gethostbyname(const char *pszName) {
    (void)pszName;
    return NULL;
}
int getnameinfo(const struct sockaddr *pAddress, socklen_t nLength, char *pszHost, size_t nHost, char *pszService,
                size_t nService, int nFlags) {
    (void)nLength, (void)nFlags;
    unsigned nPort = 0;
    if (pAddress->sa_family == AF_INET) {
        const struct sockaddr_in *pIn = (const struct sockaddr_in *)pAddress;
        if (pszHost != NULL && nHost != 0 && inet_ntop(AF_INET, &pIn->sin_addr, pszHost, nHost) == NULL) {
            return EAI_FAIL;
        }
        nPort = ntohs(pIn->sin_port);
    } else if (pAddress->sa_family == AF_INET6) {
        const struct sockaddr_in6 *pIn6 = (const struct sockaddr_in6 *)pAddress;
        if (pszHost != NULL && nHost != 0 && inet_ntop(AF_INET6, &pIn6->sin6_addr, pszHost, nHost) == NULL) {
            return EAI_FAIL;
        }
        nPort = ntohs(pIn6->sin6_port);
    } else {
        return EAI_FAMILY;
    }
    if (pszService != NULL && nService != 0) {
        snprintf(pszService, nService, "%u", nPort);
    }
    return 0;
}

// fcntl, as everything here calls it (link-title.sh wraps the name): the console answers EINVAL
// to a socket's close-on-exec, and curl gave up on every connection over it (Perfect Eye,
// 2026-10-18). There is no exec here, so that is done; a non-blocking mode it refuses goes
// through FIONBIO.
int __real_fcntl(int nFd, int nCommand, ...);
int __wrap_fcntl(int nFd, int nCommand, ...) {
    va_list args;
    va_start(args, nCommand);
    const long nArgument = va_arg(args, long);
    va_end(args);
    const int nResult = __real_fcntl(nFd, nCommand, nArgument);
    if (nResult == -1 && errno == EINVAL) {
        if (nCommand == F_SETFD) {
            return 0;
        }
        if (nCommand == F_SETFL) {
            int bOn = (nArgument & O_NONBLOCK) != 0;
            if (ioctl(nFd, FIONBIO, &bOn) == 0) {
                return 0;
            }
            errno = EINVAL;
        }
    }
    return nResult;
}

// ---- the updater ----

// At start-up a thread asks the release repo (FREQ_UPDATE_REPO, owner/name) for its latest
// release. When its tag is not this build's (FREQ_VERSION, the commit it was built from), the
// release's eboot.bin is downloaded into the title folder in this one's place, as Perfect Eye's
// port/src/update.c does on the PS5, and a notification says so: the new build runs from the next
// start. FREQ_UPDATE_TOKEN is a read-only token for the repo while it is private; FREQ_UPDATE=0
// turns the check off.
#include <curl/curl.h>

#ifndef FREQ_VERSION
#define FREQ_VERSION "unknown"
#endif

typedef struct {
    char useless[45];
    char message[3075];
} NotifyRequest;
int sceKernelSendNotificationRequest(int nDevice, NotifyRequest *pRequest, size_t nSize, int bBlocking);

static void Notify(const char *pszMessage) {
    static NotifyRequest s_request;
    memset(&s_request, 0, sizeof(s_request));
    snprintf(s_request.message, sizeof(s_request.message), "%s", pszMessage);
    fprintf(stderr, "PS5: notification: %s\n", pszMessage);
    sceKernelSendNotificationRequest(0, &s_request, sizeof(s_request), 0);
}

// host/net/online.cpp: the room's news (joined, left) on the console's screen.
void HostNotify(const char *pszMessage) {
    Notify(pszMessage);
}

typedef struct {
    char *pData;
    size_t nSize;
} Buffer;

static size_t ToBuffer(void *pData, size_t nSize, size_t nCount, void *pUser) {
    Buffer *pBuffer = pUser;
    const size_t nAdd = nSize * nCount;
    char *pGrown = realloc(pBuffer->pData, pBuffer->nSize + nAdd + 1);
    if (pGrown == NULL) {
        return 0;
    }
    memcpy(pGrown + pBuffer->nSize, pData, nAdd);
    pBuffer->pData = pGrown;
    pBuffer->nSize += nAdd;
    pBuffer->pData[pBuffer->nSize] = '\0';
    return nAdd;
}

// GET pszUrl into a buffer (pFile NULL) or a file, following redirects here. Each host is looked
// up through the console's resolver and given to curl (CURLOPT_RESOLVE), so curl's own threaded
// resolver never runs: it crashed in Curl_async_is_resolved (SYSTEM_XO_VIOLATION, 2026-11-23).
// The token goes to api.github.com only, not to the download host a redirect names.
// A file download resumes from the file's current position, so a caller can retry a stalled one.
static int Fetch(const char *pszUrl, const char *pszAccept, Buffer *pBuffer, FILE *pFile) {
    char szUrl[4096];
    snprintf(szUrl, sizeof(szUrl), "%s", pszUrl);
    const long nFrom = pFile != NULL ? ftell(pFile) : 0;
    for (int nHop = 0; nHop < 5; ++nHop) {
        // The host is everything up to the path's '/', letters, digits, dots and dashes only: no
        // port (CURLOPT_RESOLVE below covers 443 only) and no user part, which would make
        // "https://api.github.com:x@elsewhere/" read as api.github.com and get the token.
        char szHost[256] = "";
        int nHostEnd = 0;
        if (sscanf(szUrl, "https://%255[A-Za-z0-9.-]%n", szHost, &nHostEnd) != 1 || szUrl[nHostEnd] != '/') {
            fprintf(stderr, "PS5: update: refused address %.80s\n", szUrl);
            return -1;
        }
        struct in_addr address;
        if (ResolveName(szHost, &address) != 0) {
            fprintf(stderr, "PS5: update: cannot look up %s\n", szHost);
            return -1;
        }
        char szResolve[320], szIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address, szIp, sizeof(szIp));
        snprintf(szResolve, sizeof(szResolve), "%s:443:%s", szHost, szIp);
        struct curl_slist *pResolve = curl_slist_append(NULL, szResolve);

        struct curl_slist *pHeaders = NULL;
        char szHeader[512];
        snprintf(szHeader, sizeof(szHeader), "Accept: %s", pszAccept);
        pHeaders = curl_slist_append(pHeaders, szHeader);
        pHeaders = curl_slist_append(pHeaders, "X-GitHub-Api-Version: 2022-11-28");
        const char *pszToken = getenv("FREQ_UPDATE_TOKEN");
        if (pszToken != NULL && pszToken[0] != '\0' && strcmp(szHost, "api.github.com") == 0) {
            snprintf(szHeader, sizeof(szHeader), "Authorization: Bearer %s", pszToken);
            pHeaders = curl_slist_append(pHeaders, szHeader);
        }
        if (pFile != NULL) { // a redirect's own body is not the file
            fflush(pFile);
            ftruncate(fileno(pFile), nFrom);
            fseek(pFile, nFrom, SEEK_SET);
        } else if (pBuffer->pData != NULL) {
            pBuffer->nSize = 0;
        }
        CURL *pCurl = curl_easy_init();
        if (pCurl == NULL) {
            curl_slist_free_all(pResolve);
            curl_slist_free_all(pHeaders);
            return -1;
        }
        curl_easy_setopt(pCurl, CURLOPT_URL, szUrl);
        curl_easy_setopt(pCurl, CURLOPT_RESOLVE, pResolve);
        curl_easy_setopt(pCurl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pHeaders);
        curl_easy_setopt(pCurl, CURLOPT_USERAGENT, "Frequency-PS5/" FREQ_VERSION);
        curl_easy_setopt(pCurl, CURLOPT_CAINFO, TITLE_DIR "/cacert.pem");
        curl_easy_setopt(pCurl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, pFile != NULL ? 600L : 30L);
        // An eboot is about 60 MB and a release's JSON a few KB: anything far past is not ours.
        curl_easy_setopt(pCurl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)(pFile != NULL ? 256 << 20 : 1 << 20));
        if (pFile != NULL) {
            curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, pFile); // curl's own fwrite
            curl_easy_setopt(pCurl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)nFrom);
            // The first console download stalled 60 KB from the end for the full 600 s
            // (2026-11-24): under 1 KB/s for 30 s ends the attempt and the caller resumes.
            curl_easy_setopt(pCurl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(pCurl, CURLOPT_LOW_SPEED_TIME, 30L);
        } else {
            curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, ToBuffer);
            curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, pBuffer);
        }
        const CURLcode result = curl_easy_perform(pCurl);
        long nStatus = 0;
        char *pszLocation = NULL;
        curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &nStatus);
        curl_easy_getinfo(pCurl, CURLINFO_REDIRECT_URL, &pszLocation);
        const int bRedirect = result == CURLE_OK && nStatus >= 300 && nStatus < 400 && pszLocation != NULL &&
                              strncmp(pszLocation, "https://", 8) == 0 && strlen(pszLocation) < sizeof(szUrl);
        if (bRedirect) {
            snprintf(szUrl, sizeof(szUrl), "%s", pszLocation);
        }
        if (pFile != NULL && !bRedirect) {
            curl_off_t nGot = 0;
            curl_easy_getinfo(pCurl, CURLINFO_SIZE_DOWNLOAD_T, &nGot);
            fprintf(stderr, "PS5: update: %s: %lld bytes, %s (HTTP %ld)\n", szHost, (long long)nGot,
                    curl_easy_strerror(result), nStatus);
        }
        curl_easy_cleanup(pCurl);
        curl_slist_free_all(pResolve);
        curl_slist_free_all(pHeaders);
        if (bRedirect) {
            continue;
        }
        if (result != CURLE_OK || nStatus >= 300) { // a redirect that could not be followed too
            fprintf(stderr, "PS5: update: %s: %s (HTTP %ld)\n", szHost, curl_easy_strerror(result), nStatus);
            return -1;
        }
        return 0;
    }
    return -1;
}

// The string value after "pszKey": at pszFrom, into pszOut. NULL if there is none.
static const char *JsonString(const char *pszFrom, const char *pszKey, char *pszOut, size_t nOut) {
    char szNeedle[64];
    snprintf(szNeedle, sizeof(szNeedle), "\"%s\":\"", pszKey);
    const char *pszAt = pszFrom != NULL ? strstr(pszFrom, szNeedle) : NULL;
    if (pszAt == NULL) {
        return NULL;
    }
    pszAt += strlen(szNeedle);
    const char *pszEnd = strchr(pszAt, '"');
    if (pszEnd == NULL || (size_t)(pszEnd - pszAt) >= nOut) {
        return NULL;
    }
    memcpy(pszOut, pszAt, (size_t)(pszEnd - pszAt));
    pszOut[pszEnd - pszAt] = '\0';
    return pszEnd;
}

static void *RunUpdater(void *pUnused) {
    (void)pUnused;
    // A build that does not know its commit would take every release for an update, every start.
    if (strcmp(FREQ_VERSION, "unknown") == 0) {
        fprintf(stderr, "PS5: update: this build has no version, not checking\n");
        return NULL;
    }
    const char *pszRepo = getenv("FREQ_UPDATE_REPO");
    if (pszRepo == NULL) {
        pszRepo = "thesmokesession-bit/frequency-remaster";
    }
    char szUrl[512];
    snprintf(szUrl, sizeof(szUrl), "https://api.github.com/repos/%s/releases/latest", pszRepo);
    Buffer release = {NULL, 0};
    char szTag[128];
    if (Fetch(szUrl, "application/vnd.github+json", &release, NULL) != 0 ||
        JsonString(release.pData, "tag_name", szTag, sizeof(szTag)) == NULL) {
        free(release.pData);
        return NULL;
    }
    // The release's tag ends with the commit it was built from (v1.0-abc1234, or abc1234).
    const size_t nTag = strlen(szTag), nVersion = strlen(FREQ_VERSION);
    if (nTag >= nVersion && strcmp(szTag + nTag - nVersion, FREQ_VERSION) == 0) {
        fprintf(stderr, "PS5: update: %s is the latest\n", szTag);
        free(release.pData);
        return NULL;
    }
    // The eboot.bin asset's API address: the "url" its object starts with, before its "name".
    char szAsset[512] = "";
    const char *pszName = strstr(release.pData, "\"name\":\"eboot.bin\"");
    for (const char *pszAt = release.pData; pszName != NULL;) {
        const char *pszUrl = strstr(pszAt, "\"url\":\"");
        if (pszUrl == NULL || pszUrl > pszName) {
            break;
        }
        JsonString(pszUrl, "url", szAsset, sizeof(szAsset));
        pszAt = pszUrl + 1;
    }
    const char *pszSize = pszName != NULL ? strstr(pszName, "\"size\":") : NULL;
    const long nAssetSize = pszSize != NULL ? strtol(pszSize + 7, NULL, 10) : 0;
    free(release.pData);
    if (strstr(szAsset, "/releases/assets/") == NULL) {
        fprintf(stderr, "PS5: update: release %s has no eboot.bin\n", szTag);
        return NULL;
    }
    fprintf(stderr, "PS5: update: downloading %s (this build is %s)\n", szTag, FREQ_VERSION);
    const char *pszNew = TITLE_DIR "/eboot.bin.new";
    FILE *pFile = fopen(pszNew, "wb");
    if (pFile != NULL) { // unbuffered: the file's size is what has arrived
        setvbuf(pFile, NULL, _IONBF, 0);
    }
    int nFetched = -1;
    for (int nTry = 0; pFile != NULL && nFetched != 0 && nTry < 5; ++nTry) {
        nFetched = Fetch(szAsset, "application/octet-stream", NULL, pFile);
    }
    fprintf(stderr, "PS5: update: download %s\n", nFetched == 0 ? "complete" : "failed");
    const long nSize = pFile != NULL ? ftell(pFile) : 0;
    if (pFile != NULL) {
        fflush(pFile); // on the disk before it replaces the eboot: a power cut must not leave half
        fsync(fileno(pFile));
        fclose(pFile);
    }
    // A signed title starts with the SELF magic; anything else (an error page) is not installed.
    unsigned char abMagic[4] = {0};
    pFile = nFetched == 0 ? fopen(pszNew, "rb") : NULL;
    if (pFile != NULL) {
        fread(abMagic, 1, sizeof(abMagic), pFile);
        fclose(pFile);
    }
    static const unsigned char kSelfMagic[4] = {0x4f, 0x15, 0x3d, 0x1d};
    // 0777: without the executable bit the home screen cannot start the title (Perfect Eye,
    // 2026-10-19).
    if (nFetched != 0 || nSize < (1 << 20) || nSize != nAssetSize ||
        memcmp(abMagic, kSelfMagic, 4) != 0 ||
        chmod(pszNew, 0777) != 0 || rename(pszNew, TITLE_DIR "/eboot.bin") != 0) {
        fprintf(stderr, "PS5: update: %s not installed (%ld bytes)\n", szTag, nSize);
        unlink(pszNew);
        return NULL;
    }
    char szMessage[256];
    snprintf(szMessage, sizeof(szMessage), "Frequency updated to %s. Close and start it again to play it.",
             szTag);
    Notify(szMessage);
    return NULL;
}

static void StartUpdater(void) {
    const char *pszUpdate = getenv("FREQ_UPDATE");
    if (pszUpdate != NULL && strcmp(pszUpdate, "0") == 0) {
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pthread_t thread;
    if (pthread_create(&thread, NULL, RunUpdater, NULL) == 0) {
        pthread_detach(thread);
    }
}
