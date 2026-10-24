// The memory card in port 1 as the directory FREQ_MC. A card in the other ports and multitap
// slots is never inserted, and FREQ_MC unset means no card at all.
//
// Each libmc call does its work at once and leaves its result for sceMcSync(), which reports it
// finished on the next poll. The game issues one command at a time.
//
// ponytail: the card's file system is the host's. The free space is the card's 8000 KiB less the
// files and directory entries in it, and sceMcUnformat() leaves the files alone.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <fnmatch.h>
#include <ftw.h>
#include <libmc.h>

namespace {

constexpr int kCardKilobytes = 8000;
constexpr int kKilobyte = 1024;
constexpr int kFileCount = 16;

// Results of the server beyond those libmc.h names. An empty slot reports no card.
constexpr int kResultNoCard = -10;

// The attribute words the card reports for a directory and a file.
constexpr unsigned short kDirectoryAttributes = 0x8427;
constexpr unsigned short kFileAttributes = 0x8497;

struct CardFile {
    int nDescriptor;
    long long llPosition;
};

CardFile g_aFiles[kFileCount] = {};
bool g_bPending = false;
int g_nCommand = 0;
int g_nResult = 0;
long long g_llUsedBytes = 0;

// The card's directory, or null when this port and slot hold no card.
const char *Card(int nPort, int nSlot) {
    if (nPort != 0 || nSlot != 0) {
        return nullptr;
    }
    return getenv("FREQ_MC");
}

bool Formatted(const char *pszCard) {
    struct stat status;
    return stat(pszCard, &status) == 0 && S_ISDIR(status.st_mode);
}

int Finish(int nCommand, int nResult) {
    g_nCommand = nCommand;
    g_nResult = nResult;
    g_bPending = true;
    return 0;
}

int ResultOf(int nError) {
    switch (nError) {
    case ENOENT:
    case ENOTDIR:
    case EEXIST:
        return sceMcResNoEntry;
    case ENOTEMPTY:
        return sceMcResNotEmpty;
    case ENOSPC:
        return sceMcResFullDevice;
    default:
        return sceMcResDeniedPermit;
    }
}

// The host path of a card path, or an empty string for a path that leaves the card.
std::string HostPath(const char *pszCard, const char *pszName) {
    if (strstr(pszName, "..") != nullptr) {
        return std::string();
    }
    while (*pszName == '/') {
        ++pszName;
    }
    return std::string(pszCard) + "/" + pszName;
}

// Resolves the card and path of a command, or reports the command finished with the card's
// error. Returns false when the command has already finished.
bool Prepare(int nCommand, int nPort, int nSlot, const char *pszName, std::string *pPath) {
    const char *pszCard = Card(nPort, nSlot);
    if (pszCard == nullptr) {
        Finish(nCommand, kResultNoCard);
        return false;
    }
    if (!Formatted(pszCard)) {
        Finish(nCommand, sceMcResNoFormat);
        return false;
    }
    *pPath = HostPath(pszCard, pszName != nullptr ? pszName : "");
    if (pPath->empty()) {
        Finish(nCommand, sceMcResNoEntry);
        return false;
    }
    return true;
}

int CountUsed(const char *, const struct stat *pStatus, int, FTW *pWalk) {
    if (pWalk->level > 0) {
        g_llUsedBytes += kKilobyte + ((pStatus->st_size + kKilobyte - 1) / kKilobyte) * kKilobyte;
    }
    return 0;
}

int FreeKilobytes(const char *pszCard) {
    g_llUsedBytes = 0;
    nftw(pszCard, CountUsed, 8, FTW_PHYS);
    const long long llFree = kCardKilobytes - g_llUsedBytes / kKilobyte;
    return llFree > 0 ? static_cast<int>(llFree) : 0;
}

void FillEntry(sceMcTblGetDir *pEntry, const char *pszName, const struct stat &status) {
    memset(pEntry, 0, sizeof(*pEntry));
    tm local;
    localtime_r(&status.st_mtime, &local);
    sceMcStDateTime stamp = {};
    stamp.Sec = static_cast<unsigned char>(local.tm_sec);
    stamp.Min = static_cast<unsigned char>(local.tm_min);
    stamp.Hour = static_cast<unsigned char>(local.tm_hour);
    stamp.Day = static_cast<unsigned char>(local.tm_mday);
    stamp.Month = static_cast<unsigned char>(local.tm_mon + 1);
    stamp.Year = static_cast<unsigned short>(local.tm_year + 1900);
    pEntry->_Create = stamp;
    pEntry->_Modify = stamp;
    pEntry->FileSizeByte = S_ISDIR(status.st_mode) ? 0 : static_cast<unsigned>(status.st_size);
    pEntry->AttrFile = S_ISDIR(status.st_mode) ? kDirectoryAttributes : kFileAttributes;
    snprintf(reinterpret_cast<char *>(pEntry->EntryName), sizeof(pEntry->EntryName), "%s", pszName);
}

CardFile *FindFile(int nFile) {
    if (nFile < 0 || nFile >= kFileCount || g_aFiles[nFile].nDescriptor <= 0) {
        return nullptr;
    }
    return &g_aFiles[nFile];
}

} // namespace

extern "C" {

// The card of a set FREQ_MC starts out formatted and empty.
int sceMcInitLibrary() {
    const char *pszCard = Card(0, 0);
    if (pszCard != nullptr && mkdir(pszCard, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "host: cannot create the memory card directory %s\n", pszCard);
    }
    return 0;
}

int sceMcSync(int, int *pnCommand, int *pnResult) {
    if (!g_bPending) {
        return sceMcExecIdle;
    }
    g_bPending = false;
    if (pnCommand != nullptr) {
        *pnCommand = g_nCommand;
    }
    if (pnResult != nullptr) {
        *pnResult = g_nResult;
    }
    return sceMcExecFinish;
}

int sceMcGetInfo(int nPort, int nSlot, int *pnType, int *pnFree, int *pnFormat) {
    const char *pszCard = Card(nPort, nSlot);
    const bool bFormatted = pszCard != nullptr && Formatted(pszCard);
    if (pnType != nullptr) {
        *pnType = pszCard != nullptr ? sceMcTypePS2 : sceMcTypeNoCard;
    }
    if (pnFree != nullptr) {
        *pnFree = bFormatted ? FreeKilobytes(pszCard) : 0;
    }
    if (pnFormat != nullptr) {
        *pnFormat = bFormatted ? 1 : 0;
    }
    if (pszCard == nullptr) {
        return Finish(sceMcFuncNoCardInfo, kResultNoCard);
    }
    return Finish(sceMcFuncNoCardInfo, bFormatted ? sceMcResSucceed : sceMcResNoFormat);
}

int sceMcOpen(int nPort, int nSlot, const char *pszName, int nMode) {
    std::string path;
    if (!Prepare(sceMcFuncNoOpen, nPort, nSlot, pszName, &path)) {
        return 0;
    }
    int nFlags = O_RDONLY;
    if ((nMode & sceMcFileAttrWriteable) != 0) {
        nFlags = (nMode & sceMcFileAttrReadable) != 0 ? O_RDWR : O_WRONLY;
    }
    if ((nMode & sceMcFileCreateFile) != 0) {
        nFlags |= O_CREAT;
    }
    for (int i = 0; i < kFileCount; ++i) {
        if (g_aFiles[i].nDescriptor > 0) {
            continue;
        }
        const int nDescriptor = open(path.c_str(), nFlags | O_CLOEXEC, 0644);
        if (nDescriptor < 0) {
            return Finish(sceMcFuncNoOpen, ResultOf(errno));
        }
        g_aFiles[i] = CardFile{nDescriptor, 0};
        return Finish(sceMcFuncNoOpen, i);
    }
    return Finish(sceMcFuncNoOpen, sceMcResUpLimitHandle);
}

// The game defines close(), read(), and write() for itself, so the card reaches the kernel through
// the system call and positioned transfers.
int sceMcClose(int nFile) {
    CardFile *pFile = FindFile(nFile);
    if (pFile == nullptr) {
        return Finish(sceMcFuncNoClose, sceMcResDeniedPermit);
    }
    syscall(SYS_close, pFile->nDescriptor);
    pFile->nDescriptor = 0;
    return Finish(sceMcFuncNoClose, sceMcResSucceed);
}

int sceMcSeek(int nFile, int nOffset, int nOrigin) {
    CardFile *pFile = FindFile(nFile);
    if (pFile == nullptr) {
        return Finish(sceMcFuncNoSeek, sceMcResDeniedPermit);
    }
    long long llBase = 0;
    if (nOrigin == 1) {
        llBase = pFile->llPosition;
    } else if (nOrigin == 2) {
        struct stat status;
        fstat(pFile->nDescriptor, &status);
        llBase = status.st_size;
    }
    pFile->llPosition = llBase + nOffset;
    return Finish(sceMcFuncNoSeek, static_cast<int>(pFile->llPosition));
}

int sceMcRead(int nFile, void *pBuffer, int nSize) {
    CardFile *pFile = FindFile(nFile);
    if (pFile == nullptr) {
        return Finish(sceMcFuncNoRead, sceMcResDeniedPermit);
    }
    const ssize_t nRead = pread(pFile->nDescriptor, pBuffer, nSize, pFile->llPosition);
    if (nRead < 0) {
        return Finish(sceMcFuncNoRead, ResultOf(errno));
    }
    pFile->llPosition += nRead;
    return Finish(sceMcFuncNoRead, static_cast<int>(nRead));
}

int sceMcWrite(int nFile, const void *pBuffer, int nSize) {
    CardFile *pFile = FindFile(nFile);
    if (pFile == nullptr) {
        return Finish(sceMcFuncNoWrite, sceMcResDeniedPermit);
    }
    const ssize_t nWritten = pwrite(pFile->nDescriptor, pBuffer, nSize, pFile->llPosition);
    if (nWritten < 0) {
        return Finish(sceMcFuncNoWrite, ResultOf(errno));
    }
    pFile->llPosition += nWritten;
    return Finish(sceMcFuncNoWrite, static_cast<int>(nWritten));
}

int sceMcMkdir(int nPort, int nSlot, const char *pszName) {
    std::string path;
    if (!Prepare(sceMcFuncNoMkdir, nPort, nSlot, pszName, &path)) {
        return 0;
    }
    return Finish(sceMcFuncNoMkdir,
                  mkdir(path.c_str(), 0755) == 0 ? sceMcResSucceed : ResultOf(errno));
}

int sceMcDelete(int nPort, int nSlot, const char *pszName) {
    std::string path;
    if (!Prepare(sceMcFuncNoDelete, nPort, nSlot, pszName, &path)) {
        return 0;
    }
    return Finish(sceMcFuncNoDelete, remove(path.c_str()) == 0 ? sceMcResSucceed : ResultOf(errno));
}

int sceMcRename(int nPort, int nSlot, const char *pszName, const char *pszNewName) {
    std::string path;
    if (!Prepare(sceMcFuncNoRename, nPort, nSlot, pszName, &path)) {
        return 0;
    }
    if (strchr(pszNewName, '/') != nullptr || strstr(pszNewName, "..") != nullptr) {
        return Finish(sceMcFuncNoRename, sceMcResNoEntry);
    }
    const std::string::size_type nSlash = path.rfind('/');
    const std::string newPath = path.substr(0, nSlash + 1) + std::string(pszNewName, 0, 31);
    return Finish(sceMcFuncNoRename,
                  rename(path.c_str(), newPath.c_str()) == 0 ? sceMcResSucceed : ResultOf(errno));
}

// Lists the entries of the directory part of pszName that match its last component, which may
// hold the wildcards * and ?. A directory below the root lists "." and "..", as the card does.
// The whole listing comes back at once, so a continued listing has no more entries.
int sceMcGetDir(int nPort,
                int nSlot,
                const char *pszName,
                unsigned nMode,
                int nMaxEntries,
                sceMcTblGetDir *pTable) {
    std::string path;
    if (!Prepare(sceMcFuncNoGetDir, nPort, nSlot, pszName, &path)) {
        return 0;
    }
    if (nMode != 0) {
        return Finish(sceMcFuncNoGetDir, 0);
    }
    const std::string::size_type nSlash = path.rfind('/');
    const std::string directory = path.substr(0, nSlash);
    const std::string pattern = path.substr(nSlash + 1);
    const bool bRoot = directory == Card(nPort, nSlot);

    DIR *pDirectory = opendir(directory.c_str());
    if (pDirectory == nullptr) {
        return Finish(sceMcFuncNoGetDir, sceMcResNoEntry);
    }
    int nCount = 0;
    while (nCount < nMaxEntries) {
        const dirent *pEntry = readdir(pDirectory);
        if (pEntry == nullptr) {
            break;
        }
        const bool bDot = strcmp(pEntry->d_name, ".") == 0 || strcmp(pEntry->d_name, "..") == 0;
        if (bDot ? (bRoot || fnmatch(pattern.c_str(), pEntry->d_name, 0) != 0) :
                   fnmatch(pattern.c_str(), pEntry->d_name, FNM_PERIOD) != 0) {
            continue;
        }
        struct stat status;
        if (stat((directory + "/" + pEntry->d_name).c_str(), &status) != 0) {
            continue;
        }
        FillEntry(&pTable[nCount++], pEntry->d_name, status);
    }
    closedir(pDirectory);
    return Finish(sceMcFuncNoGetDir, nCount);
}

int sceMcFormat(int nPort, int nSlot) {
    const char *pszCard = Card(nPort, nSlot);
    if (pszCard == nullptr) {
        return Finish(sceMcFuncNoFormat, kResultNoCard);
    }
    mkdir(pszCard, 0755);
    return Finish(sceMcFuncNoFormat, Formatted(pszCard) ? sceMcResSucceed : sceMcResDeniedPermit);
}

int sceMcUnformat(int nPort, int nSlot) {
    return Finish(sceMcFuncNoUnformat, Card(nPort, nSlot) != nullptr ? 0 : kResultNoCard);
}

int sceMcGetEntSpace(int nPort, int nSlot, const char *pszPath) {
    std::string path;
    if (!Prepare(sceMcFuncNoEntSpace, nPort, nSlot, pszPath, &path)) {
        return 0;
    }
    return Finish(sceMcFuncNoEntSpace, FreeKilobytes(Card(nPort, nSlot)));
}

} // extern "C"
