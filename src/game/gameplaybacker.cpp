#include "game/gameplaybacker.h"

#include "app/application.h"
#include "app/scheduler.h"
#include "game/gamemanagerimpl.h"
#include "msg/hxstrtransfer.h"
#include "os/hostmode.h"
#include "os/hxstr.h"
#include "stream/ibfilestream.h"

#ifdef RESONANCE_HOST
#include <fcntl.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include "stream/iobmemstream.h"
#endif
#ifndef RESONANCE_HOST
#line 9
#endif

GamePlaybacker::GamePlaybacker(const HxStr &file, GameManagerImpl *pManager, int)
    : mManager(pManager) {
#ifdef RESONANCE_HOST
    // A kept best run (host/core/progress.cpp) is a host file, which the game's fopen() does not
    // open; the attract demo is on the disc.
    const char *pszPath = file.mStr != nullptr ? file.mStr : "";
    const int nFile = strstr(pszPath, "/replays/") != nullptr ? open(pszPath, O_RDONLY) : -1;
    if (nFile >= 0) {
        std::string data;
        char abBuffer[65536];
        for (ssize_t nRead; (nRead = read(nFile, abBuffer, sizeof(abBuffer))) > 0;) {
            data.append(abBuffer, nRead);
        }
        close(nFile);
        IOBMemStream memory(data.data(), static_cast<int>(data.size()));
        memory.Seek(0, kStreamSeekSet);
        HxStr text;
        LoadHxStr(memory, text);
        LoadHxStr(memory, text);
        LoadHxStr(memory, text);
        mManager->Load(&memory);
        Application::shared()->GetWatchdog()->StartPlayback(memory);
        return;
    }
#endif
#ifndef RESONANCE_HOST
#line 12
#endif
    IBFileStream stream(MakeFreqPath(file));

    // Yes, the binary reads the first two strings into the same string and discards all three.
    HxStr first;
    LoadHxStr(stream, first);
    LoadHxStr(stream, first);
    HxStr third;
    LoadHxStr(stream, third);

    mManager->Load(&stream);
    Application::shared()->GetWatchdog()->StartPlayback(stream);
}

GamePlaybacker::~GamePlaybacker() {
    Application::shared()->GetWatchdog()->StopRecOrPlayback();
}
