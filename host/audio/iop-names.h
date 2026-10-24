// Forced into the native build of the IOP sound driver (src/ezmidi). The IOP kernel and SIF calls
// it makes share names with Emotion Engine calls the game makes, so they link under these names to
// host/audio/audio.cpp and host/kernel/iop.cpp.
#pragma once

#define CpuEnableIntr IopCpuEnableIntr
#define EnableIntr IopEnableIntr
#define CreateThread IopCreateThread
#define StartThread IopStartThread
#define GetThreadId IopGetThreadId
#define SleepThread IopSleepThread
#define iWakeupThread IopiWakeupThread
#define GetSystemTime IopGetSystemTime
#define USec2SysClock IopUSec2SysClock
#define AllocHardTimer IopAllocHardTimer
#define FreeHardTimer IopFreeHardTimer
#define SetTimerHandler IopSetTimerHandler
#define SetupHardTimer IopSetupHardTimer
#define StartHardTimer IopStartHardTimer
#define StopHardTimer IopStopHardTimer
#define sceSifInitRpc IopSifInitRpc
#define sceSifSetRpcQueue IopSifSetRpcQueue
#define sceSifRegisterRpc IopSifRegisterRpc
#define sceSifRpcLoop IopSifRpcLoop
