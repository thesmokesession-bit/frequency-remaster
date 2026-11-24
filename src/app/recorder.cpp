#include "app/recorder.h"

#include "sch/timedcommand.h"
#include "stream/obstream.h"

#ifdef RESONANCE_HOST
#include <cmath>
#include <stdlib.h>

#include "game/controllercmd.h"
#include "sch/command.h"

extern "C" long long HostSongOffsetNs(long long nWallNs); // host/audio/audio.cpp

namespace {

// A playback judges each press when it comes (src/game/catcher.cpp), a live game
// FREQ_INPUT_OFFSET_MS earlier, so a controller reading is recorded that much earlier.
long long RecordedShiftNs(Sch::TimedCommand *pCommand) {
    const char *pszOffset = getenv("FREQ_INPUT_OFFSET_MS");
    if (pszOffset == nullptr || pCommand->mCommand == nullptr ||
        pCommand->mCommand->CmdID() != ControllerCmd::sCmdID) {
        return 0;
    }
    return HostSongOffsetNs(std::llround(atof(pszOffset) * 1e6));
}

} // namespace
#endif
#ifndef RESONANCE_HOST
#line 4
#endif

Sch::Recorder::Recorder(OBStream *pStream) : mStream(pStream) {
}

void Sch::Recorder::Record(Sch::TimedCommand *pCommand) {
#ifdef RESONANCE_HOST
    const long long nShiftNs = RecordedShiftNs(pCommand);
    pCommand->mDueTick.mValue -= nShiftNs;
    pCommand->Save(*mStream);
    pCommand->mDueTick.mValue += nShiftNs;
    mStream->Reset();
    return;
#endif
#ifndef RESONANCE_HOST
#line 9
#endif
    pCommand->Save(*mStream);
    mStream->Reset();
}
