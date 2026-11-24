// Checks of host/net/online.cpp outside the game (host/net/online-test.sh builds and runs them):
//
//     online-test parse        the STUN parser and the signalling records
//     online-test reject       one process: a wrong room never links, bad datagrams are dropped
//     online-test side <n>     one side of a two-process run (the settings say which): link, send
//                              n messages, and receive the other side's n in order

#include <assert.h>

#include "online.cpp"

namespace {

// Message i from the machine in nSlot: its index, then filler of a length and content that both
// depend on it, so a reordered, cut, or swapped message does not compare equal.
std::string TestMessage(int nSlot, int i) {
    std::string message = std::to_string(nSlot) + ":" + std::to_string(i) + ":";
    for (int n = 0; n < (i * 37) % 900; ++n) {
        message += static_cast<char>('a' + (i + n * nSlot) % 26);
    }
    return message;
}

bool WaitFor(bool (*pfnReady)(), double flSeconds) {
    for (const double flUntil = NowMs() + flSeconds * 1000; NowMs() < flUntil; usleep(20000)) {
        if (pfnReady()) {
            return true;
        }
    }
    return false;
}

int Parse() {
    // RFC 5769, 2.2: a binding response whose XOR-MAPPED-ADDRESS is 192.0.2.1 port 32853.
    static const unsigned char kResponse[] = {
        0x01, 0x01, 0x00, 0x3c, 0x21, 0x12, 0xa4, 0x42, 0xb7, 0xe7, 0xa7, 0x01, 0xbc, 0x34,
        0xd6, 0x86, 0xfa, 0x87, 0xdf, 0xae, 0x80, 0x22, 0x00, 0x0b, 0x74, 0x65, 0x73, 0x74,
        0x20, 0x76, 0x65, 0x63, 0x74, 0x6f, 0x72, 0x20, 0x00, 0x20, 0x00, 0x08, 0x00, 0x01,
        0xa1, 0x47, 0xe1, 0x12, 0xa6, 0x43, 0x00, 0x08, 0x00, 0x14, 0x2b, 0x91, 0xf5, 0x99,
        0xfd, 0x9e, 0x90, 0xc3, 0x8c, 0x74, 0x89, 0xf9, 0x2a, 0xf9, 0xba, 0x53, 0xf0, 0x6b,
        0xe7, 0xd7, 0x80, 0x28, 0x00, 0x04, 0xc0, 0x7d, 0x4c, 0x96};
    sockaddr_in mapped;
    assert(ParseStun(kResponse, sizeof(kResponse), kResponse + 8, &mapped));
    assert(AddressText(mapped) == "192.0.2.1:32853");
    unsigned char abOther[12] = {};
    assert(!ParseStun(kResponse, sizeof(kResponse), abOther, &mapped));
    assert(!ParseStun(kResponse, 30, kResponse + 8, &mapped));

    Session session;
    Session other;
    DeriveKeys("  Blue Note 42 ", &session);
    DeriveKeys("blue note 42", &other);
    assert(session.topic == other.topic && session.topic.size() == 28);
    DeriveKeys("blue note 43", &other);
    assert(session.topic != other.topic);
    const std::string record =
        SignRecord(session, "FQ1 H 0123456789abcdef 10.0.0.5:47125 203.0.113.9:6000");
    Record parsed;
    assert(ParseRecord(session, record, &parsed) && parsed.bHost &&
           parsed.nNonce == 0x0123456789abcdefull && parsed.candidates.size() == 2 &&
           AddressText(parsed.candidates[1]) == "203.0.113.9:6000");
    std::string changed = record;
    changed[30] = changed[30] == '5' ? '6' : '5';
    assert(!ParseRecord(session, changed, &parsed));
    assert(!ParseRecord(other, record, &parsed));
    assert(!ParseRecord(session, "FQ1 H 0123", &parsed));
    assert(
        !ParseRecord(session, SignRecord(session, "FQ1 H 0123456789abcdef 10.0.0.5:0"), &parsed));
    printf("ONLINE_PARSE_OK\n");
    return 0;
}

Session *g_pHost;
Session *g_pGuest;
Session *g_pStranger;

unsigned HostOnlineLinkedOf(Session &session) {
    pthread_mutex_lock(&session.lock);
    const unsigned nLinked = session.nLinked;
    pthread_mutex_unlock(&session.lock);
    return nLinked;
}

Session *StartTestSession(const char *pszRoom, bool bHost, const sockaddr_in *pPeer) {
    Session *pSession = new Session;
    DeriveKeys(pszRoom, pSession);
    pSession->bHost = bHost;
    pSession->nSlot = bHost ? 0 : -1;
    RAND_bytes(reinterpret_cast<unsigned char *>(&pSession->nNonce), sizeof(pSession->nNonce));
    pSession->nSocket = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(pSession->nSocket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    if (pPeer != nullptr) {
        pSession->targets.push_back(Target{*pPeer, 1e300});
    }
    pthread_t thread;
    assert(pthread_create(&thread, nullptr, RunNet, pSession) == 0);
    pthread_detach(thread);
    return pSession;
}

sockaddr_in BoundAddress(const Session &session) {
    sockaddr_in address{};
    socklen_t nLength = sizeof(address);
    getsockname(session.nSocket, reinterpret_cast<sockaddr *>(&address), &nLength);
    return address;
}

std::vector<Inbound> TakeInbox(Session &session) {
    pthread_mutex_lock(&session.lock);
    std::vector<Inbound> inbox(session.inbox.begin(), session.inbox.end());
    session.inbox.clear();
    pthread_mutex_unlock(&session.lock);
    return inbox;
}

void SendFrom(Session &session, const std::string &message) {
    pthread_mutex_lock(&session.lock);
    session.outbox.push_back(message);
    pthread_mutex_unlock(&session.lock);
}

int Reject() {
    setenv("FREQ_ONLINE_STUN", "off", 1);
    g_pHost = StartTestSession("alpha", true, nullptr);
    const sockaddr_in host = BoundAddress(*g_pHost);
    g_pGuest = StartTestSession("alpha", false, &host);
    g_pStranger = StartTestSession("beta", false, &host);
    assert(WaitFor([] { return HostOnlineLinkedOf(*g_pGuest) != 0; }, 5));
    usleep(1500 * 1000);
    assert(HostOnlineLinkedOf(*g_pStranger) == 0 && HostOnlineLinkedOf(*g_pHost) == 2);
    assert(g_pHost->drops.nTag > 0);

    const int nAttacker = socket(AF_INET, SOCK_DGRAM, 0);
    auto attack = [&](const std::string &bytes) {
        sendto(nAttacker,
               bytes.data(),
               bytes.size(),
               0,
               reinterpret_cast<const sockaddr *>(&host),
               sizeof(host));
    };
    Datagram data;
    data.nKind = kData;
    data.nNonce = g_pGuest->nNonce;
    data.nSeq = 1;
    data.nFrom = 1;
    data.nOrigin = 1;
    data.payload = "replayed";
    std::string forged = Encode(*g_pHost, data);
    forged[forged.size() - 1] ^= 1;
    const int nTagBefore = g_pHost->drops.nTag;
    attack(forged);
    attack(std::string(10, 'F'));
    attack(std::string(kMaxDatagram + 1, 'F'));
    Datagram stale = data;
    stale.nNonce ^= 0x55;
    attack(Encode(*g_pHost, stale));

    // The guest's first message, then the same datagram again: from the guest's own address (a
    // duplicate) and from elsewhere (not the link's address).
    SendFrom(*g_pGuest, "first");
    assert(WaitFor(
        [] {
            pthread_mutex_lock(&g_pHost->lock);
            const bool bArrived = !g_pHost->inbox.empty();
            pthread_mutex_unlock(&g_pHost->lock);
            return bArrived;
        },
        3));
    data.payload = "first";
    const std::string replay = Encode(*g_pHost, data);
    const int nSessionBefore = g_pHost->drops.nSession;
    sendto(g_pGuest->nSocket,
           replay.data(),
           replay.size(),
           0,
           reinterpret_cast<const sockaddr *>(&host),
           sizeof(host));
    attack(replay);
    usleep(300 * 1000);
    assert(g_pHost->drops.nSession > nSessionBefore);
    SendFrom(*g_pGuest, "second");
    SendFrom(*g_pHost, "back");
    usleep(500 * 1000);
    const std::vector<Inbound> atHost = TakeInbox(*g_pHost);
    const std::vector<Inbound> atGuest = TakeInbox(*g_pGuest);
    assert(atHost.size() == 2 && atHost[0].data == "first" && atHost[1].data == "second" &&
           atHost[0].nFrom == 1);
    assert(atGuest.size() == 1 && atGuest[0].data == "back" && atGuest[0].nFrom == 0);
    const Drops &drops = g_pHost->drops;
    assert(drops.nTag > nTagBefore && drops.nSize >= 2 && drops.nSession >= 1 &&
           drops.nDuplicate >= 1);
    printf("drops: size %d, tag %d, session %d, duplicate %d\n",
           drops.nSize,
           drops.nTag,
           drops.nSession,
           drops.nDuplicate);
    printf("ONLINE_REJECT_OK\n");
    return 0;
}

int Side(int nCount) {
    HostOnlineStart();
    assert(g_pSession != nullptr);
    if (!WaitFor([] { return HostOnlineLinked() != 0; }, 60)) {
        fprintf(stderr, "online-test: no link\n");
        return 1;
    }
    const int nSlot = HostOnlineSlot();
    const int nOther = nSlot == 0 ? 1 : 0;
    for (int i = 0; i < nCount; ++i) {
        const std::string message = TestMessage(nSlot, i);
        assert(HostOnlineSend(message.data(), static_cast<int>(message.size())));
    }
    int nReceived = 0;
    for (const double flUntil = NowMs() + 60000; nReceived < nCount && NowMs() < flUntil;) {
        char abMessage[HOST_ONLINE_MAX_MESSAGE];
        int nFrom = -1;
        const int nSize = HostOnlineReceive(abMessage, sizeof(abMessage), &nFrom);
        if (nSize == 0) {
            usleep(5000);
            continue;
        }
        if (nFrom != nOther || std::string(abMessage, nSize) != TestMessage(nOther, nReceived)) {
            fprintf(stderr, "online-test: message %d is wrong\n", nReceived);
            return 1;
        }
        ++nReceived;
    }
    // Stay until the other side has acknowledged everything, so it can finish too.
    WaitFor([] { return g_pSession->links[HostOnlineSlot() == 0 ? 1 : 0].unacked.empty(); }, 20);
    usleep(1000 * 1000);
    printf("side %d: sent %d, received %d in order, drops: tag %d duplicate %d\n",
           nSlot,
           nCount,
           nReceived,
           g_pSession->drops.nTag,
           g_pSession->drops.nDuplicate);
    return nReceived == nCount ? 0 : 1;
}

} // namespace

int main(int nArguments, char **ppArguments) {
    const std::string mode = nArguments > 1 ? ppArguments[1] : "";
    if (mode == "parse") {
        return Parse();
    }
    if (mode == "reject") {
        return Reject();
    }
    if (mode == "side" && nArguments > 2) {
        return Side(atoi(ppArguments[2]));
    }
    fprintf(stderr, "usage: online-test parse | reject | side <count>\n");
    return 2;
}
