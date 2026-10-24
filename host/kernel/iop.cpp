// The I/O processor as the game sees it through the SIF: the reboot, module loads, the IOP heap,
// SIF DMA, and the RPC servers. Nothing runs on an IOP. Every request finishes when it is made.
//
// ponytail: sound is silent. The game's sound driver (server 0x12346) and the libsd remote calls
// get the replies their callers wait for and nothing more. Audio is a later phase.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libsdr.h>
#include <sifdev.h>
#include <sifrpc.h>

namespace {

// The game's IOP sound driver, and its selector that allocates the event buffers.
constexpr unsigned kSoundDriverServer = 0x12346;
constexpr unsigned kSoundSelectorAllocEventBuffer = 0x8010;

// IOP addresses handed out from the top of the IOP's 2 MiB, where no host pointer refers.
constexpr unsigned kIopHeapBase = 0x00100000;
constexpr unsigned kIopHeapLimit = 0x001f0000;

constexpr int kServerCount = 8;

// One server record per bound server id. A bound client's serve field points at it.
sceSifServeData g_aServers[kServerCount];
int g_nServerCount = 0;

unsigned g_nIopHeapNext = kIopHeapBase;
unsigned g_nNextSifDmaId = 1;
int g_nNextModuleId = 1;

unsigned AllocIop(unsigned nSize) {
    const unsigned nAddress = g_nIopHeapNext;
    g_nIopHeapNext += (nSize + 0xff) & ~0xffU;
    if (g_nIopHeapNext > kIopHeapLimit) {
        g_nIopHeapNext = kIopHeapBase;
    }
    return nAddress;
}

// The reply is zeros, except the address an allocation returns. The request's first word is the
// allocation size.
void ReplySoundDriver(unsigned nSelector, const void *pSend, void *pReceive, int nReceiveSize) {
    if (pReceive == nullptr || nReceiveSize < static_cast<int>(sizeof(unsigned))) {
        return;
    }
    unsigned nReply = 0;
    if (nSelector == kSoundSelectorAllocEventBuffer && pSend != nullptr) {
        unsigned nSize;
        memcpy(&nSize, pSend, sizeof(nSize));
        nReply = AllocIop(nSize);
    }
    memset(pReceive, 0, static_cast<size_t>(nReceiveSize));
    memcpy(pReceive, &nReply, sizeof(nReply));
}

} // namespace

extern "C" {

void sceSifInitRpc(unsigned) {
}

int sceSifRebootIop(const char *) {
    return 1;
}

int sceSifSyncIop() {
    return 1;
}

int sceFsReset() {
    return 0;
}

int sceSifLoadModule(const char *, int, const char *) {
    return g_nNextModuleId++;
}

int sceSifInitIopHeap() {
    return 0;
}

void *sceSifAllocIopHeap(unsigned nSize) {
    return reinterpret_cast<void *>(static_cast<uintptr_t>(AllocIop(nSize)));
}

int sceSifFreeIopHeap(void *) {
    return 0;
}

// ponytail: SIF DMA into IOP memory is dropped; no IOP code would read it.
unsigned sceSifSetDma(sceSifDmaData *, int) {
    return g_nNextSifDmaId++;
}

int sceSifDmaStat(unsigned) {
    return -1;
}

int sceSifBindRpc(sceSifClientData *pClient, unsigned nServer, unsigned) {
    sceSifServeData *pServer = nullptr;
    for (int i = 0; i < g_nServerCount; ++i) {
        if (g_aServers[i].command == nServer) {
            pServer = &g_aServers[i];
        }
    }
    if (pServer == nullptr) {
        if (g_nServerCount == kServerCount) {
            return -1;
        }
        pServer = &g_aServers[g_nServerCount++];
        pServer->command = nServer;
    }
    pClient->command = 0;
    pClient->serve = pServer;
    pClient->rpcd.paddr = nullptr;
    return 0;
}

int sceSifCallRpc(sceSifClientData *pClient,
                  unsigned nFunction,
                  unsigned,
                  void *pSend,
                  int nSendSize,
                  void *pReceive,
                  int nReceiveSize,
                  sceSifEndFunc pfnEnd,
                  void *pEndArgument) {
    const unsigned nServer = (pClient->serve != nullptr) ? pClient->serve->command : 0;
    if (nServer == kSoundDriverServer) {
        (void)nSendSize;
        ReplySoundDriver(nFunction, pSend, pReceive, nReceiveSize);
    } else {
        fprintf(stderr, "UNIMPL rpc server 0x%x function 0x%x\n", nServer, nFunction);
    }
    pClient->rpcd.paddr = nullptr;
    if (pfnEnd != nullptr) {
        pfnEnd(pEndArgument);
    }
    return 0;
}

int sceSifCheckStatRpc(sceSifRpcData *) {
    return 0;
}

int sceSdRemoteInit() {
    return 0;
}

int sceSdRemote(int, ...) {
    return 0;
}

} // extern "C"
