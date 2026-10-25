// The controllers: a DualShock 2 in port 1, nothing in port 2, and no multitap. The pad in port 1
// holds the buttons the input script FREQ_INPUT names, and nothing otherwise.
//
// The script is text, one entry per line:
//
//     # a comment
//     after TITLE_SCREEN_REACHED
//     180 START
//
// "<frame> <BUTTON>[+<BUTTON>...]" holds the buttons for kHoldFrames vertical blanks from that
// blank. A test control for local multiplayer: "<frame> P<n>:<BUTTON>..." holds them on player n's
// pad. P2 alone connects a second pad in port 2; P3 or P4 connect a multitap in port 1 instead,
// with players 1 to 4 in its slots A to D. Without such entries only the pad in port 1 exists. Frames count from the start, or from the blank of the last "after <BREADCRUMB>" line's
// breadcrumb (see HostBreadcrumb()); entries under an "after" line wait for its breadcrumb.

#include <algorithm>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <libmtap.h>
#include <libpad.h>

#include "../gfx-null/gfxnull.h"

namespace {

constexpr int kHoldFrames = 10;
constexpr int kReportSize = 32;
constexpr int kAliveSeconds = 60;
constexpr int kDemoAliveSeconds = 120;

// The controller identifies as a DualShock 2 with two motors and pressure-sensitive buttons.
constexpr int kDualShockId = 7;
constexpr int kActuatorCount = 2;
constexpr unsigned char kDualShockPressureMode = 0x79;
constexpr unsigned char kAnalogCentre = 0x80;
constexpr unsigned char kFullPressure = 0xff;
constexpr int kPressureButtonCount = 12;

struct ButtonName {
    const char *pszName;
    unsigned short nBit;
    int nPressureIndex;
};

// The report's button word, high byte first as the pad sends it, and each button's pressure byte.
constexpr ButtonName kButtons[] = {{"SELECT", 0x0100, -1},
                                   {"L3", 0x0200, -1},
                                   {"R3", 0x0400, -1},
                                   {"START", 0x0800, -1},
                                   {"UP", 0x1000, 4},
                                   {"RIGHT", 0x2000, 0},
                                   {"DOWN", 0x4000, 5},
                                   {"LEFT", 0x8000, 1},
                                   {"L2", 0x0001, 10},
                                   {"R2", 0x0002, 11},
                                   {"L1", 0x0004, 8},
                                   {"R1", 0x0008, 9},
                                   {"TRIANGLE", 0x0010, 6},
                                   {"CIRCLE", 0x0020, 7},
                                   {"CROSS", 0x0040, 2},
                                   {"SQUARE", 0x0080, 3}};

struct InputEvent {
    std::string anchor;
    unsigned nFrame;
    unsigned short nButtons;
    int nPort;
};

struct Anchor {
    std::string name;
    unsigned nFrame;
};

pthread_mutex_t g_inputLock = PTHREAD_MUTEX_INITIALIZER;
std::vector<InputEvent> g_events;
std::vector<Anchor> g_anchors;
pthread_once_t g_scriptOnce = PTHREAD_ONCE_INIT;
int g_nPlayers = 1; // pads the script plays; above 2, through a multitap in port 1

unsigned short ParseButtons(const char *pszButtons, int nLine) {
    unsigned short nButtons = 0;
    std::string remaining(pszButtons);
    std::string::size_type nStart = 0;
    while (nStart <= remaining.size()) {
        std::string::size_type nEnd = remaining.find('+', nStart);
        if (nEnd == std::string::npos) {
            nEnd = remaining.size();
        }
        const std::string name = remaining.substr(nStart, nEnd - nStart);
        bool bKnown = false;
        for (const ButtonName &button : kButtons) {
            if (name == button.pszName) {
                nButtons |= button.nBit;
                bKnown = true;
            }
        }
        if (!bKnown) {
            fprintf(stderr, "host: FREQ_INPUT line %d: unknown button %s\n", nLine, name.c_str());
        }
        nStart = nEnd + 1;
    }
    return nButtons;
}

void LoadScript() {
    const char *pszPath = getenv("FREQ_INPUT");
    if (pszPath == nullptr) {
        return;
    }
    // fopen() opens the game's files, so the host file is opened by descriptor.
    const int nFile = open(pszPath, O_RDONLY | O_CLOEXEC);
    FILE *pFile = nFile >= 0 ? fdopen(nFile, "r") : nullptr;
    if (pFile == nullptr) {
        fprintf(stderr, "host: cannot read FREQ_INPUT %s\n", pszPath);
        return;
    }
    std::string anchor;
    char szLine[256];
    for (int nLine = 1; fgets(szLine, sizeof(szLine), pFile) != nullptr; ++nLine) {
        char szWord[128];
        unsigned nFrame;
        if (szLine[0] == '#' || sscanf(szLine, "%127s", szWord) != 1) {
            continue;
        }
        if (strcmp(szWord, "after") == 0 && sscanf(szLine, "after %127s", szWord) == 1) {
            anchor = szWord;
        } else if (sscanf(szLine, "%u %127s", &nFrame, szWord) == 2) {
            const bool bPlayer = szWord[0] == 'P' && szWord[1] >= '2' && szWord[1] <= '4' && szWord[2] == ':';
            const int nPlayer = bPlayer ? szWord[1] - '1' : 0;
            g_nPlayers = std::max(g_nPlayers, nPlayer + 1);
            g_events.push_back(InputEvent{anchor, nFrame, ParseButtons(szWord + (bPlayer ? 3 : 0), nLine), nPlayer});
        } else {
            fprintf(stderr, "host: FREQ_INPUT line %d is not understood\n", nLine);
        }
    }
    fclose(pFile);
}

// The buttons the script holds on player nPlayer's pad at this vertical blank.
unsigned short HeldButtons(int nPlayer) {
    pthread_once(&g_scriptOnce, LoadScript);
    const unsigned nNow = HostVblankCount();
    unsigned short nButtons = 0;
    pthread_mutex_lock(&g_inputLock);
    for (const InputEvent &event : g_events) {
        if (event.nPort != nPlayer) {
            continue;
        }
        unsigned nOrigin = 0;
        bool bArmed = event.anchor.empty();
        for (const Anchor &anchor : g_anchors) {
            if (anchor.name == event.anchor) {
                nOrigin = anchor.nFrame;
                bArmed = true;
            }
        }
        const unsigned nStart = nOrigin + event.nFrame;
        if (bArmed && nNow >= nStart && nNow < nStart + kHoldFrames) {
            nButtons |= event.nButtons;
        }
    }
    pthread_mutex_unlock(&g_inputLock);
    return nButtons;
}

struct AliveReport {
    const char *pszAnchor;
    int nSeconds;
    const char *pszReport;
};

AliveReport g_aliveReports[] = {
    {"MAIN_MENU_REACHED", kAliveSeconds, "ALIVE_60S_AFTER_MENU"},
    {"DEMO_STARTED", kDemoAliveSeconds, "ALIVE_120S_AFTER_DEMO"},
};

void *ReportAlive(void *pReport) {
    const auto *pAlive = static_cast<const AliveReport *>(pReport);
    sleep(pAlive->nSeconds);
    fprintf(stderr, "%s\n", pAlive->pszReport);
    return nullptr;
}

bool Multitap() {
    pthread_once(&g_scriptOnce, LoadScript);
    return g_nPlayers > 2;
}

// The player whose pad is in nPort/nSlot, or -1.
int PlayerAt(int nPort, int nSlot) {
    if (Multitap()) {
        return nPort == 0 && nSlot < g_nPlayers ? nSlot : -1;
    }
    return nSlot == 0 && nPort < g_nPlayers ? nPort : -1;
}

bool Connected(int nPort, int nSlot) {
    return PlayerAt(nPort, nSlot) >= 0;
}

// A mode or actuator request reports busy once, as the pad takes a frame to answer, and then
// complete. Only the connected pad gets requests.
bool g_bRequestBusy = false;

int StartRequest() {
    g_bRequestBusy = true;
    return 1;
}

} // namespace

// Reports a milestone of the game on stderr. The input script can time its entries from it, and
// MAIN_MENU_REACHED and DEMO_STARTED start a report 60 and 120 seconds later that the program is
// still running.
void HostBreadcrumb(const char *pszName) {
    fprintf(stderr, "%s\n", pszName);
    HostEvent("breadcrumb %s", pszName);
    pthread_mutex_lock(&g_inputLock);
    g_anchors.push_back(Anchor{pszName, HostVblankCount()});
    pthread_mutex_unlock(&g_inputLock);
    for (AliveReport &report : g_aliveReports) {
        pthread_t thread;
        if (strcmp(pszName, report.pszAnchor) != 0) {
            continue;
        }
        if (pthread_create(&thread, nullptr, ReportAlive, &report) == 0) {
            pthread_detach(thread);
        }
    }
}

extern "C" {

int scePadInit(int) {
    return 1;
}

int scePadEnd() {
    return 1;
}

int scePadPortOpen(int, int, scePadDmaFrame *) {
    return 1;
}

int scePadPortClose(int, int) {
    return 1;
}

int scePadGetState(int nPort, int nSlot) {
    return Connected(nPort, nSlot) ? scePadStateStable : scePadStateDiscon;
}

int scePadGetReqState(int, int) {
    if (g_bRequestBusy) {
        g_bRequestBusy = false;
        return scePadReqStateBusy;
    }
    return scePadReqStateComplete;
}

int scePadInfoMode(int nPort, int nSlot, int nTerm, int nOffset) {
    if (!Connected(nPort, nSlot)) {
        return 0;
    }
    switch (nTerm) {
    case InfoModeCurID:
    case InfoModeCurExID:
        return kDualShockId;
    case InfoModeCurExOffs:
        return 1;
    case InfoModeIdTable:
        return nOffset == -1 ? 2 : (nOffset == 0 ? 4 : kDualShockId);
    default:
        return 0;
    }
}

int scePadSetMainMode(int, int, int, int) {
    return StartRequest();
}

int scePadInfoAct(int nPort, int nSlot, int nActuator, int) {
    if (!Connected(nPort, nSlot)) {
        return 0;
    }
    return nActuator == -1 ? kActuatorCount : 1;
}

int scePadSetActAlign(int, int, const unsigned char *) {
    return StartRequest();
}

int scePadSetActDirect(int, int, const unsigned char *) {
    return 1;
}

int scePadInfoPressMode(int nPort, int nSlot) {
    return Connected(nPort, nSlot) ? 1 : 0;
}

int scePadEnterPressMode(int, int) {
    return StartRequest();
}

// The report: status, mode, the button word (a pressed button reads 0), the four sticks, and a
// pressure byte per pressure-sensitive button.
int scePadRead(int nPort, int nSlot, unsigned char *pData) {
    memset(pData, 0, kReportSize);
    if (!Connected(nPort, nSlot)) {
        return 0;
    }
    const unsigned short nHeld = HeldButtons(PlayerAt(nPort, nSlot));
    const unsigned short nWire = static_cast<unsigned short>(~nHeld);
    pData[1] = kDualShockPressureMode;
    pData[2] = static_cast<unsigned char>(nWire >> 8);
    pData[3] = static_cast<unsigned char>(nWire);
    memset(pData + 4, kAnalogCentre, 4);
    for (const ButtonName &button : kButtons) {
        if (button.nPressureIndex >= 0 && (nHeld & button.nBit) != 0) {
            pData[8 + button.nPressureIndex] = kFullPressure;
        }
    }
    static_assert(8 + kPressureButtonCount <= kReportSize, "the pressures fit the report");
    return kReportSize;
}

int sceMtapInit() {
    return 1;
}

int sceMtapPortOpen(int) {
    return 1;
}

int sceMtapPortClose(int) {
    return 1;
}

int sceMtapGetConnection(int nPort) {
    return nPort == 0 && Multitap() ? 1 : 0;
}

} // extern "C"
