// A check of the thread, semaphore, and interrupt emulation, outside the game. From the root:
//
//     g++ -std=gnu++17 -Isce/ee/include host/kernel/*.cpp -lpthread -o kernel-test
//     ./kernel-test

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string>
#include <unistd.h>

#include <eekernel.h>

#include "kernel.h"

namespace {

std::string g_log;
int g_nSemaphore;
int g_nInterrupts;

void LowPriority(void *) {
    g_log += "low;";
    SignalSema(g_nSemaphore);
    g_log += "low-after-signal;";
}

void HighPriority(void *) {
    g_log += "high;";
}

int CountInterrupt(int) {
    ++g_nInterrupts;
    return 0;
}

void *WakeFromInterrupt(void *pId) {
    usleep(10000);
    iWakeupThread(static_cast<int>(reinterpret_cast<intptr_t>(pId)));
    return nullptr;
}

int Start(void (*pfnEntry)(void *), int nPriority) {
    ThreadParam param = {};
    param.entry = pfnEntry;
    param.initPriority = nPriority;
    const int nId = CreateThread(&param);
    assert(nId > 1);
    assert(StartThread(nId, nullptr) == nId);
    return nId;
}

} // namespace

int main() {
    assert(GetThreadId() == 1);

    // A lower-priority thread runs only once main blocks, and its signal hands the processor back
    // at once.
    SemaParam semaphore = {};
    semaphore.maxCount = 1;
    g_nSemaphore = CreateSema(&semaphore);
    Start(LowPriority, 2);
    g_log += "main;";
    WaitSema(g_nSemaphore);
    g_log += "main-woken;";
    assert(g_log == "main;low;main-woken;");

    // A higher-priority thread runs before StartThread() returns.
    g_log.clear();
    Start(HighPriority, 0);
    g_log += "main;";
    assert(g_log == "high;main;");

    // Main blocking lets the low thread finish.
    g_log.clear();
    assert(PollSema(g_nSemaphore) < 0);
    ChangeThreadPriority(0, 3);
    assert(g_log == "low-after-signal;");
    ChangeThreadPriority(0, 1);

    // A wakeup from interrupt context ends a sleep, and a pending wakeup is not lost.
    pthread_t interrupt;
    pthread_create(&interrupt, nullptr, WakeFromInterrupt, reinterpret_cast<void *>(GetThreadId()));
    SleepThread();
    pthread_join(interrupt, nullptr);
    WakeupThread(GetThreadId());
    SleepThread();

    // Handlers run only while their cause is enabled.
    const int nHandler = AddIntcHandler(INTC_VBLANK_S, CountInterrupt, -1);
    HostRaiseIntc(INTC_VBLANK_S);
    EnableIntc(INTC_VBLANK_S);
    HostRaiseIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, nHandler);
    HostRaiseIntc(INTC_VBLANK_S);
    assert(g_nInterrupts == 1);

    printf("KERNEL_TEST_OK\n");
    return 0;
}
