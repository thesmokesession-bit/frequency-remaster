// The disc drive and the file service over the disc image FREQ_DISC: libcdvd sector reads and
// cdrom0: files, both served from an ISO 9660 image of 2048-byte sectors.
//
// ponytail: a read finishes inside sceCdRead(), and the completion callback runs there too rather
// than on libcdvd's callback thread. The game polls for it either way. The streaming calls
// (sceCdSt*) serve only the intro movie, which does not play here, and stay stubs.

#include <ctype.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <libcdvd.h>
#include <libscf.h>
#include <sifdev.h>

namespace {

constexpr int kSectorSize = 2048;
constexpr int kVolumeDescriptorSector = 16;
constexpr int kRootRecordOffset = 156;
constexpr int kDirectoryFlag = 2;
constexpr int kFileCount = 32;

// The kernel file service's error for a file that does not exist.
constexpr int kNoEntry = -2;
constexpr int kBadDescriptor = -9;

// The disc image, opened on first use. Every call that touches the disc goes through Disc().
int g_nDisc = -1;
sceCdCBFunc g_pfnCallback = nullptr;
int g_nError = 0;

struct DiscFile {
    bool bOpen;
    unsigned nLsn;
    unsigned nSize;
    unsigned nPosition;
};

DiscFile g_aFiles[kFileCount];

unsigned ReadLe32(const unsigned char *pBytes) {
    return pBytes[0] | (pBytes[1] << 8) | (pBytes[2] << 16) |
           (static_cast<unsigned>(pBytes[3]) << 24);
}

// A disc that is not there, or not an ISO 9660 image, stops the program. The console would show
// its disc error screen and never start the game.
int Disc() {
    if (g_nDisc >= 0) {
        return g_nDisc;
    }
    const char *pszPath = getenv("FREQ_DISC");
    if (pszPath == nullptr) {
        fprintf(stderr, "DISC_ERROR FREQ_DISC is not set; point it at the game's disc image\n");
        exit(2);
    }
    g_nDisc = open(pszPath, O_RDONLY | O_CLOEXEC);
    unsigned char abDescriptor[kSectorSize];
    if (g_nDisc < 0 ||
        pread(g_nDisc, abDescriptor, kSectorSize, kVolumeDescriptorSector * kSectorSize) !=
            kSectorSize ||
        memcmp(abDescriptor, "\1CD001", 6) != 0) {
        fprintf(stderr, "DISC_ERROR %s is not an ISO 9660 disc image\n", pszPath);
        exit(2);
    }
    return g_nDisc;
}

bool ReadSectors(unsigned nLsn, unsigned nCount, void *pBuffer) {
    const ssize_t nWanted = static_cast<ssize_t>(nCount) * kSectorSize;
    return pread(Disc(), pBuffer, nWanted, static_cast<off_t>(nLsn) * kSectorSize) == nWanted;
}

// Whether a directory record's name is the path component, ignoring case and the ";1" version.
bool NameMatches(const char *pszRecord, int nRecordLength, const char *pszComponent, int nLength) {
    const char *pszVersion = static_cast<const char *>(memchr(pszRecord, ';', nRecordLength));
    if (pszVersion != nullptr) {
        nRecordLength = static_cast<int>(pszVersion - pszRecord);
    }
    const char *pszComponentVersion = static_cast<const char *>(memchr(pszComponent, ';', nLength));
    if (pszComponentVersion != nullptr) {
        nLength = static_cast<int>(pszComponentVersion - pszComponent);
    }
    return nRecordLength == nLength && strncasecmp(pszRecord, pszComponent, nLength) == 0;
}

// Looks a path such as "\IOP\SIO2MAN.IRX;1" up on the disc. Both separators are accepted.
bool FindFile(const char *pszPath, unsigned *pnLsn, unsigned *pnSize, bool *pbDirectory) {
    unsigned char abSector[kSectorSize];
    if (!ReadSectors(kVolumeDescriptorSector, 1, abSector)) {
        return false;
    }
    unsigned nLsn = ReadLe32(abSector + kRootRecordOffset + 2);
    unsigned nSize = ReadLe32(abSector + kRootRecordOffset + 10);
    bool bDirectory = true;

    const char *pszComponent = pszPath;
    while (*pszComponent != '\0') {
        while (*pszComponent == '\\' || *pszComponent == '/') {
            ++pszComponent;
        }
        if (*pszComponent == '\0') {
            break;
        }
        const int nLength = static_cast<int>(strcspn(pszComponent, "\\/"));
        if (!bDirectory) {
            return false;
        }
        bool bFound = false;
        for (unsigned nOffset = 0; nOffset < nSize && !bFound; nOffset += kSectorSize) {
            if (!ReadSectors(nLsn + nOffset / kSectorSize, 1, abSector)) {
                return false;
            }
            // Records never cross a sector, and a zero length pads out the rest of one.
            for (int i = 0; i < kSectorSize && abSector[i] != 0; i += abSector[i]) {
                const unsigned char *pRecord = abSector + i;
                if (NameMatches(reinterpret_cast<const char *>(pRecord + 33),
                                pRecord[32],
                                pszComponent,
                                nLength)) {
                    nLsn = ReadLe32(pRecord + 2);
                    nSize = ReadLe32(pRecord + 10);
                    bDirectory = (pRecord[25] & kDirectoryFlag) != 0;
                    bFound = true;
                    break;
                }
            }
        }
        if (!bFound) {
            return false;
        }
        pszComponent += nLength;
    }
    *pnLsn = nLsn;
    *pnSize = nSize;
    *pbDirectory = bDirectory;
    return true;
}

void Complete(int nFunction) {
    if (g_pfnCallback != nullptr) {
        g_pfnCallback(nFunction);
    }
}

DiscFile *FindDescriptor(int nFile) {
    if (nFile < 0 || nFile >= kFileCount || !g_aFiles[nFile].bOpen) {
        return nullptr;
    }
    return &g_aFiles[nFile];
}

unsigned char ToBcd(int nValue) {
    return static_cast<unsigned char>(((nValue / 10) << 4) | (nValue % 10));
}

} // namespace

extern "C" {

int sceCdInit(int) {
    Disc();
    return 1;
}

int sceCdMmode(int) {
    return 1;
}

int sceCdInitEeCB(int, void *, int) {
    return 1;
}

sceCdCBFunc sceCdCallback(sceCdCBFunc pfnCallback) {
    const sceCdCBFunc pfnPrevious = g_pfnCallback;
    g_pfnCallback = pfnCallback;
    return pfnPrevious;
}

int sceCdSync(int) {
    return 0;
}

int sceCdDiskReady(int) {
    return SCECdComplete;
}

int sceCdGetDiskType() {
    Disc();
    return SCECdPS2CD;
}

int sceCdGetError() {
    return g_nError;
}

int sceCdTrayReq(int, unsigned *pnTrayCount) {
    if (pnTrayCount != nullptr) {
        *pnTrayCount = 0;
    }
    return 1;
}

int sceCdStop() {
    Complete(SCECdFuncStop);
    return 1;
}

int sceCdSeek(unsigned) {
    Complete(SCECdFuncSeek);
    return 1;
}

int sceCdRead(unsigned nLsn, unsigned nSectors, void *pBuffer, sceCdRMode *) {
    // A read past the end of the disc reports an illegal address, as the drive does.
    g_nError = ReadSectors(nLsn, nSectors, pBuffer) ? 0 : 0x32;
    Complete(SCECdFuncRead);
    return 1;
}

int sceCdSearchFile(sceCdlFILE *pFile, const char *pszName) {
    unsigned nLsn;
    unsigned nSize;
    bool bDirectory;
    if (!FindFile(pszName, &nLsn, &nSize, &bDirectory)) {
        return 0;
    }
    pFile->lsn = nLsn;
    pFile->size = nSize;
    const char *pszBase = pszName + strlen(pszName);
    while (pszBase > pszName && pszBase[-1] != '\\' && pszBase[-1] != '/') {
        --pszBase;
    }
    snprintf(pFile->name, sizeof(pFile->name), "%s", pszBase);
    memset(pFile->date, 0, sizeof(pFile->date));
    pFile->flag = bDirectory ? kDirectoryFlag : 0;
    return 1;
}

// The console clock is the host's local time, so the time zone conversion has nothing to do.
int sceCdReadClock(sceCdCLOCK *pClock) {
    const time_t now = time(nullptr);
    tm local;
    localtime_r(&now, &local);
    pClock->stat = 0;
    pClock->second = ToBcd(local.tm_sec);
    pClock->minute = ToBcd(local.tm_min);
    pClock->hour = ToBcd(local.tm_hour);
    pClock->pad = 0;
    pClock->day = ToBcd(local.tm_mday);
    pClock->month = ToBcd(local.tm_mon + 1);
    pClock->year = ToBcd(local.tm_year % 100);
    return 1;
}

void sceScfGetLocalTimefromRTC(sceCdCLOCK *) {
}

// Only cdrom0: has files. host0: and the other devices are absent, as on a retail console.
int sceOpen(const char *pszPath, int nFlags, ...) {
    static const char kDisc[] = "cdrom0:";
    if (strncmp(pszPath, kDisc, sizeof(kDisc) - 1) != 0 || (nFlags & SCE_WRONLY) != 0) {
        return kNoEntry;
    }
    unsigned nLsn;
    unsigned nSize;
    bool bDirectory;
    if (!FindFile(pszPath + sizeof(kDisc) - 1, &nLsn, &nSize, &bDirectory) || bDirectory) {
        return kNoEntry;
    }
    for (int i = 1; i < kFileCount; ++i) {
        if (!g_aFiles[i].bOpen) {
            g_aFiles[i] = DiscFile{true, nLsn, nSize, 0};
            return i;
        }
    }
    return -24;
}

int sceClose(int nFile) {
    DiscFile *pFile = FindDescriptor(nFile);
    if (pFile == nullptr) {
        return kBadDescriptor;
    }
    pFile->bOpen = false;
    return 0;
}

int sceRead(int nFile, void *pBuffer, int nLength) {
    DiscFile *pFile = FindDescriptor(nFile);
    if (pFile == nullptr) {
        return kBadDescriptor;
    }
    if (nLength < 0) {
        return -22;
    }
    unsigned nCount = static_cast<unsigned>(nLength);
    if (pFile->nPosition >= pFile->nSize) {
        nCount = 0;
    } else if (nCount > pFile->nSize - pFile->nPosition) {
        nCount = pFile->nSize - pFile->nPosition;
    }
    const ssize_t nRead = pread(
        Disc(), pBuffer, nCount, static_cast<off_t>(pFile->nLsn) * kSectorSize + pFile->nPosition);
    if (nRead < 0) {
        return -5;
    }
    pFile->nPosition += static_cast<unsigned>(nRead);
    return static_cast<int>(nRead);
}

int sceWrite(int, const void *, int) {
    return kBadDescriptor;
}

int sceLseek(int nFile, int nOffset, int nOrigin) {
    DiscFile *pFile = FindDescriptor(nFile);
    if (pFile == nullptr) {
        return kBadDescriptor;
    }
    long long llBase = 0;
    if (nOrigin == SCE_SEEK_CUR) {
        llBase = pFile->nPosition;
    } else if (nOrigin == SCE_SEEK_END) {
        llBase = pFile->nSize;
    }
    const long long llPosition = llBase + nOffset;
    if (llPosition < 0) {
        return -22;
    }
    pFile->nPosition = static_cast<unsigned>(llPosition);
    return static_cast<int>(pFile->nPosition);
}

// The one request the game makes asks whether the file is busy, and it never is.
int sceIoctl(int, int, void *pArgument) {
    if (pArgument != nullptr) {
        *static_cast<int *>(pArgument) = 0;
    }
    return 0;
}

} // extern "C"
