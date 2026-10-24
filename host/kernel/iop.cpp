// The I/O processor as the game sees it through the SIF: the reboot, module loads, the IOP heap,
// SIF DMA, and the RPC servers. The game's own sound driver (src/ezmidi, EZMIDI.IRX) is the one
// IOP module that runs: its RPC server is served here, on the calling thread, under the IOP lock
// of host/audio/audio.cpp. Every request finishes when it is made.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sifdev.h>
#include <sifrpc.h>

extern "C" {
void HostIopLock();                                                  // host/audio/audio.cpp
void HostIopUnlock();                                                // host/audio/audio.cpp
void HostIopWrite(void *pDest, const void *pSource, unsigned nSize); // host/audio/audio.cpp
void HostLoadEzmidi();                                               // host/audio/audio.cpp
}

namespace {

constexpr int kServerCount = 8;

// One server record per server id, bound by the EE or registered by an IOP module. A bound
// client's serve field points at it; a registered server has its handler and argument buffer.
sceSifServeData g_aServers[kServerCount];
int g_nServerCount = 0;

unsigned g_nNextSifDmaId = 1;
int g_nNextModuleId = 1;

sceSifServeData *FindServer(unsigned nServer) {
    for (int i = 0; i < g_nServerCount; ++i) {
        if (g_aServers[i].command == nServer) {
            return &g_aServers[i];
        }
    }
    if (g_nServerCount == kServerCount) {
        return nullptr;
    }
    sceSifServeData *pServer = &g_aServers[g_nServerCount++];
    pServer->command = nServer;
    return pServer;
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

// The file is opened and closed, so that a module missing from the disc shows in the
// FREQ_LOG_MISSING report as it would fail to load on the console. Of the modules, only the game's
// sound driver runs.
int sceSifLoadModule(const char *pszPath, int, const char *) {
    const int nFile = sceOpen(pszPath, SCE_RDONLY);
    if (nFile >= 0) {
        sceClose(nFile);
        if (strstr(pszPath, "EZMIDI.IRX") != nullptr) {
            HostLoadEzmidi();
        }
    }
    return g_nNextModuleId++;
}

int sceSifInitIopHeap() {
    return 0;
}

// IOP memory is host memory in the low heap, so that its 32-bit addresses are host pointers.
void *sceSifAllocIopHeap(unsigned nSize) {
    void *pBlock = nullptr;
    return posix_memalign(&pBlock, 64, nSize) == 0 ? pBlock : nullptr;
}

int sceSifFreeIopHeap(void *pBlock) {
    free(pBlock);
    return 0;
}

unsigned sceSifSetDma(sceSifDmaData *pTransfers, int nCount) {
    HostIopLock();
    for (int i = 0; i < nCount; ++i) {
        const sceSifDmaData &transfer = pTransfers[i];
        HostIopWrite(reinterpret_cast<void *>(static_cast<uintptr_t>(transfer.addr)),
                     reinterpret_cast<const void *>(static_cast<uintptr_t>(transfer.data)),
                     transfer.size);
    }
    HostIopUnlock();
    return g_nNextSifDmaId++;
}

int sceSifDmaStat(unsigned) {
    return -1;
}

int sceSifBindRpc(sceSifClientData *pClient, unsigned nServer, unsigned) {
    sceSifServeData *pServer = FindServer(nServer);
    if (pServer == nullptr) {
        return -1;
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
    sceSifServeData *pServer = pClient->serve;
    if (pServer != nullptr && pServer->func != nullptr) {
        // As the SIF does: the request into the server's buffer, the handler, then the reply
        // from the buffer the handler returns.
        HostIopLock();
        if (pSend != nullptr && nSendSize > 0) {
            memcpy(pServer->buff, pSend, static_cast<size_t>(nSendSize));
        }
        const void *pReply = pServer->func(nFunction, pServer->buff, nSendSize);
        if (pReceive != nullptr && nReceiveSize > 0 && pReply != nullptr) {
            memcpy(pReceive, pReply, static_cast<size_t>(nReceiveSize));
        }
        HostIopUnlock();
    } else {
        fprintf(stderr,
                "UNIMPL rpc server 0x%x function 0x%x\n",
                pServer != nullptr ? pServer->command : 0,
                nFunction);
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

// The IOP side of the SIF RPC, as the sound driver calls it. Its server thread registers and
// enters the loop, which returns at once: requests are served by sceSifCallRpc() above.
void IopSifInitRpc(unsigned) {
}

void IopSifSetRpcQueue(sceSifQueueData *, int) {
}

sceSifServeData *IopSifRegisterRpc(sceSifServeData *pServe,
                                   unsigned nServer,
                                   sceSifRpcFunc pfnServe,
                                   void *pBuffer,
                                   sceSifRpcFunc,
                                   void *,
                                   sceSifQueueData *) {
    sceSifServeData *pServer = FindServer(nServer);
    if (pServer != nullptr) {
        pServer->func = pfnServe;
        pServer->buff = pBuffer;
    }
    return pServe;
}

void IopSifRpcLoop(sceSifQueueData *) {
}

} // extern "C"
