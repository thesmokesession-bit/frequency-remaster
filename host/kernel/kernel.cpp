// The Emotion Engine kernel services the game calls: threads, semaphores, interrupt handlers, and
// the cache calls.
//
// The Emotion Engine has one core, and its scheduler runs the ready thread of highest priority.
// Each game thread here is a host thread, and one scheduler lock with a running-thread token lets
// one of them run at a time. A thread gives the token up only when it blocks, exits, or wakes a
// thread of higher priority. Interrupt handlers run on host threads outside the token, as an
// interrupt runs between two instructions of whichever thread holds the processor.
//
// ponytail: a thread an interrupt wakes runs once the running thread next blocks, not at once.
// The host cannot stop a running thread between two instructions. Add a point where the running
// thread yields (in VSync() or the busy-wait loops) if a game thread ever spins on work a
// higher-priority thread does.

#include "kernel.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <eekernel.h>
#include <malloc.h>

namespace {

constexpr int kThreadCount = 256;
constexpr int kSemaphoreCount = 256;
constexpr int kMainThreadId = 1;
constexpr int kMainThreadPriority = 1;

// The id the thread calls take for the calling thread.
constexpr int kSelf = 0;

// ReferThreadStatus() states and wait types, as the kernel reports them.
constexpr int kThreadRunning = 0x01;
constexpr int kThreadReady = 0x02;
constexpr int kThreadWaiting = 0x04;
constexpr int kThreadDormant = 0x10;
constexpr int kWaitNone = 0;
constexpr int kWaitSleep = 1;
constexpr int kWaitSemaphore = 2;

// Kernel error codes.
constexpr int kErrorUnknownThread = -406;
constexpr int kErrorNotDormant = -414;
constexpr int kErrorSemaphoreZero = -419;
constexpr int kErrorUnknownSemaphore = -408;
constexpr int kErrorNoSlot = -400;

// ponytail: each thread gets its own stack from the low heap instead of the one the game passes,
// which is sized for MIPS frames. Stacks are never freed; the game creates a handful of threads.
constexpr size_t kHostStackSize = 0x80000;

constexpr int kInterruptCauses = 16;
constexpr int kHandlersPerCause = 8;

struct Thread {
    bool bUsed;
    int nState;
    int nWaitType;
    int nWaitId;
    int nPriority;
    unsigned long long llOrder;
    int nWakeupCount;
    unsigned nGeneration;
    ThreadParam param;
    void *pArgument;
    pthread_cond_t wake;
};

struct Semaphore {
    bool bUsed;
    SemaParam param;
};

struct HandlerList {
    int (*handlers[kHandlersPerCause])(int);
    bool bEnabled;
};

Thread g_aThreads[kThreadCount];
Semaphore g_aSemaphores[kSemaphoreCount];
HandlerList g_aIntcHandlers[kInterruptCauses];
HandlerList g_aDmacHandlers[kInterruptCauses];

pthread_mutex_t g_schedulerLock = PTHREAD_MUTEX_INITIALIZER;
// A thread waits here forever once it has exited or been terminated.
pthread_cond_t g_parked = PTHREAD_COND_INITIALIZER;
// The thread holding the processor, or 0 while every thread waits.
int g_nRunning = 0;
unsigned long long g_llOrderClock = 0;

// Held while interrupt handlers run and while a thread has interrupts disabled. A handler may
// enable or disable a cause, which takes the lock again.
pthread_mutex_t g_interruptLock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

thread_local int t_nThreadId = 0;
thread_local unsigned t_nGeneration = 0;
thread_local bool t_bInterruptsDisabled = false;
thread_local bool t_bInInterrupt = false;

struct SchedulerGuard {
    SchedulerGuard() {
        pthread_mutex_lock(&g_schedulerLock);
    }
    ~SchedulerGuard() {
        pthread_mutex_unlock(&g_schedulerLock);
    }
};

[[noreturn]] void Park() {
    for (;;) {
        pthread_cond_wait(&g_parked, &g_schedulerLock);
    }
}

// The calling game thread, or 0 in an interrupt handler and on a host thread that is not one. The
// first thread to call outside a handler runs the game's main(), and the kernel adopts it as
// thread 1. Called with the scheduler lock held.
int Self() {
    if (t_bInInterrupt) {
        return 0;
    }
    if (t_nThreadId == 0 && !g_aThreads[kMainThreadId].bUsed) {
        Thread &main = g_aThreads[kMainThreadId];
        main.bUsed = true;
        main.nState = kThreadRunning;
        main.nPriority = kMainThreadPriority;
        main.param.initPriority = kMainThreadPriority;
        pthread_cond_init(&main.wake, nullptr);
        g_nRunning = kMainThreadId;
        t_nThreadId = kMainThreadId;
    }
    return t_nThreadId;
}

Thread *Find(int nId) {
    if (nId == kSelf) {
        nId = Self();
    }
    if (nId <= 0 || nId >= kThreadCount || !g_aThreads[nId].bUsed) {
        return nullptr;
    }
    return &g_aThreads[nId];
}

// Gives the processor to the ready thread of highest priority, the earliest made ready among
// equals.
void Schedule() {
    int nBest = 0;
    for (int i = 1; i < kThreadCount; ++i) {
        const Thread &thread = g_aThreads[i];
        if (thread.bUsed && thread.nState == kThreadReady &&
            (nBest == 0 || thread.nPriority < g_aThreads[nBest].nPriority ||
             (thread.nPriority == g_aThreads[nBest].nPriority &&
              thread.llOrder < g_aThreads[nBest].llOrder))) {
            nBest = i;
        }
    }
    g_nRunning = nBest;
    if (nBest != 0) {
        g_aThreads[nBest].nState = kThreadRunning;
        pthread_cond_broadcast(&g_aThreads[nBest].wake);
    }
}

// Waits until the calling thread holds the processor. A thread terminated meanwhile never runs
// again.
void WaitForProcessor() {
    Thread &self = g_aThreads[t_nThreadId];
    while (g_nRunning != t_nThreadId || self.nGeneration != t_nGeneration) {
        if (self.nGeneration != t_nGeneration) {
            Park();
        }
        pthread_cond_wait(&self.wake, &g_schedulerLock);
    }
}

// Puts the calling thread back among the ready threads and runs the best one, which may be the
// caller again. A thread that yields to a higher priority keeps its place at the head of its own.
void Yield(bool bToTail) {
    Thread &self = g_aThreads[t_nThreadId];
    self.nState = kThreadReady;
    self.llOrder = bToTail ? ++g_llOrderClock : 0;
    Schedule();
    WaitForProcessor();
}

void Block(int nWaitType, int nWaitId) {
    Thread &self = g_aThreads[t_nThreadId];
    self.nState = kThreadWaiting;
    self.nWaitType = nWaitType;
    self.nWaitId = nWaitId;
    self.llOrder = ++g_llOrderClock;
    Schedule();
    WaitForProcessor();
}

void MakeReady(Thread &thread) {
    thread.nState = kThreadReady;
    thread.nWaitType = kWaitNone;
    thread.llOrder = ++g_llOrderClock;
}

// After a thread was made ready: a game thread yields to it when it has higher priority, and an
// interrupt hands it the processor when no thread holds it.
void Reschedule(const Thread &woken) {
    const int nSelf = Self();
    if (nSelf == 0) {
        if (g_nRunning == 0) {
            Schedule();
        }
    } else if (woken.nPriority < g_aThreads[nSelf].nPriority) {
        Yield(false);
    }
}

void *RunThread(void *pId) {
    t_nThreadId = static_cast<int>(reinterpret_cast<intptr_t>(pId));
    pthread_mutex_lock(&g_schedulerLock);
    Thread &self = g_aThreads[t_nThreadId];
    t_nGeneration = self.nGeneration;
    WaitForProcessor();
    pthread_mutex_unlock(&g_schedulerLock);
    self.param.entry(self.pArgument);
    pthread_mutex_lock(&g_schedulerLock);
    self.nState = kThreadDormant;
    Schedule();
    Park();
}

int Wakeup(int nId) {
    SchedulerGuard guard;
    Thread *pThread = Find(nId);
    if (pThread == nullptr) {
        return kErrorUnknownThread;
    }
    if (pThread->nState == kThreadWaiting && pThread->nWaitType == kWaitSleep) {
        MakeReady(*pThread);
        Reschedule(*pThread);
    } else {
        ++pThread->nWakeupCount;
    }
    return nId;
}

int Signal(int nId) {
    SchedulerGuard guard;
    if (nId <= 0 || nId >= kSemaphoreCount || !g_aSemaphores[nId].bUsed) {
        return kErrorUnknownSemaphore;
    }
    Thread *pFirst = nullptr;
    for (Thread &thread : g_aThreads) {
        if (thread.bUsed && thread.nState == kThreadWaiting && thread.nWaitType == kWaitSemaphore &&
            thread.nWaitId == nId && (pFirst == nullptr || thread.llOrder < pFirst->llOrder)) {
            pFirst = &thread;
        }
    }
    if (pFirst == nullptr) {
        ++g_aSemaphores[nId].param.currentCount;
        return nId;
    }
    --g_aSemaphores[nId].param.numWaitThreads;
    MakeReady(*pFirst);
    Reschedule(*pFirst);
    return nId;
}

int Poll(int nId) {
    SchedulerGuard guard;
    if (nId <= 0 || nId >= kSemaphoreCount || !g_aSemaphores[nId].bUsed) {
        return kErrorUnknownSemaphore;
    }
    if (g_aSemaphores[nId].param.currentCount == 0) {
        return kErrorSemaphoreZero;
    }
    --g_aSemaphores[nId].param.currentCount;
    return nId;
}

int AddHandler(HandlerList *pLists, int nCause, int (*pfnHandler)(int), int nNext) {
    if (nCause < 0 || nCause >= kInterruptCauses) {
        return -1;
    }
    pthread_mutex_lock(&g_interruptLock);
    auto &handlers = pLists[nCause].handlers;
    int nSlot = -1;
    for (int i = 0; i < kHandlersPerCause; ++i) {
        if (handlers[i] == nullptr) {
            nSlot = i;
            break;
        }
    }
    if (nSlot >= 0) {
        // ponytail: handlers run in slot order, so nNext (0 to run first) is ignored. The game
        // installs one handler per cause.
        (void)nNext;
        handlers[nSlot] = pfnHandler;
    }
    pthread_mutex_unlock(&g_interruptLock);
    return nSlot + 1;
}

int RemoveHandler(HandlerList *pLists, int nCause, int nHandlerId) {
    if (nCause < 0 || nCause >= kInterruptCauses || nHandlerId <= 0 ||
        nHandlerId > kHandlersPerCause) {
        return -1;
    }
    pthread_mutex_lock(&g_interruptLock);
    pLists[nCause].handlers[nHandlerId - 1] = nullptr;
    pthread_mutex_unlock(&g_interruptLock);
    return 0;
}

int SetEnabled(HandlerList *pLists, int nCause, bool bEnabled) {
    if (nCause < 0 || nCause >= kInterruptCauses) {
        return -1;
    }
    pthread_mutex_lock(&g_interruptLock);
    const bool bWas = pLists[nCause].bEnabled;
    pLists[nCause].bEnabled = bEnabled;
    pthread_mutex_unlock(&g_interruptLock);
    return bWas != bEnabled;
}

void Raise(HandlerList *pLists, int nCause) {
    pthread_mutex_lock(&g_interruptLock);
    const bool bWasInInterrupt = t_bInInterrupt;
    t_bInInterrupt = true;
    if (pLists[nCause].bEnabled) {
        for (auto pfnHandler : pLists[nCause].handlers) {
            if (pfnHandler != nullptr) {
                pfnHandler(nCause);
            }
        }
    }
    t_bInInterrupt = bWasInInterrupt;
    pthread_mutex_unlock(&g_interruptLock);
}

} // namespace

// Runs the handlers of an interrupt controller cause, as the hardware does when it raises one.
void HostRaiseIntc(int nCause) {
    Raise(g_aIntcHandlers, nCause);
}

// Runs the handlers of a DMA channel's completion interrupt.
void HostRaiseDmac(int nChannel) {
    Raise(g_aDmacHandlers, nChannel);
}

extern "C" {

int CreateThread(ThreadParam *pParam) {
    SchedulerGuard guard;
    Self();
    for (int i = kMainThreadId + 1; i < kThreadCount; ++i) {
        Thread &thread = g_aThreads[i];
        if (thread.bUsed) {
            continue;
        }
        thread.bUsed = true;
        thread.nState = kThreadDormant;
        thread.nWaitType = kWaitNone;
        thread.nPriority = pParam->initPriority;
        thread.nWakeupCount = 0;
        thread.param = *pParam;
        pthread_cond_init(&thread.wake, nullptr);
        return i;
    }
    return kErrorNoSlot;
}

int StartThread(int nId, void *pArgument) {
    SchedulerGuard guard;
    Self();
    Thread *pThread = Find(nId);
    if (pThread == nullptr || nId == kSelf) {
        return kErrorUnknownThread;
    }
    if (pThread->nState != kThreadDormant) {
        return kErrorNotDormant;
    }
    ++pThread->nGeneration;
    pThread->pArgument = pArgument;
    pThread->nPriority = pThread->param.initPriority;
    pThread->nWakeupCount = 0;
    MakeReady(*pThread);

    pthread_attr_t attributes;
    pthread_t host;
    void *pStack = memalign(64, kHostStackSize);
    if (pStack == nullptr || pthread_attr_init(&attributes) != 0 ||
        pthread_attr_setstack(&attributes, pStack, kHostStackSize) != 0 ||
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) != 0 ||
        pthread_create(&host, &attributes, RunThread, reinterpret_cast<void *>(nId)) != 0) {
        pThread->nState = kThreadDormant;
        return kErrorNoSlot;
    }
    pthread_attr_destroy(&attributes);
    Reschedule(*pThread);
    return nId;
}

int TerminateThread(int nId) {
    SchedulerGuard guard;
    Thread *pThread = Find(nId);
    if (pThread == nullptr || nId == kSelf || nId == Self()) {
        return kErrorUnknownThread;
    }
    if (pThread->nState == kThreadWaiting && pThread->nWaitType == kWaitSemaphore) {
        --g_aSemaphores[pThread->nWaitId].param.numWaitThreads;
    }
    pThread->nState = kThreadDormant;
    pThread->nWaitType = kWaitNone;
    // The host thread behind it parks the next time it wakes.
    ++pThread->nGeneration;
    pthread_cond_broadcast(&pThread->wake);
    return nId;
}

int DeleteThread(int nId) {
    SchedulerGuard guard;
    Thread *pThread = Find(nId);
    if (pThread == nullptr || nId == kSelf) {
        return kErrorUnknownThread;
    }
    if (pThread->nState != kThreadDormant) {
        return kErrorNotDormant;
    }
    pThread->bUsed = false;
    return nId;
}

int GetThreadId() {
    SchedulerGuard guard;
    return Self();
}

int ChangeThreadPriority(int nId, int nPriority) {
    SchedulerGuard guard;
    Thread *pThread = Find(nId);
    if (pThread == nullptr) {
        return kErrorUnknownThread;
    }
    const int nPrevious = pThread->nPriority;
    pThread->nPriority = nPriority;
    if (Self() != 0) {
        Yield(false);
    }
    return nPrevious;
}

int RotateThreadReadyQueue(int nPriority) {
    SchedulerGuard guard;
    const int nSelf = Self();
    if (nSelf != 0 && g_aThreads[nSelf].nPriority == nPriority) {
        Yield(true);
    }
    return nPriority;
}

int ReferThreadStatus(int nId, ThreadParam *pInfo) {
    SchedulerGuard guard;
    Thread *pThread = Find(nId);
    if (pThread == nullptr) {
        return kErrorUnknownThread;
    }
    *pInfo = pThread->param;
    pInfo->status = pThread->nState;
    pInfo->currentPriority = pThread->nPriority;
    pInfo->waitType = pThread->nWaitType;
    pInfo->waitId = pThread->nWaitId;
    pInfo->wakeupCount = pThread->nWakeupCount;
    return nId == kSelf ? Self() : nId;
}

int SleepThread() {
    SchedulerGuard guard;
    const int nSelf = Self();
    Thread &self = g_aThreads[nSelf];
    if (self.nWakeupCount > 0) {
        --self.nWakeupCount;
    } else {
        Block(kWaitSleep, 0);
    }
    return nSelf;
}

int WakeupThread(int nId) {
    return Wakeup(nId);
}

int iWakeupThread(int nId) {
    return Wakeup(nId);
}

int CreateSema(SemaParam *pParam) {
    SchedulerGuard guard;
    for (int i = 1; i < kSemaphoreCount; ++i) {
        if (!g_aSemaphores[i].bUsed) {
            g_aSemaphores[i].bUsed = true;
            g_aSemaphores[i].param = *pParam;
            g_aSemaphores[i].param.currentCount = pParam->initCount;
            g_aSemaphores[i].param.numWaitThreads = 0;
            return i;
        }
    }
    return kErrorNoSlot;
}

int DeleteSema(int nId) {
    SchedulerGuard guard;
    if (nId <= 0 || nId >= kSemaphoreCount || !g_aSemaphores[nId].bUsed) {
        return kErrorUnknownSemaphore;
    }
    g_aSemaphores[nId].bUsed = false;
    return nId;
}

int SignalSema(int nId) {
    return Signal(nId);
}

int iSignalSema(int nId) {
    return Signal(nId);
}

int PollSema(int nId) {
    return Poll(nId);
}

int WaitSema(int nId) {
    SchedulerGuard guard;
    if (nId <= 0 || nId >= kSemaphoreCount || !g_aSemaphores[nId].bUsed) {
        return kErrorUnknownSemaphore;
    }
    SemaParam &param = g_aSemaphores[nId].param;
    if (param.currentCount > 0) {
        --param.currentCount;
        return nId;
    }
    ++param.numWaitThreads;
    Block(kWaitSemaphore, nId);
    return nId;
}

int AddIntcHandler(int nCause, int (*pfnHandler)(int), int nNext) {
    return AddHandler(g_aIntcHandlers, nCause, pfnHandler, nNext);
}

int RemoveIntcHandler(int nCause, int nHandlerId) {
    return RemoveHandler(g_aIntcHandlers, nCause, nHandlerId);
}

int AddDmacHandler(int nChannel, int (*pfnHandler)(int), int nNext) {
    return AddHandler(g_aDmacHandlers, nChannel, pfnHandler, nNext);
}

int RemoveDmacHandler(int nChannel, int nHandlerId) {
    return RemoveHandler(g_aDmacHandlers, nChannel, nHandlerId);
}

int EnableIntc(int nCause) {
    return SetEnabled(g_aIntcHandlers, nCause, true);
}

int DisableIntc(int nCause) {
    return SetEnabled(g_aIntcHandlers, nCause, false);
}

int EnableDmac(int nChannel) {
    return SetEnabled(g_aDmacHandlers, nChannel, true);
}

int DisableDmac(int nChannel) {
    return SetEnabled(g_aDmacHandlers, nChannel, false);
}

// Interrupts stay off for the calling thread until EIntr(). Both report whether interrupts were
// enabled.
int DIntr() {
    if (t_bInterruptsDisabled) {
        return 0;
    }
    pthread_mutex_lock(&g_interruptLock);
    t_bInterruptsDisabled = true;
    return 1;
}

int EIntr() {
    if (!t_bInterruptsDisabled) {
        return 1;
    }
    t_bInterruptsDisabled = false;
    pthread_mutex_unlock(&g_interruptLock);
    return 0;
}

// The host keeps memory coherent, and DMA reads memory directly.
void FlushCache(int) {
}

void SyncDCache(void *, void *) {
}

} // extern "C"
