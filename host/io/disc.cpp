// The disc drive and the file service over the disc image FREQ_DISC: libcdvd sector reads and
// cdrom0: files, both served from an ISO 9660 image of 2048-byte sectors.
//
// ponytail: a read finishes inside sceCdRead(), and the completion callback runs there too rather
// than on libcdvd's callback thread. The game polls for it either way. The streaming calls
// (sceCdSt*), which only the intro movie uses, read straight from the image at the stream's
// sector and never call back.
//
// Every folder under FREQ_SONGS is a song folder with files at their game paths (see
// host/io/songs.py). Each ark the folders change is served as a rebuilt one placed past the end of
// the image, made of new tables in the first 64 KiB, the disc's ark after them, and the folders'
// files. The image itself is never written.

#include <algorithm>
#include <ctype.h>
#include <fcntl.h>
#include <map>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <dirent.h>
#include <libcdvd.h>
#include <libscf.h>
#include <sifdev.h>

namespace {

constexpr int kSectorSize = 2048;
constexpr int kVolumeDescriptorSector = 16;
constexpr int kRootRecordOffset = 156;
constexpr int kDirectoryFlag = 2;
// A directory record's name follows its 33 fixed bytes, the last of which is the name's length.
constexpr int kRecordNameOffset = 33;
constexpr int kFileCount = 32;

// The kernel file service's error for a file that does not exist.
constexpr int kNoEntry = -2;
constexpr int kBadDescriptor = -9;

// The disc image, opened on first use. Every call that touches the disc goes through Disc().
int g_nDisc = -1;
sceCdCBFunc g_pfnCallback = nullptr;
int g_nError = 0;
// The next sector sceCdStRead() returns.
unsigned g_nStreamLsn = 0;

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

// One run of bytes of the served image: from the image (nFile -1), from rebuilt ark tables (nFile
// -2), or from a song folder's file. The first read of a folder's file reports log.
struct Piece {
    unsigned long long nAt;
    unsigned long long nLength;
    int nFile;
    unsigned long long nSource;
    std::string log;
    std::vector<unsigned char> tables;
};

struct ServedArk {
    unsigned nDiscLsn;
    unsigned nDiscSize;
    unsigned nLsn;
    unsigned nSize;
};

unsigned long long g_llImageSize = 0;
unsigned long long g_llServedEnd = 0;
std::vector<Piece> g_pieces;
std::vector<ServedArk> g_servedArks;

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
    struct stat status;
    if (g_nDisc < 0 ||
        pread(g_nDisc, abDescriptor, kSectorSize, kVolumeDescriptorSector * kSectorSize) !=
            kSectorSize ||
        memcmp(abDescriptor, "\1CD001", 6) != 0 || fstat(g_nDisc, &status) != 0) {
        fprintf(stderr, "DISC_ERROR %s is not an ISO 9660 disc image\n", pszPath);
        exit(2);
    }
    g_llImageSize = static_cast<unsigned long long>(status.st_size);
    g_llServedEnd = g_llImageSize;
    return g_nDisc;
}

// Reads the served image: the disc below its end, the pieces above it (zeros between them). A
// read past the end comes back short.
ssize_t ReadImage(void *pBuffer, unsigned long long llLength, unsigned long long llAt) {
    if (llAt + llLength <= g_llImageSize || llAt + llLength > g_llServedEnd) {
        return pread(Disc(), pBuffer, llLength, static_cast<off_t>(llAt));
    }
    unsigned char *pOut = static_cast<unsigned char *>(pBuffer);
    memset(pOut, 0, llLength);
    for (Piece &piece : g_pieces) {
        const unsigned long long llFirst = std::max(llAt, piece.nAt);
        const unsigned long long llEnd = std::min(llAt + llLength, piece.nAt + piece.nLength);
        if (llFirst >= llEnd) {
            continue;
        }
        unsigned char *pTo = pOut + (llFirst - llAt);
        const unsigned long long llFrom = piece.nSource + (llFirst - piece.nAt);
        if (piece.nFile == -2) {
            memcpy(pTo, piece.tables.data() + llFrom, llEnd - llFirst);
        } else if (pread(piece.nFile == -1 ? g_nDisc : piece.nFile,
                         pTo,
                         llEnd - llFirst,
                         static_cast<off_t>(llFrom)) != static_cast<ssize_t>(llEnd - llFirst)) {
            return -1;
        }
        if (piece.nFile >= 0) {
            static pthread_mutex_t s_logLock = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&s_logLock);
            if (!piece.log.empty()) {
                fprintf(stderr, "songs: read %s\n", piece.log.c_str());
                piece.log.clear();
            }
            pthread_mutex_unlock(&s_logLock);
        }
    }
    return static_cast<ssize_t>(llLength);
}

bool ReadSectors(unsigned nLsn, unsigned nCount, void *pBuffer) {
    const ssize_t nWanted = static_cast<ssize_t>(nCount) * kSectorSize;
    return ReadImage(pBuffer, nWanted, static_cast<unsigned long long>(nLsn) * kSectorSize) ==
           nWanted;
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
                if (i + kRecordNameOffset > kSectorSize ||
                    i + kRecordNameOffset + pRecord[32] > kSectorSize) {
                    break;
                }
                if (NameMatches(reinterpret_cast<const char *>(pRecord + kRecordNameOffset),
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

// With FREQ_LOG_MISSING=1, every disc file the game asks for and does not get is reported.
int Missing(const char *pszCall, const char *pszPath) {
    static const bool bLog = getenv("FREQ_LOG_MISSING") != nullptr;
    if (bLog) {
        fprintf(stderr, "MISSING %s %s\n", pszCall, pszPath);
    }
    return kNoEntry;
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

constexpr unsigned kArkChunk = 0x10000;
constexpr unsigned kArkHeaderSize = 0x100;
constexpr unsigned kArkPathOffset = 0x40;
constexpr unsigned kArkFileEntrySize = 24;
constexpr unsigned kArkDirectoryEntrySize = 8;
constexpr const char *kArkPaths[] = {
    "\\ARK\\ROOT.ARK", "\\ARK\\LEVELS.ARK", "\\ARK\\ARENAS.ARK", "\\ARK\\LOADING.ARK"};

void PutLe(unsigned char *pBytes, unsigned nValue, int nBytes) {
    for (int i = 0; i < nBytes; ++i) {
        pBytes[i] = static_cast<unsigned char>(nValue >> (8 * i));
    }
}

// The game's HashArkString().
unsigned ArkHash(const std::string &text) {
    unsigned nHash = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        nHash ^= static_cast<unsigned char>(text[i]) << (i & 7);
    }
    return nHash & 0xffff;
}

using ArkKey = std::pair<std::string, std::string>;

struct ArkFile {
    ArkKey key;
    unsigned long long llAt;
    unsigned nLength;
    unsigned nSize;
};

struct SongArk {
    unsigned nLsn;
    unsigned nSize;
    std::string mount;
    std::vector<unsigned char> header;
    std::vector<std::string> directories;
    std::vector<ArkFile> files;
    std::map<ArkKey, std::vector<Piece>> changed;
};

bool ReadArk(const char *pszPath, SongArk *pArk) {
    bool bDirectory;
    pArk->header.resize(kArkChunk);
    if (!FindFile(pszPath, &pArk->nLsn, &pArk->nSize, &bDirectory) ||
        !ReadSectors(pArk->nLsn, kArkChunk / kSectorSize, pArk->header.data())) {
        return false;
    }
    const unsigned char *pHeader = pArk->header.data();
    if (memcmp(pHeader, "ARK", 4) != 0 || ReadLe32(pHeader + 4) != 2 ||
        ReadLe32(pHeader + 36) != kSectorSize || ReadLe32(pHeader + 40) != 0 ||
        ReadLe32(pHeader + 32) > std::min(kArkChunk, pArk->nSize)) {
        return false;
    }
    auto text = [pHeader](unsigned nAt) {
        return std::string(reinterpret_cast<const char *>(pHeader + nAt));
    };
    std::string path = text(kArkPathOffset);
    std::transform(path.begin(), path.end(), path.begin(), tolower);
    const size_t nRun = path.find("/run");
    if (nRun == std::string::npos) {
        return false;
    }
    pArk->mount = path.substr(std::min(path.size(), nRun + 5));
    for (unsigned i = 0; i < ReadLe32(pHeader + 20); ++i) {
        pArk->directories.push_back(
            text(ReadLe32(pHeader + ReadLe32(pHeader + 16) + i * kArkDirectoryEntrySize + 4)));
    }
    for (unsigned i = 0; i < ReadLe32(pHeader + 12); ++i) {
        const unsigned char *pEntry = pHeader + ReadLe32(pHeader + 8) + i * kArkFileEntrySize;
        const unsigned nDirectory = pEntry[8] | (pEntry[9] << 8);
        if (nDirectory >= pArk->directories.size()) {
            return false;
        }
        pArk->files.push_back(
            ArkFile{{pArk->directories[nDirectory], text(ReadLe32(pEntry + 4))},
                    ReadLe32(pEntry + 12) * 1ULL * kSectorSize + (pEntry[10] | (pEntry[11] << 8)),
                    ReadLe32(pEntry + 16),
                    ReadLe32(pEntry + 20)});
    }
    return true;
}

// Collects a song folder's files as lowercased game paths, in name order.
void WalkFolder(const std::string &directory,
                const std::string &gamePath,
                std::vector<std::pair<std::string, std::string>> *pFiles) {
    DIR *pDirectory = opendir(directory.c_str());
    if (pDirectory == nullptr) {
        return;
    }
    std::vector<std::string> names;
    for (dirent *pEntry; (pEntry = readdir(pDirectory)) != nullptr;) {
        if (pEntry->d_name[0] != '.') {
            names.push_back(pEntry->d_name);
        }
    }
    closedir(pDirectory);
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
        const std::string path = directory + "/" + name;
        std::string lower = gamePath + (gamePath.empty() ? "" : "/") + name;
        std::transform(lower.begin(), lower.end(), lower.begin(), tolower);
        struct stat status;
        if (stat(path.c_str(), &status) != 0) {
            continue;
        }
        if (S_ISDIR(status.st_mode)) {
            WalkFolder(path, lower, pFiles);
        } else if (S_ISREG(status.st_mode)) {
            pFiles->emplace_back(lower, path);
        }
    }
}

// Serves the ark rebuilt with its changed files after g_llServedEnd, as new tables, the disc's ark
// 64 KiB on, and the changed files.
void ServeArk(SongArk &ark, const char *pszName) {
    std::vector<ArkFile> entries = ark.files;
    for (ArkFile &entry : entries) {
        entry.llAt += kArkChunk;
    }
    unsigned long long llDataAt =
        kArkChunk + (ark.nSize + kSectorSize - 1ULL) / kSectorSize * kSectorSize;
    std::vector<std::pair<unsigned long long, Piece>> data;
    int nNew = 0;
    for (auto &change : ark.changed) {
        const ArkKey &key = change.first;
        std::vector<Piece> &sources = change.second;
        unsigned long long llLength = 0;
        for (Piece &source : sources) {
            data.emplace_back(llDataAt + llLength, source);
            llLength += source.nLength;
        }
        unsigned nSize = static_cast<unsigned>(llLength);
        const Piece &last = sources.back();
        unsigned char abTail[4];
        if (key.second.size() > 3 && key.second.compare(key.second.size() - 3, 3, ".gz") == 0 &&
            last.nLength >= 4 &&
            pread(last.nFile == -1 ? g_nDisc : last.nFile,
                  abTail,
                  4,
                  static_cast<off_t>(last.nSource + last.nLength - 4)) == 4) {
            nSize = ReadLe32(abTail);
        }
        const ArkFile file{key, llDataAt, static_cast<unsigned>(llLength), nSize};
        auto it = std::find_if(entries.begin(), entries.end(), [&key](const ArkFile &entry) {
            return entry.key == key;
        });
        if (it != entries.end()) {
            *it = file;
        } else {
            entries.push_back(file);
            ++nNew;
        }
        llDataAt += (llLength + kSectorSize - 1) / kSectorSize * kSectorSize;
    }

    std::vector<std::string> directories = ark.directories;
    std::vector<std::string> added;
    for (const ArkFile &entry : entries) {
        if (std::find(directories.begin(), directories.end(), entry.key.first) ==
                directories.end() &&
            std::find(added.begin(), added.end(), entry.key.first) == added.end()) {
            added.push_back(entry.key.first);
        }
    }
    std::sort(added.begin(), added.end());
    directories.insert(directories.end(), added.begin(), added.end());

    const unsigned nFilesAt = kArkHeaderSize;
    const unsigned nDirectoriesAt = nFilesAt + kArkFileEntrySize * entries.size();
    const unsigned nStringsAt = nDirectoriesAt + kArkDirectoryEntrySize * directories.size();
    std::map<std::string, unsigned> stringAt;
    std::string strings;
    auto addString = [&](const std::string &text) {
        if (stringAt.emplace(text, nStringsAt + strings.size()).second) {
            strings.append(text.c_str(), text.size() + 1);
        }
    };
    for (const ArkFile &entry : entries) {
        addString(entry.key.second);
    }
    for (const std::string &directory : directories) {
        addString(directory);
    }
    const unsigned nEnd = nStringsAt + strings.size();
    if (nEnd > kArkChunk) {
        fprintf(stderr,
                "songs: %s needs %u bytes of tables, more than %u; not served\n",
                pszName,
                nEnd,
                kArkChunk);
        return;
    }

    Piece tables{0, kArkChunk, -2, 0, {}, std::vector<unsigned char>(kArkChunk)};
    unsigned char *pTables = tables.tables.data();
    memcpy(pTables, ark.header.data(), kArkHeaderSize);
    const unsigned anFields[] = {nFilesAt,
                                 static_cast<unsigned>(entries.size()),
                                 nDirectoriesAt,
                                 static_cast<unsigned>(directories.size()),
                                 nStringsAt,
                                 static_cast<unsigned>(entries.size() + directories.size()),
                                 nEnd};
    for (size_t i = 0; i < sizeof(anFields) / sizeof(anFields[0]); ++i) {
        PutLe(pTables + 8 + 4 * i, anFields[i], 4);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        unsigned char *pEntry = pTables + nFilesAt + i * kArkFileEntrySize;
        const ArkFile &entry = entries[i];
        PutLe(pEntry, ArkHash(entry.key.second), 2);
        PutLe(pEntry + 4, stringAt[entry.key.second], 4);
        PutLe(pEntry + 8,
              static_cast<unsigned>(
                  std::find(directories.begin(), directories.end(), entry.key.first) -
                  directories.begin()),
              2);
        PutLe(pEntry + 10, entry.llAt % kSectorSize, 2);
        PutLe(pEntry + 12, entry.llAt / kSectorSize, 4);
        PutLe(pEntry + 16, entry.nLength, 4);
        PutLe(pEntry + 20, entry.nSize, 4);
    }
    for (size_t i = 0; i < directories.size(); ++i) {
        unsigned char *pEntry = pTables + nDirectoriesAt + i * kArkDirectoryEntrySize;
        PutLe(pEntry, ArkHash(directories[i]), 2);
        PutLe(pEntry + 4, stringAt[directories[i]], 4);
    }
    memcpy(pTables + nStringsAt, strings.data(), strings.size());

    const unsigned long long llBase = (g_llServedEnd + kSectorSize - 1) / kSectorSize * kSectorSize;
    tables.nAt = llBase;
    g_pieces.push_back(std::move(tables));
    g_pieces.push_back(
        Piece{llBase + kArkChunk, ark.nSize, -1, ark.nLsn * 1ULL * kSectorSize, {}, {}});
    for (auto &placed : data) {
        placed.second.nAt = llBase + placed.first;
        g_pieces.push_back(placed.second);
    }
    // The ark ends on a whole 64 KiB chunk. The asynchronous loader reads whole chunks.
    const unsigned long long llSize = (llDataAt + kArkChunk - 1) / kArkChunk * kArkChunk;
    g_llServedEnd = llBase + llSize;
    g_servedArks.push_back(ServedArk{ark.nLsn,
                                     ark.nSize,
                                     static_cast<unsigned>(llBase / kSectorSize),
                                     static_cast<unsigned>(llSize)});
    fprintf(stderr,
            "songs: %s served with %zu files from the song folders (%d new)\n",
            pszName,
            ark.changed.size(),
            nNew);
}

// Each folder in FREQ_SONGS is a song folder (see host/io/songs.py). A file replaces the disc's
// file of its game path or adds one, and a file <path>.append is added to the end of <path>.
void ServeSongs() {
    const char *pszSongs = getenv("FREQ_SONGS");
    DIR *pDirectory = pszSongs != nullptr && *pszSongs != '\0' ? opendir(pszSongs) : nullptr;
    if (pDirectory == nullptr) {
        return;
    }
    closedir(pDirectory);
    Disc();
    std::vector<std::pair<std::string, std::string>> files;
    WalkFolder(pszSongs, "", &files);

    static const std::string kAppend = ".append";
    std::map<std::string, std::string> replace;
    std::map<std::string, std::vector<std::string>> append;
    std::map<std::string, bool> paths;
    for (const auto &file : files) {
        // The first component is the song folder's name.
        const size_t nSlash = file.first.find('/');
        if (nSlash == std::string::npos) {
            continue;
        }
        std::string inFolder = file.first.substr(nSlash + 1);
        if (inFolder.size() > kAppend.size() &&
            inFolder.compare(inFolder.size() - kAppend.size(), kAppend.size(), kAppend) == 0) {
            inFolder.resize(inFolder.size() - kAppend.size());
            append[inFolder].push_back(file.second);
        } else {
            replace[inFolder] = file.second;
        }
        paths[inFolder] = true;
    }

    SongArk arks[sizeof(kArkPaths) / sizeof(kArkPaths[0])];
    for (size_t i = 0; i < sizeof(kArkPaths) / sizeof(kArkPaths[0]); ++i) {
        if (!ReadArk(kArkPaths[i], &arks[i])) {
            fprintf(stderr,
                    "songs: %s is not an ark this loader knows; no songs served\n",
                    kArkPaths[i]);
            return;
        }
    }
    for (const auto &entry : paths) {
        const std::string &gamePath = entry.first;
        const size_t nSlash = gamePath.rfind('/');
        const std::string directory = nSlash == std::string::npos ? "" : gamePath.substr(0, nSlash);
        SongArk *pArk = nullptr;
        for (SongArk &ark : arks) {
            const bool bMatches = ark.mount.empty() || directory == ark.mount ||
                                  directory.compare(0, ark.mount.size() + 1, ark.mount + "/") == 0;
            if (bMatches && (pArk == nullptr || ark.mount.size() > pArk->mount.size())) {
                pArk = &ark;
            }
        }
        const std::string inArk = directory.substr(
            std::min(directory.size(), pArk->mount.size() + (pArk->mount.empty() ? 0 : 1)));
        const ArkKey key{inArk, gamePath.substr(nSlash + 1)};
        std::vector<std::string> sourcePaths;
        std::vector<Piece> sources;
        if (replace.count(gamePath) != 0) {
            sourcePaths.push_back(replace[gamePath]);
        } else {
            for (const ArkFile &file : pArk->files) {
                if (file.key == key) {
                    sources.push_back(Piece{
                        0, file.nLength, -1, pArk->nLsn * 1ULL * kSectorSize + file.llAt, {}, {}});
                }
            }
        }
        for (const std::string &path : append[gamePath]) {
            sourcePaths.push_back(path);
        }
        for (const std::string &path : sourcePaths) {
            const int nFile = open(path.c_str(), O_RDONLY | O_CLOEXEC);
            struct stat status;
            if (nFile < 0 || fstat(nFile, &status) != 0) {
                fprintf(stderr, "songs: cannot read %s\n", path.c_str());
                continue;
            }
            sources.push_back(Piece{0,
                                    static_cast<unsigned long long>(status.st_size),
                                    nFile,
                                    0,
                                    gamePath + " from " + path,
                                    {}});
        }
        if (!sources.empty()) {
            pArk->changed[key] = std::move(sources);
        }
    }

    g_pieces.push_back(Piece{0, g_llImageSize, -1, 0, {}, {}});
    for (size_t i = 0; i < sizeof(kArkPaths) / sizeof(kArkPaths[0]); ++i) {
        if (!arks[i].changed.empty()) {
            ServeArk(arks[i], kArkPaths[i]);
        }
    }
}

// FindFile(), with each ark the song folders change replaced by its rebuilt one.
bool FindServedFile(const char *pszPath, unsigned *pnLsn, unsigned *pnSize, bool *pbDirectory) {
    static const bool s_bServed = [] {
        ServeSongs();
        return true;
    }();
    (void)s_bServed;
    if (!FindFile(pszPath, pnLsn, pnSize, pbDirectory)) {
        return false;
    }
    for (const ServedArk &ark : g_servedArks) {
        if (*pnLsn == ark.nDiscLsn && *pnSize == ark.nDiscSize) {
            *pnLsn = ark.nLsn;
            *pnSize = ark.nSize;
        }
    }
    return true;
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

int sceCdStInit(unsigned, unsigned, unsigned) {
    return 1;
}

int sceCdStStart(unsigned nLsn, sceCdRMode *) {
    g_nStreamLsn = nLsn;
    return 1;
}

int sceCdStRead(unsigned nSectors, unsigned *pBuffer, unsigned, unsigned *pnError) {
    *pnError = ReadSectors(g_nStreamLsn, nSectors, pBuffer) ? 0 : 0x32;
    g_nStreamLsn += nSectors;
    return *pnError == 0 ? static_cast<int>(nSectors) : 0;
}

int sceCdStStop() {
    return 1;
}

int sceCdSearchFile(sceCdlFILE *pFile, const char *pszName) {
    unsigned nLsn;
    unsigned nSize;
    bool bDirectory;
    if (!FindServedFile(pszName, &nLsn, &nSize, &bDirectory)) {
        Missing("sceCdSearchFile", pszName);
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
        return Missing("sceOpen", pszPath);
    }
    unsigned nLsn;
    unsigned nSize;
    bool bDirectory;
    if (!FindServedFile(pszPath + sizeof(kDisc) - 1, &nLsn, &nSize, &bDirectory) || bDirectory) {
        return Missing("sceOpen", pszPath);
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
    const ssize_t nRead =
        ReadImage(pBuffer,
                  nCount,
                  static_cast<unsigned long long>(pFile->nLsn) * kSectorSize + pFile->nPosition);
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
