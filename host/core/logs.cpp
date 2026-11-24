// The history of the log, crash reports, and reports sent to an ntfy topic (hostlogs.h). Files are
// written only at start-up, by the crash handler, by the Linux log's child process, or on the
// report's thread. A write to the PS5's title folder can stall the thread that makes it.

#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <dirent.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <poll.h>
#include <ucontext.h>

#include "hostlogs.h"

#ifndef FREQ_VERSION
#define FREQ_VERSION "unknown"
#endif

extern char **environ;
extern "C" void HostNotify(const char *pszMessage) __attribute__((weak)); // host/ps5/ps5.c
extern "C" int HostSaveSetting(const char *pszKey, const char *pszValue); // host/core/settings.c
std::string HostTitleFile(const char *pszName);                           // host/core/progress.cpp
// main() itself, whose address the crash lines print to match the unsigned ELF.
extern "C" void MainEntry() __asm__("main");

namespace {

constexpr size_t kKept = 10;
constexpr size_t kCrashLogTail = 16384;
constexpr size_t kReportMax = 24000;
constexpr size_t kReportHead = 6000;
// ntfy.sh turns a longer message into an attachment. It deletes attachments after 3 hours.
constexpr size_t kMessageMax = 3800;
constexpr const char *kCrashMark = "CRASH report saved: logs/";
constexpr const char *kRemoved = "[removed]";

// The PS5 fills these arrays from a constructor that runs before this file's initialisers.
char g_szLogs[1024];
char g_szLogPath[1024];
char g_szCrashPath[1200];
char g_szLastCrash[128];
int g_nCrashLog = -1;
std::atomic<bool> g_bSending{false};

void Notify(const std::string &message) {
    if (HostNotify != nullptr) {
        HostNotify(message.c_str()); // which logs it too
    } else {
        fprintf(stderr, "notification: %s\n", message.c_str());
    }
}

// The functions from here to OnCrash() make only system calls. The crash handler uses them.
void Put(int nFile, const char *pData, size_t nSize) {
    while (nFile >= 0 && nSize > 0) {
        const ssize_t nWritten = write(nFile, pData, nSize);
        if (nWritten <= 0) {
            return;
        }
        pData += nWritten;
        nSize -= static_cast<size_t>(nWritten);
    }
}

size_t ReadTail(const char *pszPath, char *pBuffer, size_t nSize) {
    const int nFile = open(pszPath, O_RDONLY);
    if (nFile < 0) {
        return 0;
    }
    const off_t nEnd = lseek(nFile, 0, SEEK_END);
    lseek(nFile, nEnd > static_cast<off_t>(nSize) ? nEnd - static_cast<off_t>(nSize) : 0, SEEK_SET);
    size_t nRead = 0;
    for (ssize_t n; nRead < nSize && (n = read(nFile, pBuffer + nRead, nSize - nRead)) > 0;) {
        nRead += static_cast<size_t>(n);
    }
    close(nFile);
    return nRead;
}

// A line to the report file and to the log.
void CrashLine(int nReport, const char *pszFormat, ...) {
    char szLine[256];
    va_list args;
    va_start(args, pszFormat);
    const int nLength = vsnprintf(szLine, sizeof(szLine), pszFormat, args);
    va_end(args);
    if (nLength > 0) {
        const size_t nSize = std::min(static_cast<size_t>(nLength), sizeof(szLine) - 1);
        Put(nReport, szLine, nSize);
        Put(g_nCrashLog, szLine, nSize);
    }
}

// The faulting instruction and the code addresses on the stack, with main's address, so that they
// match the unsigned ELF (eboot.bin.elf): address - main here + main there.
// The console calls it the old BSD way even with SA_SIGINFO: the second argument is the fault code
// (1), not a siginfo, and reading it as one faulted inside this handler (2026-11-24 klog: fault
// address 9 = 1 + si_code's offset), which hid the real crash. The third is the context either way.
void OnCrash(int nSignal, siginfo_t *pInfo, void *pContext) {
    static std::atomic<bool> s_bCrashed{false};
    if (s_bCrashed.exchange(true)) {
        for (;;) {
            pause(); // a second thread's crash waits for the first report's _Exit()
        }
    }
    const int nReport = open(g_szCrashPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    static const char kTitle[] = "Frequency " FREQ_VERSION " crash report\n";
    Put(nReport, kTitle, sizeof(kTitle) - 1);
    if (reinterpret_cast<uintptr_t>(pInfo) < 0x1000) {
        pInfo = nullptr;
    }
    if (pContext == nullptr) {
        CrashLine(nReport, "CRASH signal %d, no context\n", nSignal);
    } else {
#ifdef __PROSPERO__
        const mcontext_t &machine = static_cast<ucontext_t *>(pContext)->uc_mcontext;
        const uintptr_t nRip = machine.mc_rip;
        const uintptr_t nRsp = machine.mc_rsp;
        const int64_t anRegisters[] = {machine.mc_rax,
                                       machine.mc_rbx,
                                       machine.mc_rcx,
                                       machine.mc_rdx,
                                       machine.mc_rsi,
                                       machine.mc_rdi,
                                       machine.mc_rbp,
                                       machine.mc_r8,
                                       machine.mc_r9,
                                       machine.mc_r10,
                                       machine.mc_r11,
                                       machine.mc_r12,
                                       machine.mc_r13,
                                       machine.mc_r14,
                                       machine.mc_r15,
                                       machine.mc_rflags};
#else
        const greg_t *pRegisters = static_cast<ucontext_t *>(pContext)->uc_mcontext.gregs;
        const uintptr_t nRip = pRegisters[REG_RIP];
        const uintptr_t nRsp = pRegisters[REG_RSP];
        const int64_t anRegisters[] = {pRegisters[REG_RAX],
                                       pRegisters[REG_RBX],
                                       pRegisters[REG_RCX],
                                       pRegisters[REG_RDX],
                                       pRegisters[REG_RSI],
                                       pRegisters[REG_RDI],
                                       pRegisters[REG_RBP],
                                       pRegisters[REG_R8],
                                       pRegisters[REG_R9],
                                       pRegisters[REG_R10],
                                       pRegisters[REG_R11],
                                       pRegisters[REG_R12],
                                       pRegisters[REG_R13],
                                       pRegisters[REG_R14],
                                       pRegisters[REG_R15],
                                       pRegisters[REG_EFL]};
#endif
        static const char *const kNames[] = {"rax",
                                             "rbx",
                                             "rcx",
                                             "rdx",
                                             "rsi",
                                             "rdi",
                                             "rbp",
                                             "r8",
                                             "r9",
                                             "r10",
                                             "r11",
                                             "r12",
                                             "r13",
                                             "r14",
                                             "r15",
                                             "rflags"};
        const uintptr_t nCodeStart = reinterpret_cast<uintptr_t>(&MainEntry) & ~uintptr_t{0x3fffff};
        const uintptr_t nCodeEnd = nCodeStart + 0x4000000;
        CrashLine(nReport,
                  "CRASH signal %d code %d address %p rip %p rsp %p main %p\n",
                  nSignal,
                  pInfo != nullptr ? pInfo->si_code : 0,
                  pInfo != nullptr ? pInfo->si_addr : nullptr,
                  reinterpret_cast<void *>(nRip),
                  reinterpret_cast<void *>(nRsp),
                  reinterpret_cast<void *>(&MainEntry));
        for (size_t i = 0; i < 16; i += 4) {
            CrashLine(nReport,
                      "  %s 0x%llx %s 0x%llx %s 0x%llx %s 0x%llx\n",
                      kNames[i],
                      static_cast<unsigned long long>(anRegisters[i]),
                      kNames[i + 1],
                      static_cast<unsigned long long>(anRegisters[i + 1]),
                      kNames[i + 2],
                      static_cast<unsigned long long>(anRegisters[i + 2]),
                      kNames[i + 3],
                      static_cast<unsigned long long>(anRegisters[i + 3]));
        }
        const uintptr_t *pStack = reinterpret_cast<const uintptr_t *>(nRsp);
        for (int i = 0, nFound = 0; i < 2048 && nFound < 32; ++i) {
            if (pStack[i] >= nCodeStart && pStack[i] < nCodeEnd) {
                CrashLine(nReport, "  stack[%d] %p\n", i, reinterpret_cast<void *>(pStack[i]));
                ++nFound;
            }
        }
    }
    // The log's writer (the PS5's thread, the Linux child process) gets the pipe's last lines into
    // the file meanwhile, for the tail below and for the log itself.
    const timespec drain = {0, 200000000};
    nanosleep(&drain, nullptr);
    if (nReport >= 0) {
        CrashLine(-1, "%s%s\n", kCrashMark, strrchr(g_szCrashPath, '/') + 1);
        static char s_abTail[kCrashLogTail];
        static const char kTailTitle[] = "\nThe end of frequency.log:\n";
        Put(nReport, kTailTitle, sizeof(kTailTitle) - 1);
        Put(nReport, s_abTail, ReadTail(g_szLogPath, s_abTail, sizeof(s_abTail)));
        close(nReport);
    }
    _Exit(128 + nSignal);
}

void Stamp(time_t nSeconds, long nNanoseconds, char *pszOut, size_t nOut) {
    struct tm local;
    localtime_r(&nSeconds, &local);
    const size_t nLength = strftime(pszOut, nOut, "%Y%m%d-%H%M%S", &local);
    snprintf(pszOut + nLength, nOut - nLength, "-%03ld", nNanoseconds / 1000000);
}

// Deletes all but the newest kKept files in logs/ whose names start with pszPrefix. The date in
// the names orders them.
void Prune(const char *pszPrefix) {
    static char s_aszNames[512][96];
    size_t nCount = 0;
    DIR *pDirectory = opendir(g_szLogs);
    for (dirent *pEntry; pDirectory != nullptr && nCount < 512 && (pEntry = readdir(pDirectory));) {
        if (strncmp(pEntry->d_name, pszPrefix, strlen(pszPrefix)) == 0 &&
            strlen(pEntry->d_name) < sizeof(s_aszNames[0])) {
            strcpy(s_aszNames[nCount++], pEntry->d_name);
        }
    }
    if (pDirectory != nullptr) {
        closedir(pDirectory);
    }
    qsort(s_aszNames, nCount, sizeof(s_aszNames[0]), [](const void *pLeft, const void *pRight) {
        return strcmp(static_cast<const char *>(pLeft), static_cast<const char *>(pRight));
    });
    for (size_t i = 0; i + kKept < nCount; ++i) {
        char szPath[1200];
        snprintf(szPath, sizeof(szPath), "%s/%s", g_szLogs, s_aszNames[i]);
        unlink(szPath);
    }
}

#ifndef __PROSPERO__
// The Linux log's child process copies standard output and standard error to where they went
// before, and both to the log, until the game has exited and the pipes are empty.
void CopyLog(int nOut, int nErr, int nLog) {
    pollfd aWait[2] = {{nOut, POLLIN, 0}, {nErr, POLLIN, 0}};
    char abChunk[16384];
    for (int nOpen = 2; nOpen > 0 && poll(aWait, 2, -1) > 0;) {
        for (int i = 0; i < 2; ++i) {
            if (aWait[i].fd < 0 || aWait[i].revents == 0) {
                continue;
            }
            const ssize_t nRead = read(aWait[i].fd, abChunk, sizeof(abChunk));
            if (nRead <= 0) {
                aWait[i].fd = -1;
                --nOpen;
                continue;
            }
            Put(i == 0 ? STDOUT_FILENO : STDERR_FILENO, abChunk, static_cast<size_t>(nRead));
            Put(nLog, abChunk, static_cast<size_t>(nRead));
        }
    }
}
#endif

// ---- the report ----

bool Contains(std::string name, const std::vector<const char *> &words) {
    for (char &c : name) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    for (const char *pszWord : words) {
        if (name.find(pszWord) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void RemoveAll(std::string &text, const std::string &secret) {
    for (size_t nAt = text.find(secret); nAt != std::string::npos;
         nAt = text.find(secret, nAt + strlen(kRemoved))) {
        text.replace(nAt, secret.size(), kRemoved);
    }
}

bool IsNamePart(char c) {
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
}

// Removes from a report the value of every environment variable whose name marks a secret, the
// rest of each "<name>=" or "<name>:" line whose name does, GitHub tokens, and bytes that are not
// printable ASCII (ntfy takes a message as UTF-8 text).
void Scrub(std::string &text) {
    for (char **ppszEntry = environ; *ppszEntry != nullptr; ++ppszEntry) {
        const char *pszEqual = strchr(*ppszEntry, '=');
        if (pszEqual != nullptr && strlen(pszEqual + 1) >= 4 &&
            Contains(std::string(*ppszEntry, static_cast<size_t>(pszEqual - *ppszEntry)),
                     {"TOKEN", "SECRET", "PASSW", "KEY", "AUTH", "ROOM", "BOARD", "CODE"})) {
            RemoveAll(text, pszEqual + 1);
        }
    }
    for (size_t nAt = text.find_first_of("=:"); nAt != std::string::npos;
         nAt = text.find_first_of("=:", nAt + 1)) {
        size_t nName = nAt;
        while (nName > 0 && IsNamePart(text[nName - 1])) {
            --nName;
        }
        if (Contains(text.substr(nName, nAt - nName),
                     {"TOKEN", "SECRET", "PASSW", "KEY", "AUTH"})) {
            const size_t nEnd = text.find('\n', nAt);
            text.replace(nAt + 1,
                         (nEnd == std::string::npos ? text.size() : nEnd) - nAt - 1,
                         std::string(" ") + kRemoved);
        }
    }
    for (const char *pszPrefix : {"ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_"}) {
        for (size_t nAt = text.find(pszPrefix); nAt != std::string::npos;
             nAt = text.find(pszPrefix, nAt + 1)) {
            size_t nEnd = nAt;
            while (nEnd < text.size() &&
                   (isalnum(static_cast<unsigned char>(text[nEnd])) || text[nEnd] == '_')) {
                ++nEnd;
            }
            text.replace(nAt, nEnd - nAt, kRemoved);
        }
    }
    for (char &c : text) {
        if ((c < ' ' || c > '~') && c != '\n' && c != '\t') {
            c = '?';
        }
    }
}

// The file, or its first kReportHead bytes and its last ones when it is longer than kReportMax.
std::string ReadReport(const char *pszPath) {
    const int nFile = open(pszPath, O_RDONLY | O_CLOEXEC);
    if (nFile < 0) {
        return "";
    }
    const auto readAt = [nFile](off_t nFrom, size_t nCount) {
        std::string part(nCount, '\0');
        size_t nRead = 0;
        for (ssize_t n; nRead < nCount &&
                        (n = pread(nFile, &part[nRead], nCount - nRead, nFrom + nRead)) > 0;) {
            nRead += static_cast<size_t>(n);
        }
        part.resize(nRead);
        return part;
    };
    const off_t nSize = lseek(nFile, 0, SEEK_END);
    std::string text;
    if (nSize <= static_cast<off_t>(kReportMax)) {
        text = readAt(0, static_cast<size_t>(nSize));
    } else {
        const size_t nTail = kReportMax - kReportHead;
        text = readAt(0, kReportHead) + "\n[" + std::to_string(nSize - kReportMax) +
               " bytes cut]\n" + readAt(nSize - static_cast<off_t>(nTail), nTail);
    }
    close(nFile);
    return text;
}

size_t Discard(char *pData, size_t nSize, size_t nCount, void *pUser) {
    (void)pData;
    (void)pUser;
    return nSize * nCount;
}

// Posts each part as one ntfy message to url. Returns 0 when every part arrived.
int Post(const std::string &url, const std::vector<std::string> &parts, const std::string &title) {
    CURL *pCurl = curl_easy_init();
    if (pCurl == nullptr) {
        return -1;
    }
    curl_slist *pResolve = nullptr;
#ifdef __PROSPERO__
    // curl's own resolver fails on the console (host/ps5/ps5.c); getaddrinfo there asks the
    // console's.
    char szHost[256] = "";
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    addrinfo *pFound = nullptr;
    if (sscanf(url.c_str(), "https://%255[A-Za-z0-9.-]", szHost) == 1 &&
        getaddrinfo(szHost, "443", &hints, &pFound) == 0) {
        char szIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET,
                  &reinterpret_cast<sockaddr_in *>(pFound->ai_addr)->sin_addr,
                  szIp,
                  sizeof(szIp));
        pResolve = curl_slist_append(nullptr, (std::string(szHost) + ":443:" + szIp).c_str());
        freeaddrinfo(pFound);
    }
    curl_easy_setopt(pCurl, CURLOPT_RESOLVE, pResolve);
    curl_easy_setopt(pCurl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(pCurl, CURLOPT_CAINFO, "/app0/cacert.pem");
#endif
    curl_easy_setopt(pCurl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(pCurl, CURLOPT_USERAGENT, "Frequency/" FREQ_VERSION);
    curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, Discard);
    int nResult = 0;
    for (size_t i = 0; i < parts.size() && nResult == 0; ++i) {
        const std::string header =
            "Title: " + title + " " + std::to_string(i + 1) + "/" + std::to_string(parts.size());
        curl_slist *pHeaders = curl_slist_append(nullptr, header.c_str());
        curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pHeaders);
        curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, parts[i].c_str());
        curl_easy_setopt(pCurl, CURLOPT_POSTFIELDSIZE, static_cast<long>(parts[i].size()));
        const CURLcode result = curl_easy_perform(pCurl);
        long nStatus = 0;
        curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &nStatus);
        curl_slist_free_all(pHeaders);
        if (result != CURLE_OK || nStatus != 200) {
            fprintf(stderr, "reports: %s (HTTP %ld)\n", curl_easy_strerror(result), nStatus);
            nResult = -1;
        }
    }
    curl_easy_cleanup(pCurl);
    curl_slist_free_all(pResolve);
    return nResult;
}

// FREQ_REPORT_TOPIC, created and saved when it is unset. Empty when it is not an ntfy topic name.
std::string Topic() {
    const char *pszTopic = getenv("FREQ_REPORT_TOPIC");
    if (pszTopic != nullptr && pszTopic[0] != '\0') {
        const std::string topic = pszTopic;
        const bool bValid = topic.size() <= 64 &&
                            topic.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRS"
                                                    "TUVWXYZ0123456789_-") == std::string::npos;
        return bValid ? topic : "";
    }
    unsigned char abRandom[8];
    if (RAND_bytes(abRandom, sizeof(abRandom)) != 1) {
        return "";
    }
    std::string topic = "frequency-";
    for (unsigned char c : abRandom) {
        topic += "0123456789abcdef"[c >> 4];
        topic += "0123456789abcdef"[c & 15];
    }
    HostSaveSetting("FREQ_REPORT_TOPIC", topic.c_str());
    return topic;
}

// The upload, and a notification of how it went. Returns 0 when sent.
int SendAndNotify(const std::string &topic) {
    if (topic.empty()) {
        Notify("The report was not sent: FREQ_REPORT_TOPIC must be letters, digits, - or _.");
        return -1;
    }
    const bool bCrash = g_szLastCrash[0] != '\0';
    char szPath[1200];
    snprintf(szPath, sizeof(szPath), "%s/%s", g_szLogs, g_szLastCrash);
    const char *pszPath = bCrash ? szPath : g_szLogPath;
    const char *pszName = strrchr(pszPath, '/') + 1;
    std::string text =
        "Frequency " FREQ_VERSION ", " + std::string(pszName) + "\n" + ReadReport(pszPath);
    Scrub(text);
    text.resize(std::min(text.size(), kReportMax));
    std::vector<std::string> parts;
    for (size_t nAt = 0; nAt < text.size();) {
        size_t nSize = std::min(kMessageMax, text.size() - nAt);
        const size_t nLineEnd = text.rfind('\n', nAt + nSize - 1);
        if (nAt + nSize < text.size() && nLineEnd != std::string::npos && nLineEnd > nAt) {
            nSize = nLineEnd + 1 - nAt;
        }
        parts.push_back(text.substr(nAt, nSize));
        nAt += nSize;
    }
    const char *pszServer = getenv("FREQ_REPORT_SERVER");
    std::string url = pszServer != nullptr && pszServer[0] != '\0' ? pszServer : "https://ntfy.sh";
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/" + topic;
    const int nResult =
        Post(url, parts, std::string("Frequency ") + FREQ_VERSION + (bCrash ? " crash" : " log"));
    fprintf(stderr,
            "reports: %s %s to %s (%zu bytes in %zu messages)\n",
            pszName,
            nResult == 0 ? "sent" : "not sent",
            url.c_str(),
            text.size(),
            parts.size());
    const size_t nScheme = url.find("://");
    const std::string shown = nScheme == std::string::npos ? url : url.substr(nScheme + 3);
    if (nResult != 0) {
        Notify("The report could not be sent. Check the network and try again.");
        return nResult;
    }
    if (bCrash) {
        g_szLastCrash[0] = '\0';
    }
    Notify("Report sent. Open " + shown + " within 12 hours to read it.");
    return 0;
}

void *SendThread(void *pTopic) {
    std::string *pOwned = static_cast<std::string *>(pTopic);
    SendAndNotify(*pOwned);
    delete pOwned;
    g_bSending = false;
    return nullptr;
}

} // namespace

extern "C" {

int HostLogOpen(const char *pszFolder) {
    snprintf(g_szLogs, sizeof(g_szLogs), "%s/logs", pszFolder);
    snprintf(g_szLogPath, sizeof(g_szLogPath), "%s/frequency.log", pszFolder);
    mkdir(g_szLogs, 0755);
    timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    char szStamp[48];
    Stamp(now.tv_sec, now.tv_nsec, szStamp, sizeof(szStamp));
    snprintf(g_szCrashPath, sizeof(g_szCrashPath), "%s/crash-%s.txt", g_szLogs, szStamp);

    static char s_abTail[65536];
    const size_t nTail = ReadTail(g_szLogPath, s_abTail, sizeof(s_abTail));
    const char *pszMark =
        std::search(s_abTail, s_abTail + nTail, kCrashMark, kCrashMark + strlen(kCrashMark));
    g_szLastCrash[0] = '\0';
    if (pszMark != s_abTail + nTail) {
        const size_t nFrom = static_cast<size_t>(pszMark - s_abTail) + strlen(kCrashMark);
        size_t nLength = 0;
        while (nFrom + nLength < nTail && nLength + 1 < sizeof(g_szLastCrash) &&
               (isalnum(static_cast<unsigned char>(s_abTail[nFrom + nLength])) ||
                strchr(".-", s_abTail[nFrom + nLength]) != nullptr)) {
            ++nLength;
        }
        if (nLength > 6 && strncmp(s_abTail + nFrom, "crash-", 6) == 0) {
            memcpy(g_szLastCrash, s_abTail + nFrom, nLength);
            g_szLastCrash[nLength] = '\0';
        }
    }

    struct stat status;
    if (stat(g_szLogPath, &status) == 0 && status.st_size > 0) {
        Stamp(status.st_mtim.tv_sec, status.st_mtim.tv_nsec, szStamp, sizeof(szStamp));
        char szOld[1200];
        snprintf(szOld, sizeof(szOld), "%s/frequency-%s.log", g_szLogs, szStamp);
        rename(g_szLogPath, szOld);
    }
    Prune("frequency-");
    Prune("crash-");
    return open(g_szLogPath, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
}

#ifndef __PROSPERO__
void HostLogStart(void) {
    std::string folder = HostTitleFile("");
    folder.pop_back();
    const int nLog = HostLogOpen(folder.c_str());
    int anOut[2];
    int anErr[2];
    if (nLog >= 0 && pipe(anOut) == 0) {
        if (pipe(anErr) == 0) {
            fflush(stdout);
            fflush(stderr);
            const pid_t nChild = fork();
            if (nChild == 0) {
                // Interrupts and terminal hang-ups go to the game; this process exits after it.
                signal(SIGINT, SIG_IGN);
                signal(SIGTERM, SIG_IGN);
                signal(SIGHUP, SIG_IGN);
                signal(SIGPIPE, SIG_IGN);
                close(anOut[1]);
                close(anErr[1]);
                CopyLog(anOut[0], anErr[0], nLog);
                _exit(0);
            }
            if (nChild > 0) {
                dup2(anOut[1], STDOUT_FILENO);
                dup2(anErr[1], STDERR_FILENO);
            }
            close(anErr[0]);
            close(anErr[1]);
        }
        close(anOut[0]);
        close(anOut[1]);
    }
    if (nLog >= 0) {
        close(nLog);
    }
    HostReportsStart(STDERR_FILENO);
}
#endif

void HostReportsStart(int nLog) {
    g_nCrashLog = nLog;
    static char s_abAltStack[64 * 1024];
    stack_t altStack = {};
    altStack.ss_sp = s_abAltStack;
    altStack.ss_size = sizeof(s_abAltStack);
    sigaltstack(&altStack, nullptr);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = OnCrash;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&action.sa_mask); // A fault inside the handler ends the process.

    for (const int nSignal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS}) {
        sigaction(nSignal, &action, nullptr);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    if (getenv("FREQ_REPORT_TOPIC") == nullptr) {
        Topic(); // Written now, not from the thread that requests a report.
    }

    if (g_szLastCrash[0] != '\0') {
        fprintf(stderr, "reports: the last run crashed; its report is logs/%s\n", g_szLastCrash);
        Notify("Frequency closed after an error last time and saved a crash report. To send it, "
               "choose Send report in the settings.");
    }
    const char *pszTestCrash = getenv("FREQ_TEST_CRASH");
    if (pszTestCrash != nullptr && strcmp(pszTestCrash, "1") == 0) {
        fprintf(stderr, "reports: FREQ_TEST_CRASH=1, crashing now\n");
        int *volatile pNowhere = nullptr;
        *pNowhere = 1;
    }
}

int HostSendReport(void) {
    if (g_bSending.exchange(true)) {
        return -1;
    }
    std::string *pTopic = new std::string(Topic());
    pthread_t thread;
    if (pthread_create(&thread, nullptr, SendThread, pTopic) != 0) {
        delete pTopic;
        g_bSending = false;
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

int HostReportSelfTest(void) {
    const int nResult = SendAndNotify(Topic());
    fprintf(stderr, "REPORT_SELFTEST %s\n", nResult == 0 ? "sent" : "failed");
    return nResult;
}

} // extern "C"
