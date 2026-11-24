// The room: one UDP socket, a thread that serves it, and a thread that meets the other machines
// through the signalling server. See online.h.
//
// A datagram is a 24-byte header, up to HOST_ONLINE_MAX_MESSAGE bytes of payload, and a 16-byte
// tag (HMAC-SHA256 under a key the room code derives, cut short). The header holds "FQ", the
// protocol, the kind, the sender's session nonce (random each start, so a datagram of an earlier
// session is refused), a sequence number, the sender's cumulative acknowledgement, the sender's
// slot, the slot the message came from first (the host passes guests' messages on), and the
// payload's length, all big-endian.
//
// Meeting: each machine posts a signed record of its addresses (its LAN address and the one a
// STUN server saw) to an ntfy topic named after the room code, and reads the topic for the other
// side's. The host then sends probes to a guest's addresses and the guest sends hellos to the
// host's, which opens both NATs; a hello must echo the host's nonce (a probe tells it to a guest
// that has none), and the host answers it with a welcome and a slot. FREQ_ONLINE_PEER skips the
// meeting and sends hellos to one address.

#include "online.h"

#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <deque>
#include <fcntl.h>
#include <map>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <poll.h>

extern "C" void HostNotify(const char *pszMessage) __attribute__((weak)); // host/ps5/ps5.c

namespace {

constexpr int kMaxSlots = 4;
constexpr int kHeaderSize = 24;
constexpr int kTagSize = 16;
constexpr int kMaxDatagram = kHeaderSize + HOST_ONLINE_MAX_MESSAGE + kTagSize;
constexpr unsigned char kProtocol = 1;
constexpr int kDefaultHostPort = 47125;
constexpr int kMaxCandidates = 4;
constexpr double kPunchEveryMs = 250;
constexpr double kPunchForMs = 30000;
constexpr double kPingEveryMs = 1000;
constexpr double kLinkTimeoutMs = 15000;
constexpr double kMinResendMs = 100;
// ntfy.sh gives a visitor a burst of requests and then one every few seconds.
constexpr double kSignalEveryMs = 3000;
constexpr double kPublishEveryMs = 60000;
constexpr size_t kMaxUnacked = 512;
constexpr uint32_t kMaxEarly = 64;
constexpr size_t kMaxInbox = 8192;
constexpr size_t kMaxResponse = 256 * 1024;
constexpr const char *kDefaultSignal = "https://ntfy.sh";
constexpr const char *kDefaultStun = "stun.l.google.com:19302";

enum Kind : unsigned char {
    kProbe = 1,   // host to guest: the host's nonce, and a hole in the host's NAT
    kHello = 2,   // guest to host: payload is the host nonce it echoes
    kWelcome = 3, // host to guest: payload is the guest's nonce and its slot
    kData = 4,    // a reliable message
    kAck = 5,
    kPing = 6, // sequence number is the sender's clock in ms
    kPong = 7,
    kBye = 8, // the sender exited the room
};

struct Datagram {
    unsigned char nKind = 0;
    uint64_t nNonce = 0;
    uint32_t nSeq = 0;
    uint32_t nAck = 0;
    unsigned char nFrom = 0;
    unsigned char nOrigin = 0;
    std::string payload;
};

struct Outgoing {
    uint32_t nSeq;
    unsigned char nOrigin;
    std::string payload;
    double flSentMs;
};

struct Link {
    bool bUsed = false;
    sockaddr_in address{};
    uint64_t nNonce = 0;
    uint32_t nNextTx = 1;
    uint32_t nNextRx = 1;
    std::deque<Outgoing> unacked;
    std::map<uint32_t, std::pair<unsigned char, std::string>> early;
    double flHeardMs = 0;
    double flPingMs = 0;
    double flRttMs = 0; // the shortest round trip seen, 0 before the first
};

struct Inbound {
    int nFrom;
    std::string data;
};

struct Target {
    sockaddr_in address;
    double flUntilMs;
};

// What one record on the signalling topic says.
struct Record {
    bool bHost = false;
    uint64_t nNonce = 0;
    std::vector<sockaddr_in> candidates;
};

// The reasons a datagram is dropped, for the self-check.
struct Drops {
    int nSize = 0;
    int nTag = 0;
    int nSession = 0;
    int nDuplicate = 0;
};

struct Session {
    unsigned char abMacKey[32];
    unsigned char abSignalKey[32];
    std::string topic;
    std::string signal; // the ntfy server, or empty
    bool bHost = false;
    int nSocket = -1;
    uint64_t nNonce = 0;
    double flLossPercent = 0;

    // Shared with the game thread and the signalling thread, under lock.
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int nSlot = -1;
    bool bOpen = true;
    unsigned nLinked = 0;
    int anRttMs[kMaxSlots] = {};
    std::vector<std::string> outbox;
    std::deque<Inbound> inbox;
    std::string record;        // this machine's signed record, once its addresses are known
    std::vector<Record> found; // records the signalling thread read and the net thread takes
    bool bSignalWanted = true;
    int nSignalFailures = 0; // polls of the signalling server in a row that failed

    // HostOnlineClose() sets bStop; the net and signalling threads each have a reference, and the
    // last to finish deletes the session.
    std::atomic<bool> bStop{false};
    std::atomic<int> nReferences{2};
    pthread_t netThread;

    // The net thread's own.
    Link links[kMaxSlots];
    std::vector<Target> targets;
    uint64_t nHostNonce = 0; // a guest's view of the host's nonce, 0 until known
    Drops drops;
};

Session *g_pSession = nullptr;

double NowMs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * 1e3 + static_cast<double>(now.tv_nsec) / 1e6;
}

void Notify(const char *pszMessage) {
    if (HostNotify != nullptr) {
        HostNotify(pszMessage); // which logs it too
    } else {
        fprintf(stderr, "online: %s\n", pszMessage);
    }
}

void Hmac(const unsigned char *pKey, const void *pData, size_t nSize, unsigned char abOut[32]) {
    unsigned nLength = 32;
    HMAC(EVP_sha256(), pKey, 32, static_cast<const unsigned char *>(pData), nSize, abOut, &nLength);
}

std::string Hex(const unsigned char *pData, size_t nSize) {
    static const char kDigits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < nSize; ++i) {
        text += kDigits[pData[i] >> 4];
        text += kDigits[pData[i] & 15];
    }
    return text;
}

void Put(std::string &out, uint64_t nValue, int nBytes) {
    for (int i = nBytes - 1; i >= 0; --i) {
        out += static_cast<char>((nValue >> (8 * i)) & 0xff);
    }
}

uint64_t Get(const unsigned char *pData, int nBytes) {
    uint64_t nValue = 0;
    for (int i = 0; i < nBytes; ++i) {
        nValue = (nValue << 8) | pData[i];
    }
    return nValue;
}

// The keys and the topic name of a room code, which is case-blind and trimmed. The topic is a
// one-way function of the code, so reading the topic does not give the key.
void DeriveKeys(const char *pszRoom, Session *pSession) {
    std::string room(pszRoom);
    room.erase(0, room.find_first_not_of(" \t"));
    room.erase(room.find_last_not_of(" \t") + 1);
    std::transform(room.begin(), room.end(), room.begin(), [](unsigned char c) {
        return static_cast<char>(tolower(c));
    });
    // Stretched (PBKDF2, 200000 rounds, a fixed salt every machine shares): the topic name is
    // public, and a single hash of a short code could be guessed from it offline.
    static const char kSalt[] = "frequency-online-1";
    unsigned char abBase[32];
    PKCS5_PBKDF2_HMAC(room.data(),
                      static_cast<int>(room.size()),
                      reinterpret_cast<const unsigned char *>(kSalt),
                      sizeof(kSalt) - 1,
                      200000,
                      EVP_sha256(),
                      sizeof(abBase),
                      abBase);
    Hmac(abBase, "datagram", 8, pSession->abMacKey);
    Hmac(abBase, "signal", 6, pSession->abSignalKey);
    unsigned char abTopic[32];
    Hmac(abBase, "topic", 5, abTopic);
    pSession->topic = "freq" + Hex(abTopic, 12);
}

std::string Encode(const Session &session, const Datagram &datagram) {
    std::string bytes = "FQ";
    bytes += static_cast<char>(kProtocol);
    bytes += static_cast<char>(datagram.nKind);
    Put(bytes, datagram.nNonce, 8);
    Put(bytes, datagram.nSeq, 4);
    Put(bytes, datagram.nAck, 4);
    bytes += static_cast<char>(datagram.nFrom);
    bytes += static_cast<char>(datagram.nOrigin);
    Put(bytes, datagram.payload.size(), 2);
    bytes += datagram.payload;
    unsigned char abTag[32];
    Hmac(session.abMacKey, bytes.data(), bytes.size(), abTag);
    bytes.append(reinterpret_cast<const char *>(abTag), kTagSize);
    return bytes;
}

// The trust boundary for datagrams: the size, the header, and the tag.
bool Decode(Session &session, const unsigned char *pData, int nSize, Datagram *pOut) {
    if (nSize < kHeaderSize + kTagSize || nSize > kMaxDatagram || pData[0] != 'F' ||
        pData[1] != 'Q' || pData[2] != kProtocol ||
        static_cast<int>(Get(pData + 22, 2)) != nSize - kHeaderSize - kTagSize) {
        ++session.drops.nSize;
        return false;
    }
    unsigned char abTag[32];
    Hmac(session.abMacKey, pData, nSize - kTagSize, abTag);
    if (CRYPTO_memcmp(abTag, pData + nSize - kTagSize, kTagSize) != 0) {
        ++session.drops.nTag;
        return false;
    }
    pOut->nKind = pData[3];
    pOut->nNonce = Get(pData + 4, 8);
    pOut->nSeq = static_cast<uint32_t>(Get(pData + 12, 4));
    pOut->nAck = static_cast<uint32_t>(Get(pData + 16, 4));
    pOut->nFrom = pData[20];
    pOut->nOrigin = pData[21];
    pOut->payload.assign(reinterpret_cast<const char *>(pData) + kHeaderSize,
                         nSize - kHeaderSize - kTagSize);
    return true;
}

std::string AddressText(const sockaddr_in &address) {
    char szIp[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &address.sin_addr, szIp, sizeof(szIp));
    return std::string(szIp) + ":" + std::to_string(ntohs(address.sin_port));
}

bool ParseAddress(const std::string &text, sockaddr_in *pOut) {
    const size_t nColon = text.rfind(':');
    if (nColon == std::string::npos) {
        return false;
    }
    char *pszEnd = nullptr;
    const long nPort = strtol(text.c_str() + nColon + 1, &pszEnd, 10);
    memset(pOut, 0, sizeof(*pOut));
    pOut->sin_family = AF_INET;
    pOut->sin_port = htons(static_cast<uint16_t>(nPort));
    return *pszEnd == '\0' && nPort > 0 && nPort < 65536 &&
           inet_pton(AF_INET, text.substr(0, nColon).c_str(), &pOut->sin_addr) == 1;
}

// A host name or number with its port, looked up as IPv4.
bool Resolve(const std::string &text, sockaddr_in *pOut) {
    const size_t nColon = text.rfind(':');
    if (nColon == std::string::npos) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *pResult = nullptr;
    if (getaddrinfo(text.substr(0, nColon).c_str(), text.c_str() + nColon + 1, &hints, &pResult) !=
            0 ||
        pResult == nullptr) {
        return false;
    }
    memcpy(pOut, pResult->ai_addr, sizeof(*pOut));
    freeaddrinfo(pResult);
    return true;
}

bool SameAddress(const sockaddr_in &left, const sockaddr_in &right) {
    return left.sin_addr.s_addr == right.sin_addr.s_addr && left.sin_port == right.sin_port;
}

std::string SignRecord(const Session &session, const std::string &text) {
    unsigned char abTag[32];
    Hmac(session.abSignalKey, text.data(), text.size(), abTag);
    return text + " " + Hex(abTag, 16);
}

// The trust boundary for the public topic: a record of this room, from the other side, signed.
bool ParseRecord(const Session &session, const std::string &text, Record *pOut) {
    std::vector<std::string> words;
    for (size_t nAt = 0; nAt < text.size();) {
        const size_t nEnd = std::min(text.find(' ', nAt), text.size());
        words.push_back(text.substr(nAt, nEnd - nAt));
        nAt = nEnd + 1;
    }
    if (words.size() < 4 || words.size() > 4 + kMaxCandidates || words[0] != "FQ1" ||
        (words[1] != "H" && words[1] != "G") || words[2].size() != 16 ||
        words[2].find_first_not_of("0123456789abcdef") != std::string::npos) {
        return false;
    }
    const std::string body = text.substr(0, text.size() - words.back().size() - 1);
    if (SignRecord(session, body) != text) {
        return false;
    }
    pOut->bHost = words[1] == "H";
    pOut->nNonce = strtoull(words[2].c_str(), nullptr, 16);
    pOut->candidates.clear();
    for (size_t i = 3; i + 1 < words.size(); ++i) {
        sockaddr_in address;
        if (!ParseAddress(words[i], &address)) {
            return false;
        }
        pOut->candidates.push_back(address);
    }
    return true;
}

// STUN (RFC 5389): the address a binding response reports, XOR-MAPPED-ADDRESS or MAPPED-ADDRESS.
bool ParseStun(const unsigned char *pData,
               int nSize,
               const unsigned char abTransaction[12],
               sockaddr_in *pOut) {
    constexpr uint32_t kCookie = 0x2112a442;
    if (nSize < 20 || Get(pData, 2) != 0x0101 || Get(pData + 4, 4) != kCookie ||
        memcmp(pData + 8, abTransaction, 12) != 0 ||
        static_cast<int>(Get(pData + 2, 2)) + 20 > nSize) {
        return false;
    }
    const int nEnd = 20 + static_cast<int>(Get(pData + 2, 2));
    for (int nAt = 20; nAt + 4 <= nEnd;) {
        const int nType = static_cast<int>(Get(pData + nAt, 2));
        const int nLength = static_cast<int>(Get(pData + nAt + 2, 2));
        const unsigned char *pValue = pData + nAt + 4;
        if (nAt + 4 + nLength > nEnd) {
            return false;
        }
        if ((nType == 0x0020 || nType == 0x0001) && nLength == 8 && pValue[1] == 1) {
            const uint16_t nMask = nType == 0x0020 ? kCookie >> 16 : 0;
            const uint32_t nAddressMask = nType == 0x0020 ? kCookie : 0;
            memset(pOut, 0, sizeof(*pOut));
            pOut->sin_family = AF_INET;
            pOut->sin_port = htons(static_cast<uint16_t>(Get(pValue + 2, 2) ^ nMask));
            pOut->sin_addr.s_addr = htonl(static_cast<uint32_t>(Get(pValue + 4, 4)) ^ nAddressMask);
            return true;
        }
        nAt += 4 + ((nLength + 3) & ~3);
    }
    return false;
}

void SendRaw(Session &session, const sockaddr_in &address, const Datagram &datagram) {
    if (session.flLossPercent > 0 && rand() % 100 < session.flLossPercent) {
        return; // FREQ_ONLINE_TEST_LOSS
    }
    const std::string bytes = Encode(session, datagram);
    sendto(session.nSocket,
           bytes.data(),
           bytes.size(),
           0,
           reinterpret_cast<const sockaddr *>(&address),
           sizeof(address));
}

int MySlotByte(Session &session) {
    pthread_mutex_lock(&session.lock);
    const int nSlot = session.nSlot;
    pthread_mutex_unlock(&session.lock);
    return nSlot < 0 ? 0xff : nSlot;
}

void SendOnLink(Session &session,
                Link &link,
                unsigned char nKind,
                uint32_t nSeq,
                unsigned char nOrigin,
                const std::string &payload) {
    Datagram datagram;
    datagram.nKind = nKind;
    datagram.nNonce = session.nNonce;
    datagram.nSeq = nSeq;
    datagram.nAck = link.nNextRx;
    datagram.nFrom = static_cast<unsigned char>(MySlotByte(session));
    datagram.nOrigin = nOrigin;
    datagram.payload = payload;
    SendRaw(session, link.address, datagram);
}

void PublishLinks(Session &session) {
    unsigned nLinked = 0;
    for (int i = 0; i < kMaxSlots; ++i) {
        if (session.links[i].bUsed) {
            nLinked |= 1u << i;
        }
    }
    pthread_mutex_lock(&session.lock);
    session.nLinked = nLinked;
    for (int i = 0; i < kMaxSlots; ++i) {
        session.anRttMs[i] = static_cast<int>(session.links[i].flRttMs);
    }
    const int nFree = kMaxSlots - 1 - __builtin_popcount(nLinked);
    session.bSignalWanted = session.bHost ? (session.bOpen && nFree > 0) : nLinked == 0;
    pthread_mutex_unlock(&session.lock);
}

void Queue(Session &session, Link &link, unsigned char nOrigin, const std::string &payload) {
    const uint32_t nSeq = link.nNextTx++;
    link.unacked.push_back(Outgoing{nSeq, nOrigin, payload, NowMs()});
    SendOnLink(session, link, kData, nSeq, nOrigin, payload);
}

void AddTarget(Session &session, const sockaddr_in &address) {
    for (Target &target : session.targets) {
        if (SameAddress(target.address, address)) {
            target.flUntilMs = std::max(target.flUntilMs, NowMs() + kPunchForMs);
            return;
        }
    }
    session.targets.push_back(Target{address, NowMs() + kPunchForMs});
}

void DropLink(Session &session, int nSlot, const char *pszWhy) {
    if (!session.bHost) {
        AddTarget(session, session.links[nSlot].address);
    }
    session.links[nSlot] = Link();
    char szMessage[128];
    if (session.bHost) {
        snprintf(szMessage,
                 sizeof(szMessage),
                 "Frequency online: player %d left the room (%s)",
                 nSlot + 1,
                 pszWhy);
    } else {
        pthread_mutex_lock(&session.lock);
        session.nSlot = -1;
        pthread_mutex_unlock(&session.lock);
        session.nHostNonce = 0;
        snprintf(szMessage, sizeof(szMessage), "Frequency online: lost the host (%s)", pszWhy);
    }
    Notify(szMessage);
    PublishLinks(session);
}

void Deliver(Session &session, int nOrigin, const std::string &payload) {
    pthread_mutex_lock(&session.lock);
    if (session.inbox.size() < kMaxInbox) {
        session.inbox.push_back(Inbound{nOrigin, payload});
    }
    pthread_mutex_unlock(&session.lock);
    if (session.bHost) { // pass a guest's message on to the other guests
        for (int i = 1; i < kMaxSlots; ++i) {
            if (i != nOrigin && session.links[i].bUsed) {
                Queue(session, session.links[i], static_cast<unsigned char>(nOrigin), payload);
            }
        }
    }
}

void OnHello(Session &session, const Datagram &datagram, const sockaddr_in &from) {
    if (datagram.payload.size() != 8 ||
        Get(reinterpret_cast<const unsigned char *>(datagram.payload.data()), 8) !=
            session.nNonce) {
        Datagram probe;
        probe.nKind = kProbe;
        probe.nNonce = session.nNonce;
        SendRaw(session, from, probe);
        return;
    }
    int nSlot = -1;
    for (int i = 1; i < kMaxSlots; ++i) {
        if (session.links[i].bUsed && session.links[i].nNonce == datagram.nNonce) {
            nSlot = i;
        }
    }
    pthread_mutex_lock(&session.lock);
    const bool bOpen = session.bOpen;
    pthread_mutex_unlock(&session.lock);
    for (int i = 1; i < kMaxSlots && nSlot < 0 && bOpen; ++i) {
        if (!session.links[i].bUsed) {
            nSlot = i;
            session.links[i] = Link();
            session.links[i].bUsed = true;
            session.links[i].address = from;
            session.links[i].nNonce = datagram.nNonce;
            session.links[i].flHeardMs = NowMs();
            char szMessage[96];
            snprintf(szMessage,
                     sizeof(szMessage),
                     "Frequency online: player %d joined from %s",
                     i + 1,
                     AddressText(from).c_str());
            Notify(szMessage);
        }
    }
    if (nSlot < 0) {
        return; // full, or a song is playing
    }
    std::string payload;
    Put(payload, datagram.nNonce, 8);
    payload += static_cast<char>(nSlot);
    SendOnLink(session, session.links[nSlot], kWelcome, 0, 0, payload);
    PublishLinks(session);
}

void OnWelcome(Session &session, const Datagram &datagram, const sockaddr_in &from) {
    const unsigned char *pPayload =
        reinterpret_cast<const unsigned char *>(datagram.payload.data());
    if (session.links[0].bUsed || datagram.payload.size() != 9 ||
        Get(pPayload, 8) != session.nNonce || pPayload[8] < 1 || pPayload[8] >= kMaxSlots ||
        (session.nHostNonce != 0 && datagram.nNonce != session.nHostNonce)) {
        return;
    }
    Link &link = session.links[0];
    link = Link();
    link.bUsed = true;
    link.address = from;
    link.nNonce = datagram.nNonce;
    link.flHeardMs = NowMs();
    session.nHostNonce = datagram.nNonce;
    pthread_mutex_lock(&session.lock);
    session.nSlot = pPayload[8];
    pthread_mutex_unlock(&session.lock);
    char szMessage[96];
    snprintf(szMessage,
             sizeof(szMessage),
             "Frequency online: joined the room as player %d",
             pPayload[8] + 1);
    Notify(szMessage);
    PublishLinks(session);
}

void OnLinked(Session &session, const Datagram &datagram, const sockaddr_in &from) {
    // A link keeps the address it was made with, so a copied datagram sent from elsewhere cannot
    // move it. A NAT that gives the peer a new port ends the link, and the guest links again.
    if (datagram.nFrom >= kMaxSlots || !session.links[datagram.nFrom].bUsed ||
        session.links[datagram.nFrom].nNonce != datagram.nNonce ||
        !SameAddress(session.links[datagram.nFrom].address, from)) {
        ++session.drops.nSession;
        return;
    }
    if (datagram.nKind == kBye) {
        DropLink(session, datagram.nFrom, "closed");
        return;
    }
    Link &link = session.links[datagram.nFrom];
    const double flNow = NowMs();
    link.flHeardMs = flNow;
    while (!link.unacked.empty() && link.unacked.front().nSeq < datagram.nAck) {
        link.unacked.pop_front();
    }
    if (datagram.nKind == kPing) {
        SendOnLink(session, link, kPong, datagram.nSeq, 0, std::string());
    } else if (datagram.nKind == kPong) {
        const double flRtt = static_cast<uint32_t>(static_cast<uint32_t>(flNow) - datagram.nSeq);
        if (flRtt < 10000 && (link.flRttMs == 0 || flRtt < link.flRttMs)) {
            link.flRttMs = std::max(1.0, flRtt);
            PublishLinks(session);
        }
    } else if (datagram.nKind == kData) {
        // A guest speaks only for itself; the host speaks for anyone.
        const int nOrigin = datagram.nFrom == 0 ? datagram.nOrigin : datagram.nFrom;
        if (nOrigin >= kMaxSlots) {
            return;
        }
        if (datagram.nSeq == link.nNextRx) {
            ++link.nNextRx;
            Deliver(session, nOrigin, datagram.payload);
            for (auto it = link.early.find(link.nNextRx); it != link.early.end();
                 it = link.early.find(link.nNextRx)) {
                ++link.nNextRx;
                const int nEarlyOrigin = datagram.nFrom == 0 ? it->second.first : datagram.nFrom;
                const std::string payload = it->second.second;
                link.early.erase(it);
                Deliver(session, nEarlyOrigin, payload);
            }
        } else if (datagram.nSeq > link.nNextRx && datagram.nSeq - link.nNextRx < kMaxEarly) {
            link.early[datagram.nSeq] = {datagram.nOrigin, datagram.payload};
        } else {
            ++session.drops.nDuplicate;
        }
        SendOnLink(session, link, kAck, 0, 0, std::string());
    }
}

void OnDatagram(Session &session, const unsigned char *pData, int nSize, const sockaddr_in &from) {
    Datagram datagram;
    if (!Decode(session, pData, nSize, &datagram)) {
        return;
    }
    if (datagram.nKind == kHello && session.bHost) {
        OnHello(session, datagram, from);
    } else if (datagram.nKind == kProbe && !session.bHost && !session.links[0].bUsed) {
        session.nHostNonce = datagram.nNonce;
        AddTarget(session, from);
    } else if (datagram.nKind == kWelcome && !session.bHost) {
        OnWelcome(session, datagram, from);
    } else if (datagram.nKind >= kData && datagram.nKind <= kBye) {
        OnLinked(session, datagram, from);
    }
}

void Tick(Session &session, double &flLastPunch) {
    const double flNow = NowMs();
    std::vector<std::string> outbox;
    std::vector<Record> found;
    pthread_mutex_lock(&session.lock);
    outbox.swap(session.outbox);
    found.swap(session.found);
    pthread_mutex_unlock(&session.lock);

    const unsigned char nOrigin = static_cast<unsigned char>(MySlotByte(session));
    for (const std::string &message : outbox) {
        for (Link &link : session.links) {
            if (link.bUsed) {
                Queue(session, link, nOrigin, message);
            }
        }
    }
    for (const Record &record : found) {
        if (record.bHost == session.bHost) {
            continue;
        }
        if (!session.bHost) {
            session.nHostNonce = record.nNonce;
        }
        for (const sockaddr_in &address : record.candidates) {
            AddTarget(session, address);
        }
    }

    for (int i = 0; i < kMaxSlots; ++i) {
        Link &link = session.links[i];
        if (!link.bUsed) {
            continue;
        }
        if (flNow - link.flHeardMs > kLinkTimeoutMs) {
            DropLink(session, i, "no answer");
            continue;
        }
        if (link.unacked.size() > kMaxUnacked) {
            DropLink(session, i, "too far behind");
            continue;
        }
        const double flResendMs = std::max(kMinResendMs, 2 * link.flRttMs);
        int nBudget = 64;
        for (Outgoing &outgoing : link.unacked) {
            if (nBudget-- == 0) {
                break;
            }
            if (flNow - outgoing.flSentMs > flResendMs) {
                outgoing.flSentMs = flNow;
                SendOnLink(session, link, kData, outgoing.nSeq, outgoing.nOrigin, outgoing.payload);
            }
        }
        if (flNow - link.flPingMs > kPingEveryMs) {
            link.flPingMs = flNow;
            SendOnLink(session, link, kPing, static_cast<uint32_t>(flNow), 0, std::string());
        }
    }

    if (flNow - flLastPunch < kPunchEveryMs) {
        return;
    }
    flLastPunch = flNow;
    session.targets.erase(
        std::remove_if(session.targets.begin(),
                       session.targets.end(),
                       [flNow](const Target &target) { return target.flUntilMs < flNow; }),
        session.targets.end());
    if (session.bHost || !session.links[0].bUsed) {
        Datagram datagram;
        datagram.nKind = session.bHost ? kProbe : kHello;
        datagram.nNonce = session.nNonce;
        datagram.nFrom = 0xff;
        if (!session.bHost) {
            Put(datagram.payload, session.nHostNonce, 8);
        }
        for (const Target &target : session.targets) {
            bool bLinked = false;
            for (const Link &link : session.links) {
                bLinked |= link.bUsed && SameAddress(link.address, target.address);
            }
            if (!bLinked) {
                SendRaw(session, target.address, datagram);
            }
        }
    }
}

// This machine's address on its LAN: the one a UDP socket "connected" outward would use (no
// datagram is sent).
bool LocalAddress(sockaddr_in *pOut) {
    const int nSocket = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in outward{};
    outward.sin_family = AF_INET;
    outward.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &outward.sin_addr);
    socklen_t nLength = sizeof(*pOut);
    const bool bFound =
        nSocket >= 0 &&
        connect(nSocket, reinterpret_cast<sockaddr *>(&outward), sizeof(outward)) == 0 &&
        getsockname(nSocket, reinterpret_cast<sockaddr *>(pOut), &nLength) == 0 &&
        pOut->sin_addr.s_addr != 0;
    if (nSocket >= 0) {
        close(nSocket);
    }
    return bFound;
}

bool StunAddress(Session &session, sockaddr_in *pOut) {
    const char *pszStun = getenv("FREQ_ONLINE_STUN");
    const std::string stun = pszStun != nullptr ? pszStun : kDefaultStun;
    sockaddr_in server;
    if (stun == "off" || !Resolve(stun, &server)) {
        fprintf(stderr, "online: no STUN server (%s)\n", stun.c_str());
        return false;
    }
    unsigned char abRequest[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xa4, 0x42};
    RAND_bytes(abRequest + 8, 12);
    for (int nTry = 0; nTry < 3 && !session.bStop; ++nTry) {
        sendto(session.nSocket,
               abRequest,
               sizeof(abRequest),
               0,
               reinterpret_cast<const sockaddr *>(&server),
               sizeof(server));
        pollfd wait = {session.nSocket, POLLIN, 0};
        const double flUntil = NowMs() + 700;
        while (poll(&wait, 1, std::max(1, static_cast<int>(flUntil - NowMs()))) > 0) {
            unsigned char abReply[kMaxDatagram];
            const ssize_t nSize = recv(session.nSocket, abReply, sizeof(abReply), MSG_DONTWAIT);
            if (nSize > 0 && ParseStun(abReply, static_cast<int>(nSize), abRequest + 8, pOut)) {
                return true;
            }
        }
    }
    fprintf(stderr, "online: STUN server %s did not answer\n", stun.c_str());
    return false;
}

size_t ToString(void *pData, size_t nSize, size_t nCount, void *pUser) {
    std::string *pOut = static_cast<std::string *>(pUser);
    if (pOut->size() + nSize * nCount > kMaxResponse) {
        return 0;
    }
    pOut->append(static_cast<const char *>(pData), nSize * nCount);
    return nSize * nCount;
}

// One HTTPS request to the signalling server: a POST of pBody, or a GET. Returns the HTTP status,
// or 0.
long Http(const std::string &url, const std::string *pBody, std::string *pOut) {
    CURL *pCurl = curl_easy_init();
    if (pCurl == nullptr) {
        return 0;
    }
    curl_slist *pResolve = nullptr;
#ifdef __PROSPERO__
    // curl's own resolver fails on the console (host/ps5/ps5.c); getaddrinfo there asks the
    // console's.
    char szHost[256] = "";
    sscanf(url.c_str(), "https://%255[^/:]", szHost);
    sockaddr_in address;
    if (Resolve(std::string(szHost) + ":443", &address)) {
        char szIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &address.sin_addr, szIp, sizeof(szIp));
        pResolve = curl_slist_append(nullptr, (std::string(szHost) + ":443:" + szIp).c_str());
    }
    curl_easy_setopt(pCurl, CURLOPT_RESOLVE, pResolve);
    curl_easy_setopt(pCurl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(pCurl, CURLOPT_CAINFO, "/app0/cacert.pem");
#endif
    const char *pszCa = getenv("FREQ_ONLINE_CA");
    if (pszCa != nullptr && pszCa[0] != '\0') {
        curl_easy_setopt(pCurl, CURLOPT_CAINFO, pszCa);
    }
    curl_easy_setopt(pCurl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(pCurl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(pCurl, CURLOPT_USERAGENT, "Frequency-online/1");
    curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(pCurl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, ToString);
    curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, pOut);
    if (pBody != nullptr) {
        curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, pBody->c_str());
    }
    long nStatus = 0;
    const CURLcode result = curl_easy_perform(pCurl);
    curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &nStatus);
    if (result != CURLE_OK) {
        fprintf(stderr, "online: signal server: %s\n", curl_easy_strerror(result));
        nStatus = 0;
    }
    curl_easy_cleanup(pCurl);
    curl_slist_free_all(pResolve);
    return nStatus;
}

// The records in an ntfy poll: one JSON object a line, the record in its "message" string.
std::vector<std::string> Messages(const std::string &response) {
    std::vector<std::string> messages;
    static const char kKey[] = "\"message\":\"";
    for (size_t nAt = response.find(kKey); nAt != std::string::npos;
         nAt = response.find(kKey, nAt)) {
        nAt += sizeof(kKey) - 1;
        const size_t nEnd = response.find('"', nAt);
        if (nEnd == std::string::npos) {
            break;
        }
        messages.push_back(response.substr(nAt, nEnd - nAt));
        nAt = nEnd;
    }
    return messages;
}

void Release(Session *pSession) {
    if (--pSession->nReferences == 0) {
        delete pSession;
    }
}

void *RunSignal(void *pArgument) {
    Session &session = *static_cast<Session *>(pArgument);
    double flPublished = -kPublishEveryMs;
    double flWaitMs = kSignalEveryMs;
    std::vector<uint64_t> seen;
    while (!session.bStop) {
        for (double flSlept = 0; flSlept < flWaitMs && !session.bStop; flSlept += 100) {
            usleep(100 * 1000);
        }
        pthread_mutex_lock(&session.lock);
        const std::string record = session.record;
        const bool bWanted = session.bSignalWanted;
        pthread_mutex_unlock(&session.lock);
        if (record.empty() || !bWanted) {
            continue;
        }
        const std::string url = session.signal + "/" + session.topic;
        std::string response;
        if (NowMs() - flPublished > kPublishEveryMs) {
            const long nStatus = Http(url, &record, &response);
            if (nStatus == 200) {
                flPublished = NowMs();
            } else {
                fprintf(stderr, "online: signal server would not take the record (%ld)\n", nStatus);
            }
            response.clear();
        }
        const long nStatus = Http(url + "/json?poll=1&since=120s", nullptr, &response);
        flWaitMs = nStatus == 200 ? kSignalEveryMs : std::min(flWaitMs * 2, 30000.0);
        std::vector<Record> found;
        for (const std::string &message : Messages(response)) {
            Record parsed;
            if (ParseRecord(session, message, &parsed) && parsed.bHost != session.bHost &&
                std::find(seen.begin(), seen.end(), parsed.nNonce) == seen.end()) {
                seen.push_back(parsed.nNonce);
                found.push_back(parsed);
                fprintf(stderr, "online: found the %s's record\n", parsed.bHost ? "host" : "guest");
            }
        }
        pthread_mutex_lock(&session.lock);
        session.found.insert(session.found.end(), found.begin(), found.end());
        session.nSignalFailures = nStatus == 200 ? 0 : session.nSignalFailures + 1;
        pthread_mutex_unlock(&session.lock);
    }
    Release(&session);
    return nullptr;
}

void *RunNet(void *pArgument) {
    Session &session = *static_cast<Session *>(pArgument);
    sockaddr_in bound{};
    socklen_t nLength = sizeof(bound);
    getsockname(session.nSocket, reinterpret_cast<sockaddr *>(&bound), &nLength);
    std::string text = std::string("FQ1 ") + (session.bHost ? "H " : "G ") +
                       Hex(reinterpret_cast<const unsigned char *>(&session.nNonce), 8);
    sockaddr_in address;
    if (LocalAddress(&address)) {
        address.sin_port = bound.sin_port;
        text += " " + AddressText(address);
    }
    sockaddr_in mapped;
    if (StunAddress(session, &mapped) && !SameAddress(mapped, address)) {
        text += " " + AddressText(mapped);
    }
    fprintf(stderr, "online: this machine's addresses:%s\n", text.c_str() + 22);
    pthread_mutex_lock(&session.lock);
    session.record = SignRecord(session, text);
    pthread_mutex_unlock(&session.lock);

    double flLastPunch = 0;
    while (!session.bStop) {
        pollfd wait = {session.nSocket, POLLIN, 0};
        poll(&wait, 1, 10);
        for (;;) {
            unsigned char abData[kMaxDatagram + 1];
            sockaddr_in from;
            socklen_t nFromLength = sizeof(from);
            const ssize_t nSize = recvfrom(session.nSocket,
                                           abData,
                                           sizeof(abData),
                                           MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr *>(&from),
                                           &nFromLength);
            if (nSize < 0) {
                break;
            }
            OnDatagram(session, abData, static_cast<int>(nSize), from);
        }
        Tick(session, flLastPunch);
    }
    for (int nTry = 0; nTry < 3; ++nTry) { // three, as a datagram can be lost
        for (Link &link : session.links) {
            if (link.bUsed) {
                SendOnLink(session, link, kBye, 0, 0, std::string());
            }
        }
    }
    close(session.nSocket);
    Release(&session);
    return nullptr;
}

// Opens the socket and sets up the session of a room from the settings, or returns null.
Session *NewSession(const char *pszRoom, bool bHost) {
    Session *pSession = new Session;
    Session &session = *pSession;
    DeriveKeys(pszRoom, pSession);
    session.bHost = bHost;
    session.nSlot = session.bHost ? 0 : -1;
    RAND_bytes(reinterpret_cast<unsigned char *>(&session.nNonce), sizeof(session.nNonce));
    if (getenv("FREQ_ONLINE_TEST_LOSS") != nullptr) {
        session.flLossPercent = atof(getenv("FREQ_ONLINE_TEST_LOSS"));
    }
    const char *pszSignal = getenv("FREQ_ONLINE_SIGNAL");
    session.signal = pszSignal != nullptr ? pszSignal : kDefaultSignal;
    if (session.signal == "off" || session.signal.compare(0, 8, "https://") != 0) {
        session.signal.clear();
    }
    while (!session.signal.empty() && session.signal.back() == '/') {
        session.signal.pop_back();
    }

    const char *pszPort = getenv("FREQ_ONLINE_PORT");
    const int nPort = pszPort != nullptr ? atoi(pszPort) : (session.bHost ? kDefaultHostPort : 0);
    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(static_cast<uint16_t>(nPort));
    session.nSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (session.nSocket < 0 ||
        bind(session.nSocket, reinterpret_cast<sockaddr *>(&bind_address), sizeof(bind_address)) !=
            0) {
        fprintf(stderr, "online: cannot open UDP port %d\n", nPort);
        if (session.nSocket >= 0) {
            close(session.nSocket);
        }
        delete pSession;
        return nullptr;
    }
    const char *pszPeer = getenv("FREQ_ONLINE_PEER");
    sockaddr_in peer;
    if (!session.bHost && pszPeer != nullptr && pszPeer[0] != '\0') {
        if (Resolve(pszPeer, &peer)) {
            session.targets.push_back(Target{peer, 1e300});
        } else {
            fprintf(stderr, "online: FREQ_ONLINE_PEER %s is not an address\n", pszPeer);
        }
    }
    fprintf(stderr,
            "online: room %s as the %s, UDP port %d, signal server %s\n",
            session.topic.c_str(),
            session.bHost ? "host" : "guest",
            nPort,
            session.signal.empty() ? "off" : session.signal.c_str());
    return pSession;
}

} // namespace

extern "C" {

void HostOnlineStart(void) {
    const char *pszRoom = getenv("FREQ_ONLINE_ROOM");
    const char *pszHost = getenv("FREQ_ONLINE_HOST");
    if (pszRoom != nullptr && pszRoom[0] != '\0') {
        HostOnlineOpen(pszRoom, pszHost != nullptr && atoi(pszHost) != 0);
    }
}

int HostOnlineOpen(const char *pszRoom, int bHost) {
    HostOnlineClose();
    Session *pSession = NewSession(pszRoom, bHost != 0);
    if (pSession == nullptr) {
        return 0;
    }
    static pthread_once_t s_curlOnce = PTHREAD_ONCE_INIT;
    pthread_once(&s_curlOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (pthread_create(&pSession->netThread, nullptr, RunNet, pSession) != 0) {
        fprintf(stderr, "online: cannot start the network thread\n");
        close(pSession->nSocket);
        delete pSession;
        return 0;
    }
    pthread_t thread;
    if (!pSession->signal.empty() && pthread_create(&thread, nullptr, RunSignal, pSession) == 0) {
        pthread_detach(thread);
    } else {
        --pSession->nReferences;
    }
    g_pSession = pSession;
    Notify(pSession->bHost ? "Frequency online: room open, waiting for players" :
                             "Frequency online: looking for the host");
    return 1;
}

void HostOnlineClose(void) {
    Session *pSession = g_pSession;
    if (pSession == nullptr) {
        return;
    }
    g_pSession = nullptr;
    pSession->bStop = true;
    pthread_join(pSession->netThread, nullptr); // the socket is closed, its port free again
    Notify("Frequency online: left the room");
}

int HostOnlineActive(void) {
    return g_pSession != nullptr;
}

int HostOnlineSignalFailing(void) {
    if (g_pSession == nullptr) {
        return 0;
    }
    pthread_mutex_lock(&g_pSession->lock);
    const int nFailures = g_pSession->nSignalFailures;
    pthread_mutex_unlock(&g_pSession->lock);
    return nFailures >= 2;
}

int HostOnlineSlot(void) {
    if (g_pSession == nullptr) {
        return -1;
    }
    pthread_mutex_lock(&g_pSession->lock);
    const int nSlot = g_pSession->nSlot;
    pthread_mutex_unlock(&g_pSession->lock);
    return nSlot;
}

unsigned HostOnlineLinked(void) {
    if (g_pSession == nullptr) {
        return 0;
    }
    pthread_mutex_lock(&g_pSession->lock);
    const unsigned nLinked = g_pSession->nLinked;
    pthread_mutex_unlock(&g_pSession->lock);
    return nLinked;
}

int HostOnlineRttMs(int nSlot) {
    if (g_pSession == nullptr || nSlot < 0 || nSlot >= kMaxSlots) {
        return 0;
    }
    pthread_mutex_lock(&g_pSession->lock);
    const int nRtt = g_pSession->anRttMs[nSlot];
    pthread_mutex_unlock(&g_pSession->lock);
    return nRtt;
}

void HostOnlineSetOpen(int bOpen) {
    if (g_pSession != nullptr) {
        pthread_mutex_lock(&g_pSession->lock);
        g_pSession->bOpen = bOpen != 0;
        pthread_mutex_unlock(&g_pSession->lock);
    }
}

int HostOnlineSend(const void *pData, int nSize) {
    if (g_pSession == nullptr || nSize <= 0 || nSize > HOST_ONLINE_MAX_MESSAGE) {
        return 0;
    }
    pthread_mutex_lock(&g_pSession->lock);
    g_pSession->outbox.emplace_back(static_cast<const char *>(pData), nSize);
    pthread_mutex_unlock(&g_pSession->lock);
    return 1;
}

int HostOnlineReceive(void *pBuffer, int nCapacity, int *pnFrom) {
    for (;;) {
        if (g_pSession == nullptr) {
            return 0;
        }
        pthread_mutex_lock(&g_pSession->lock);
        if (g_pSession->inbox.empty()) {
            pthread_mutex_unlock(&g_pSession->lock);
            return 0;
        }
        const Inbound inbound = g_pSession->inbox.front();
        g_pSession->inbox.pop_front();
        pthread_mutex_unlock(&g_pSession->lock);
        if (static_cast<int>(inbound.data.size()) <= nCapacity) {
            memcpy(pBuffer, inbound.data.data(), inbound.data.size());
            *pnFrom = inbound.nFrom;
            return static_cast<int>(inbound.data.size());
        }
    }
}

} // extern "C"

// ---- the friends board (FREQ_BOARD) ----
//
// Each machine keeps one record of its player's best scores on the ntfy topic of the board code:
// "FB1 <name> <level>:<difficulty>:<score>... <tag>", signed as a room's records are, so only a
// machine given the code can post. Anyone with the code can post any name and any score: it is a
// board among friends, not a proof. ntfy.sh keeps a message for 12 hours, so each record seen is
// kept in board.txt in the title folder too, and a machine posts its record again after 6 hours.

std::string HostTitleFile(const char *pszName); // host/core/progress.cpp
extern "C" void HostAchievement(const char *pszId);

namespace {

// ntfy.sh takes about 250 messages a day from one address: at most one post every 6 minutes.
constexpr double kBoardPostGapMs = 6 * 60 * 1000.0;
constexpr long long kBoardRepostSeconds = 6 * 3600;
constexpr size_t kBoardMaxEntries = 120;
constexpr size_t kBoardMaxName = 16;

struct Board {
    Session keys; // the code's signing key and topic
    std::string signal;
    std::string cachePath;
    std::map<std::string, std::string> records; // a verified record per name
    std::string me;
    std::map<std::string, int> mine; // "<level>:<difficulty>" to score
    long long nPostedAt = 0;         // this machine's last post, in seconds since 1970
    double flLastPostMs = -1e300;
};

Board *g_pBoard = nullptr;
pthread_mutex_t g_boardLock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_boardWake = PTHREAD_COND_INITIALIZER;
// Under g_boardLock, from the game thread: new bests, and the bests to report a rank for after the
// next post.
std::string g_boardName;
std::map<std::string, int> g_boardBests;
std::vector<std::string> g_boardRankKeys;
// Under g_boardLock, for the online page: the last rank notified, and the players on the board.
std::string g_boardLastRank;
size_t g_nBoardPlayers = 0;

std::string BoardName(const char *pszName) {
    std::string name;
    for (const char *p = pszName != nullptr ? pszName : "";
         *p != '\0' && name.size() < kBoardMaxName;
         ++p) {
        name += isalnum(static_cast<unsigned char>(*p)) || *p == '-' ? *p : '_';
    }
    return name.empty() ? "player" : name;
}

bool BoardKey(const std::string &key) {
    const size_t nColon = key.find(':');
    return nColon > 0 && nColon < 32 && nColon + 2 == key.size() && key[nColon + 1] >= '0' &&
           key[nColon + 1] <= '2' &&
           key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == nColon;
}

void InitBoard(Board &board, const char *pszCode) {
    DeriveKeys((std::string("board ") + pszCode).c_str(), &board.keys);
    const char *pszSignal = getenv("FREQ_ONLINE_SIGNAL");
    board.signal = pszSignal != nullptr ? pszSignal : kDefaultSignal;
    if (board.signal == "off" || board.signal.compare(0, 8, "https://") != 0) {
        board.signal.clear();
    }
    while (!board.signal.empty() && board.signal.back() == '/') {
        board.signal.pop_back();
    }
}

std::string BoardRecord(const Board &board) {
    std::string text = "FB1 " + board.me;
    for (const auto &entry : board.mine) {
        text += " " + entry.first + ":" + std::to_string(entry.second);
    }
    return SignRecord(board.keys, text);
}

// The trust boundary for the board's topic: a record of this code, signed, well formed.
bool ParseBoard(const Board &board,
                const std::string &text,
                std::string *pName,
                std::map<std::string, int> *pScores) {
    std::vector<std::string> words;
    for (size_t nAt = 0; nAt < text.size();) {
        const size_t nEnd = std::min(text.find(' ', nAt), text.size());
        words.push_back(text.substr(nAt, nEnd - nAt));
        nAt = nEnd + 1;
    }
    if (words.size() < 3 || words.size() > 3 + kBoardMaxEntries || words[0] != "FB1" ||
        words[1] != BoardName(words[1].c_str()) || words.back().size() != 32 ||
        SignRecord(board.keys, text.substr(0, text.size() - 33)) != text) {
        return false;
    }
    pScores->clear();
    for (size_t i = 2; i + 1 < words.size(); ++i) {
        const size_t nColon = words[i].rfind(':');
        char *pszEnd = nullptr;
        const long nScore = strtol(words[i].c_str() + nColon + 1, &pszEnd, 10);
        if (nColon == std::string::npos || !BoardKey(words[i].substr(0, nColon)) ||
            *pszEnd != '\0' || nScore < 0 || nScore > 99999999) {
            return false;
        }
        (*pScores)[words[i].substr(0, nColon)] = static_cast<int>(nScore);
    }
    *pName = words[1];
    return true;
}

// This machine's own record is the one it last posted: a poll can still return an older one. A
// machine with no bests of its own (a new title folder) takes them from the board.
void BoardTake(Board &board, const std::string &text) {
    std::string name;
    std::map<std::string, int> scores;
    if (!ParseBoard(board, text, &name, &scores) || (name == board.me && !board.mine.empty())) {
        return;
    }
    board.records[name] = text;
    if (name == board.me) {
        board.mine = scores;
    }
}

void BoardFetch(Board &board) {
    std::string response;
    if (Http(board.signal + "/" + board.keys.topic + "/json?poll=1&since=12h",
             nullptr,
             &response) == 200) {
        for (const std::string &message : Messages(response)) {
            BoardTake(board, message);
        }
    }
}

bool BoardCanPost(const Board &board, double flNowMs) {
    return flNowMs - board.flLastPostMs >= kBoardPostGapMs;
}

bool BoardPost(Board &board) {
    const std::string record = BoardRecord(board);
    std::string response;
    board.flLastPostMs = NowMs();
    const long nStatus = Http(board.signal + "/" + board.keys.topic, &record, &response);
    if (nStatus != 200) {
        fprintf(stderr, "board: the server would not take the record (%ld)\n", nStatus);
        return false;
    }
    board.nPostedAt = time(nullptr);
    board.records[board.me] = record;
    return true;
}

// This machine's rank for one best, and the board's top three.
std::string BoardRank(const Board &board, const std::string &key) {
    std::vector<std::pair<int, std::string>> scores;
    for (const auto &record : board.records) {
        std::string name;
        std::map<std::string, int> entries;
        if (ParseBoard(board, record.second, &name, &entries) && entries.count(key) != 0) {
            scores.emplace_back(-entries[key], name);
        }
    }
    std::sort(scores.begin(), scores.end());
    std::string top;
    size_t nRank = 0;
    for (size_t i = 0; i < scores.size(); ++i) {
        nRank = scores[i].second == board.me ? i + 1 : nRank;
        if (i < 3) {
            top += (i == 0 ? "" : ", ") + std::to_string(i + 1) + " " + scores[i].second + " " +
                   std::to_string(-scores[i].first);
        }
    }
    static const char *const kDifficulties[] = {"Easy", "Normal", "Expert"};
    return "Board, " + key.substr(0, key.find(':')) + " " + kDifficulties[key.back() - '0'] +
           ": #" + std::to_string(nRank) + " of " + std::to_string(scores.size()) + " (" + top +
           ")";
}

void SaveBoard(const Board &board) {
    std::string text = "me " + board.me + "\nposted " + std::to_string(board.nPostedAt) + "\n";
    for (const auto &record : board.records) {
        text += record.second + "\n";
    }
    const std::string temporary = board.cachePath + ".new";
    FILE *pFile =
        fdopen(open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), "w");
    if (pFile != nullptr && fputs(text.c_str(), pFile) >= 0 && fclose(pFile) == 0) {
        rename(temporary.c_str(), board.cachePath.c_str());
    }
}

void LoadBoard(Board &board) {
    FILE *pFile = fdopen(open(board.cachePath.c_str(), O_RDONLY | O_CLOEXEC), "r");
    char szLine[4096];
    while (pFile != nullptr && fgets(szLine, sizeof(szLine), pFile) != nullptr) {
        std::string line(szLine, strcspn(szLine, "\r\n"));
        if (line.compare(0, 3, "me ") == 0 && board.me.empty()) {
            board.me = BoardName(line.c_str() + 3);
        } else if (line.compare(0, 7, "posted ") == 0) {
            board.nPostedAt = atoll(line.c_str() + 7);
        } else {
            BoardTake(board, line);
        }
    }
    if (pFile != nullptr) {
        fclose(pFile);
    }
}

// The board is this thread's own; the game thread leaves its bests in g_boardBests.
void *RunBoard(void *) {
    Board &board = *g_pBoard;
    LoadBoard(board);
    BoardFetch(board);
    SaveBoard(board);
    fprintf(stderr, "board: %zu players on the board\n", board.records.size());
    pthread_mutex_lock(&g_boardLock);
    g_nBoardPlayers = board.records.size();
    pthread_mutex_unlock(&g_boardLock);
    bool bRepost = !board.mine.empty() && time(nullptr) - board.nPostedAt > kBoardRepostSeconds;
    pthread_mutex_lock(&g_boardLock);
    for (;;) {
        while (!bRepost && g_boardRankKeys.empty()) {
            pthread_cond_wait(&g_boardWake, &g_boardLock);
        }
        if (!BoardCanPost(board, NowMs())) {
            pthread_mutex_unlock(&g_boardLock);
            usleep(10 * 1000 * 1000);
            pthread_mutex_lock(&g_boardLock);
            continue;
        }
        bRepost = false;
        std::vector<std::string> keys;
        keys.swap(g_boardRankKeys);
        if (board.me.empty()) {
            board.me = g_boardName;
        }
        for (const auto &best : g_boardBests) {
            board.mine[best.first] = std::max(board.mine[best.first], best.second);
        }
        g_boardBests.clear();
        pthread_mutex_unlock(&g_boardLock);
        const bool bPosted = BoardPost(board);
        if (bPosted) {
            HostAchievement("board");
        }
        BoardFetch(board);
        SaveBoard(board);
        std::string rank;
        for (const std::string &key : keys) {
            if (bPosted) {
                rank = BoardRank(board, key);
                Notify(rank.c_str());
            }
        }
        pthread_mutex_lock(&g_boardLock);
        g_nBoardPlayers = board.records.size();
        g_boardLastRank = rank.empty() ? g_boardLastRank : rank;
        if (!bPosted) { // refused (ntfy.sh's daily limit): again after the gap, ranks after that
            g_boardRankKeys.insert(g_boardRankKeys.end(), keys.begin(), keys.end());
            bRepost = true;
        }
    }
    return nullptr;
}

} // namespace

extern "C" {

// From main(), through host/core/progress.cpp: the board thread when FREQ_BOARD names a code.
void HostBoardStart(void) {
    const char *pszCode = getenv("FREQ_BOARD");
    if (pszCode == nullptr || pszCode[0] == '\0') {
        return;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_pBoard = new Board;
    InitBoard(*g_pBoard, pszCode);
    g_pBoard->cachePath = HostTitleFile("board.txt");
    const char *pszName = getenv("FREQ_ONLINE_NAME");
    if (pszName != nullptr && pszName[0] != '\0') {
        g_pBoard->me = BoardName(pszName);
    }
    pthread_t thread;
    if (g_pBoard->signal.empty() || pthread_create(&thread, nullptr, RunBoard, nullptr) != 0) {
        fprintf(stderr, "board: off (no signal server, or no thread)\n");
        return;
    }
    pthread_detach(thread);
    fprintf(stderr, "board: topic %s\n", g_pBoard->keys.topic.c_str());
}

// For the online page, after HostSaveSetting() changed FREQ_BOARD. A first code starts the board
// now, and a changed or cleared one applies from the next start. Returns whether it applies now.
int HostBoardCodeChanged(void) {
    if (g_pBoard != nullptr) {
        return 0;
    }
    HostBoardStart();
    return g_pBoard != nullptr;
}

// For the online page, the number of players on the board and the last rank notified ("" before
// one).
int HostBoardStatus(char *pszRank, int nCapacity) {
    pthread_mutex_lock(&g_boardLock);
    snprintf(pszRank, nCapacity, "%s", g_boardLastRank.c_str());
    const int nPlayers = static_cast<int>(g_nBoardPlayers);
    pthread_mutex_unlock(&g_boardLock);
    return nPlayers;
}

// A new best (host/core/progress.cpp): posted with the rest, as soon as the rate allows, and its
// rank notified after.
void HostBoardBest(const char *pszLevel, int nDifficulty, int nScore, const char *pszName) {
    const std::string key =
        std::string(pszLevel != nullptr ? pszLevel : "") + ":" + std::to_string(nDifficulty);
    if (g_pBoard == nullptr || !BoardKey(key)) {
        return;
    }
    pthread_mutex_lock(&g_boardLock);
    g_boardName = BoardName(pszName);
    g_boardBests[key] = std::max(g_boardBests[key], nScore);
    g_boardRankKeys.push_back(key);
    pthread_cond_signal(&g_boardWake);
    pthread_mutex_unlock(&g_boardLock);
}

// FREQ_SELFTEST=board: two players on a fresh code through the real server, a forged record, and
// the rate limit. Prints BOARD_SELFTEST_OK when every check holds.
int HostBoardSelfTest(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    unsigned char abCode[8];
    RAND_bytes(abCode, sizeof(abCode));
    const std::string code = "selftest " + Hex(abCode, sizeof(abCode));
    Board alice;
    Board bob;
    Board mallory;
    InitBoard(alice, code.c_str());
    InitBoard(bob, code.c_str());
    InitBoard(mallory, "a code nobody gave mallory");
    mallory.keys.topic = alice.keys.topic;
    alice.me = "alice";
    bob.me = "bob";
    mallory.me = "mallory";
    alice.mine["dubpistols:2"] = 1000;
    bob.mine["dubpistols:2"] = 1500;
    bob.mine["funk:1"] = 700;
    mallory.mine["dubpistols:2"] = 99999;
    int nFailed = 0;
    const auto check = [&nFailed](bool bOk, const char *pszWhat) {
        fprintf(stderr, "board self-test: %s %s\n", bOk ? "ok" : "FAILED", pszWhat);
        nFailed += !bOk;
    };
    check(alice.signal == kDefaultSignal, "the default server is ntfy.sh");
    check(BoardPost(alice), "alice posts");
    check(BoardPost(bob), "bob posts");
    check(BoardPost(mallory), "mallory posts to the topic with the wrong key");
    sleep(2);
    BoardFetch(alice);
    BoardFetch(bob);
    check(alice.records.size() == 2 && bob.records.size() == 2, "each sees two players");
    check(alice.records.count("mallory") == 0, "the forged record is refused");
    const std::string aliceRank = BoardRank(alice, "dubpistols:2");
    const std::string bobRank = BoardRank(bob, "dubpistols:2");
    fprintf(stderr, "board self-test: %s | %s\n", aliceRank.c_str(), bobRank.c_str());
    check(aliceRank.find("#2 of 2 (1 bob 1500, 2 alice 1000)") != std::string::npos,
          "alice is 2nd");
    check(bobRank.find("#1 of 2") != std::string::npos, "bob is 1st");
    check(BoardRank(alice, "funk:1").find("#0 of 1") != std::string::npos,
          "alice has no funk score");
    check(!BoardCanPost(alice, NowMs()), "a second post right after is held back");
    check(BoardCanPost(alice, NowMs() + kBoardPostGapMs), "and allowed after 6 minutes");
    std::string tampered = alice.records["bob"];
    tampered.replace(tampered.find("1500"), 4, "9500");
    std::string name;
    std::map<std::string, int> scores;
    check(!ParseBoard(alice, tampered, &name, &scores), "a changed score breaks the signature");
    if (nFailed == 0) {
        printf("BOARD_SELFTEST_OK\n");
    }
    return nFailed == 0 ? 0 : 1;
}

} // extern "C"
