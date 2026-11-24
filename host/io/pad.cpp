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
// Test controls: LUP, LDOWN, LLEFT, LRIGHT and RUP... RRIGHT push the left or right stick fully that
// way; TEXREFS (player 1) prints the loaded textures' referrers once (see HostDumpTexRefs()).

#include <algorithm>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <SDL.h>
#include <libmtap.h>
#include <libpad.h>

#include "../gfx-null/gfxnull.h"
#include "lightbar.h"

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
    unsigned nBit;
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

// The test controls above the button word: the stick pushes, in report byte order (right X, right
// Y, left X, left Y; each pair low then high), and the texture reference dump.
constexpr unsigned kStickShift = 16;
constexpr ButtonName kSticks[] = {{"RLEFT", 1u << 16, -1}, {"RRIGHT", 1u << 17, -1},
                                  {"RUP", 1u << 18, -1},   {"RDOWN", 1u << 19, -1},
                                  {"LLEFT", 1u << 20, -1}, {"LRIGHT", 1u << 21, -1},
                                  {"LUP", 1u << 22, -1},   {"LDOWN", 1u << 23, -1}};
constexpr unsigned kTexRefs = 1u << 24;

struct InputEvent {
    std::string anchor;
    unsigned nFrame;
    unsigned nButtons;
    int nPort;
    bool bFired;
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

extern "C" void HostDumpTexRefs() __attribute__((weak));
extern "C" void HostAutoPause(int nPlayer); // src/game/inputpollerps2.cpp: FREQ_AUTOPAUSE

unsigned ParseButtons(const char *pszButtons, int nLine) {
    unsigned nButtons = 0;
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
        for (const ButtonName &stick : kSticks) {
            if (name == stick.pszName) {
                nButtons |= stick.nBit;
                bKnown = true;
            }
        }
        if (name == "TEXREFS") {
            nButtons |= kTexRefs;
            bKnown = true;
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
            g_events.push_back(InputEvent{anchor, nFrame, ParseButtons(szWord + (bPlayer ? 3 : 0), nLine), nPlayer, false});
        } else {
            fprintf(stderr, "host: FREQ_INPUT line %d is not understood\n", nLine);
        }
    }
    fclose(pFile);
}

// The buttons the script holds on player nPlayer's pad at this vertical blank.
unsigned HeldButtons(int nPlayer) {
    pthread_once(&g_scriptOnce, LoadScript);
    const unsigned nNow = HostVblankCount();
    unsigned nButtons = 0;
    bool bDump = false;
    pthread_mutex_lock(&g_inputLock);
    for (InputEvent &event : g_events) {
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
            bDump |= (event.nButtons & kTexRefs) != 0 && !event.bFired;
            event.bFired = true;
        }
    }
    pthread_mutex_unlock(&g_inputLock);
    if (bDump && HostDumpTexRefs != nullptr) {
        HostDumpTexRefs();
    }
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

int LivePlayers();

// The script's players, or the real controllers' when more are connected (at least one).
int Players() {
    pthread_once(&g_scriptOnce, LoadScript);
    return std::max(g_nPlayers, LivePlayers());
}

bool Multitap() {
    return Players() > 2;
}

// The player whose pad is in nPort/nSlot, or -1.
int PlayerAt(int nPort, int nSlot) {
    const int nPlayers = Players();
    if (nPlayers > 2) {
        return nPort == 0 && nSlot < nPlayers ? nSlot : -1;
    }
    return nSlot == 0 && nPort < nPlayers ? nPort : -1;
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

// Real controllers, through SDL2's game controllers (as Perfect Eye and Rush 2 read the DualSense
// on the PS5), when no input script runs: controller n plays player n+1, in the order they connect
// (the PS5's media remote, which SDL lists too, plays no one). Two players are pads in ports 1 and
// 2, three or four a multitap in port 1, as the script's P2-P4 entries make them.
struct LivePad {
    unsigned nButtons;
    unsigned char abSticks[4]; // right X, right Y, left X, left Y, as the report orders them
};

constexpr int kMaxPlayers = 4;
// One slot per player. A controller keeps its slot while connected; an unplugged one leaves its
// slot empty (that player reads as idle) for the next controller, so nobody changes player and the
// multitap does not change under a running song.
SDL_GameController *g_controllers[kMaxPlayers];

// The motors' bytes scePadSetActDirect() last sent for each player, which a catch tick restores.
unsigned char g_abMotors[kMaxPlayers][2];

// Each player holds what the tunnel last reported (HostPadFrame() once a frame, HostPadFlash() on
// a capture), when the catch tick ends (HostPadTick()), and the light bar last sent.
struct PadFeel {
    float afColor[3];
    float flTick;
    int nTriggers; // Bit 0 is set when L2 plays a lane, bit 1 when R2 does.
    bool bReported;
    Uint32 nReportedAt;
    float afFlash[3];
    bool bFlash;
    Uint32 nFlashAt;
    bool bTick;
    Uint32 nTickEnd;
    SDL_GameController *pLit;
    unsigned char abLit[3];
    Uint32 nLitAt;
};
PadFeel g_feel[kMaxPlayers];

// A report older than this means that no song is drawing (a menu, or the pause screen). The light
// bar then holds the colour steady and the triggers go off.
constexpr Uint32 kFeelStaleMs = 250;
// The light bar is sent at most this often while it changes.
constexpr Uint32 kLightBarIntervalMs = 20;
// The catch tick is a short, light buzz of the high-frequency motor.
constexpr Uint16 kCatchTickStrength = 0x5000;
constexpr Uint32 kCatchTickMs = 40;

// Reads FREQ_LIGHTBAR or FREQ_TRIGGERS each frame, so that a change in the options applies at once.
bool SettingOn(const char *pszKey) {
    const char *pszValue = getenv(pszKey);
    return pszValue == nullptr || strcmp(pszValue, "0") != 0;
}

bool LiveInputOn() {
    static const bool s_bOn = getenv("FREQ_INPUT") == nullptr &&
                              SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
    return s_bOn;
}

unsigned char StickByte(SDL_GameController *pController, SDL_GameControllerAxis axis) {
    return static_cast<unsigned char>((SDL_GameControllerGetAxis(pController, axis) + 32768) >> 8);
}

// Once a frame (port 1's read): finds controllers connected or paired since, and drops gone ones.
void UpdateControllers() {
    if (!LiveInputOn()) {
        return;
    }
    static int s_nJoysticks = -1;
    SDL_GameControllerUpdate();
    // A controller gone since (even one swapped for another between frames, which leaves the count
    // as it was) frees its slot.
    bool bChanged = SDL_NumJoysticks() != s_nJoysticks;
    for (int p = 0; p < kMaxPlayers; ++p) {
        if (g_controllers[p] != nullptr && !SDL_GameControllerGetAttached(g_controllers[p])) {
            fprintf(stderr, "host: player %d's controller left\n", p + 1);
            HostAutoPause(p + 1);
            SDL_GameControllerClose(g_controllers[p]);
            g_controllers[p] = nullptr;
            bChanged = true;
        }
    }
    if (!bChanged) {
        return;
    }
    s_nJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < s_nJoysticks; ++i) {
        const char *pszName = SDL_GameControllerNameForIndex(i);
        if (!SDL_IsGameController(i) || (pszName != nullptr && strstr(pszName, "Remote") != nullptr)) {
            continue;
        }
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        int nFree = -1;
        bool bOpen = false;
        for (int p = kMaxPlayers - 1; p >= 0; --p) {
            if (g_controllers[p] == nullptr) {
                nFree = p;
            } else if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controllers[p])) == id) {
                bOpen = true;
            }
        }
        if (bOpen || nFree < 0) {
            continue;
        }
        if ((g_controllers[nFree] = SDL_GameControllerOpen(i)) != nullptr) {
            fprintf(stderr, "host: controller %s plays player %d\n", SDL_GameControllerName(g_controllers[nFree]),
                    nFree + 1);
        }
    }
}

// The highest player with a controller, so a player who unplugs keeps the others' numbers.
int LivePlayers() {
    for (int p = kMaxPlayers; p > 0; --p) {
        if (g_controllers[p - 1] != nullptr) {
            return p;
        }
    }
    return 0;
}

LivePad ReadLivePad(int nPlayer) {
    LivePad pad = {0, {kAnalogCentre, kAnalogCentre, kAnalogCentre, kAnalogCentre}};
    if (nPlayer < 0 || nPlayer >= LivePlayers()) {
        return pad;
    }
    SDL_GameController *pController = g_controllers[nPlayer];
    if (pController == nullptr) {
        return pad;
    }
    static const struct {
        SDL_GameControllerButton button;
        unsigned nBit;
    } kMap[] = {{SDL_CONTROLLER_BUTTON_A, 0x0040},          {SDL_CONTROLLER_BUTTON_B, 0x0020},
                {SDL_CONTROLLER_BUTTON_X, 0x0080},          {SDL_CONTROLLER_BUTTON_Y, 0x0010},
                {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0004}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0008},
                {SDL_CONTROLLER_BUTTON_BACK, 0x0100},       {SDL_CONTROLLER_BUTTON_START, 0x0800},
                {SDL_CONTROLLER_BUTTON_LEFTSTICK, 0x0200},  {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0400},
                {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x1000},    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x2000},
                {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x4000},  {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x8000}};
    constexpr Sint16 kTriggerPressed = 8000;
    for (const auto &entry : kMap) {
        if (SDL_GameControllerGetButton(pController, entry.button) != 0) {
            pad.nButtons |= entry.nBit;
        }
    }
    if (SDL_GameControllerGetAxis(pController, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerPressed) {
        pad.nButtons |= 0x0001; // L2
    }
    if (SDL_GameControllerGetAxis(pController, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerPressed) {
        pad.nButtons |= 0x0002; // R2
    }
    pad.abSticks[0] = StickByte(pController, SDL_CONTROLLER_AXIS_RIGHTX);
    pad.abSticks[1] = StickByte(pController, SDL_CONTROLLER_AXIS_RIGHTY);
    pad.abSticks[2] = StickByte(pController, SDL_CONTROLLER_AXIS_LEFTX);
    pad.abSticks[3] = StickByte(pController, SDL_CONTROLLER_AXIS_LEFTY);
    return pad;
}

// Drives player nPlayer's motors from the bytes scePadSetActDirect() took, low frequency (the big
// motor) from byte 1 and high from byte 0. Each send lasts one second.
void RumbleMotors(int nPlayer) {
    SDL_GameControllerRumble(g_controllers[nPlayer],
                             static_cast<Uint16>(g_abMotors[nPlayer][1] * 257),
                             g_abMotors[nPlayer][0] != 0 ? 0xc000 : 0,
                             1000);
}

#ifdef __PROSPERO__
// The adaptive triggers go through the console's own pad calls, as in Perfect Eye, since SDL's PS5
// driver has none. The layout is ScePadTriggerEffectParam, with command[0] for L2 and command[1]
// for R2.
struct TriggerEffect {
    unsigned char nTriggerMask;
    unsigned char abPadding[7];
    struct {
        unsigned nMode;
        unsigned char abPadding[4];
        unsigned char abData[48];
    } command[2];
};
static_assert(sizeof(TriggerEffect) == 120, "ScePadTriggerEffectParam is 120 bytes");

extern "C" int scePadGetHandle(int nUser, int nType, int nIndex);
extern "C" int scePadSetTriggerEffect(int nHandle, const TriggerEffect *pEffect);
extern "C" int sceUserServiceGetLoginUserIdList(int anUsers[4]);
extern "C" int sceUserServiceGetUserName(int nUser, char *pszName, size_t nSize);

// The weapon effect (mode 2) resists from zone 2 to zone 3 of the trigger's ten and breaks at its
// end. ReadLivePad()'s kTriggerPressed (a quarter of the travel) lies inside it, so the press
// counts as the click is felt. Strength runs from 0 to 8.
constexpr unsigned kTriggerWeapon = 2;
constexpr unsigned char kTriggerStart = 2;
constexpr unsigned char kTriggerEnd = 3;
constexpr unsigned char kTriggerStrength = 5;

// Finds the pad's system handle. SDL names each pad after its user and opens it as (user, standard
// port, 0), which scePadGetHandle() hands back. The result is -1 when no logged-in user has the
// pad's name.
int PadHandle(SDL_GameController *pController) {
    int anUsers[4];
    char szName[64];
    const char *pszWant = SDL_GameControllerName(pController);
    if (pszWant == nullptr || sceUserServiceGetLoginUserIdList(anUsers) != 0) {
        return -1;
    }
    for (const int nUser : anUsers) {
        if (nUser != -1 && sceUserServiceGetUserName(nUser, szName, sizeof(szName)) == 0 &&
            strcmp(szName, pszWant) == 0) {
            return scePadGetHandle(nUser, 0, 0);
        }
    }
    return -1;
}

// Puts the weapon effect on the triggers in nTriggers (bit 0 L2, bit 1 R2) and turns the others
// off, sent only on a change. A handle not found is looked up again a second later; one a send
// fails on (a pad that went) is looked up again.
void SetTriggers(int nPlayer, SDL_GameController *pController, int nTriggers, Uint32 nNow) {
    static SDL_GameController *s_apFor[kMaxPlayers];
    static int s_anHandle[kMaxPlayers];
    static int s_anSent[kMaxPlayers];
    static Uint32 s_anRetryAt[kMaxPlayers];
    if (s_apFor[nPlayer] != pController) {
        s_apFor[nPlayer] = pController;
        s_anHandle[nPlayer] = -1;
        s_anSent[nPlayer] = -1;
        s_anRetryAt[nPlayer] = nNow;
    }
    if (nTriggers == s_anSent[nPlayer]) {
        return;
    }
    if (s_anHandle[nPlayer] < 0) {
        if (!SDL_TICKS_PASSED(nNow, s_anRetryAt[nPlayer])) {
            return;
        }
        s_anHandle[nPlayer] = PadHandle(pController);
        s_anRetryAt[nPlayer] = nNow + 1000;
        fprintf(stderr,
                "host: player %d's triggers: pad handle %d\n",
                nPlayer + 1,
                s_anHandle[nPlayer]);
        if (s_anHandle[nPlayer] < 0) {
            return;
        }
    }
    TriggerEffect effect = {};
    effect.nTriggerMask = 0x03;
    for (int t = 0; t < 2; ++t) {
        if ((nTriggers & (1 << t)) != 0) {
            effect.command[t].nMode = kTriggerWeapon;
            effect.command[t].abData[0] = kTriggerStart;
            effect.command[t].abData[1] = kTriggerEnd;
            effect.command[t].abData[2] = kTriggerStrength;
        }
    }
    const int nResult = scePadSetTriggerEffect(s_anHandle[nPlayer], &effect);
    if (nResult == 0) {
        s_anSent[nPlayer] = nTriggers;
    } else {
        fprintf(stderr,
                "host: player %d's triggers refused (0x%08x)\n",
                nPlayer + 1,
                static_cast<unsigned>(nResult));
        s_anHandle[nPlayer] = -1;
    }
}
#endif

// Runs once a frame after UpdateControllers(). It sets each player's light bar and triggers from
// the tunnel's reports and restores the game's own motors once a catch tick ends.
void UpdateFeel() {
    const Uint32 nNow = SDL_GetTicks();
    const bool bLightBar = SettingOn("FREQ_LIGHTBAR");
    for (int p = 0; p < kMaxPlayers; ++p) {
        PadFeel &feel = g_feel[p];
        SDL_GameController *pController = g_controllers[p];
        if (pController == nullptr) {
            feel.pLit = nullptr;
            continue;
        }
        if (feel.bTick && SDL_TICKS_PASSED(nNow, feel.nTickEnd)) {
            feel.bTick = false;
            if (g_abMotors[p][0] != 0 || g_abMotors[p][1] != 0) {
                RumbleMotors(p);
            }
        }
        const bool bPlaying = feel.bReported && nNow - feel.nReportedAt < kFeelStaleMs;
        if (bLightBar && feel.bReported) {
            const float flFlash = feel.bFlash ? LightBarFlash(nNow - feel.nFlashAt) : 0.0f;
            unsigned char abRgb[3];
            LightBarRgb(feel.afColor, feel.flTick, !bPlaying, feel.afFlash, flFlash, abRgb);
            if (pController != feel.pLit || (memcmp(abRgb, feel.abLit, sizeof(abRgb)) != 0 &&
                                             nNow - feel.nLitAt >= kLightBarIntervalMs)) {
                SDL_GameControllerSetLED(pController, abRgb[0], abRgb[1], abRgb[2]);
                memcpy(feel.abLit, abRgb, sizeof(abRgb));
                feel.pLit = pController;
                feel.nLitAt = nNow;
            }
        }
#ifdef __PROSPERO__
        SetTriggers(
            p, pController, bPlaying && SettingOn("FREQ_TRIGGERS") ? feel.nTriggers : 0, nNow);
#endif
    }
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

// The motors, on the player's own controller: actuator 0 (the small motor, on or off) and 1 (the
// big one, 0-255), as the game aligns them. ponytail: the default alignment (byte n drives
// actuator n); a game that aligns them otherwise would want scePadSetActAlign's table kept.
int scePadSetActDirect(int nPort, int nSlot, const unsigned char *pData) {
    const int nPlayer = PlayerAt(nPort, nSlot);
    if (pData != nullptr && nPlayer >= 0 && nPlayer < LivePlayers() &&
        g_controllers[nPlayer] != nullptr) {
        static Uint32 s_anSent[kMaxPlayers];
        const Uint32 nNow = SDL_GetTicks();
        // Sent on a change, and again every half second while a motor stays on, since each send
        // lasts one second (a still pad stops within it if the game stops calling).
        if (g_abMotors[nPlayer][0] != pData[0] || g_abMotors[nPlayer][1] != pData[1] ||
            ((pData[0] != 0 || pData[1] != 0) && nNow - s_anSent[nPlayer] >= 500)) {
            g_abMotors[nPlayer][0] = pData[0];
            g_abMotors[nPlayer][1] = pData[1];
            s_anSent[nPlayer] = nNow;
            RumbleMotors(nPlayer);
        }
    }
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
    if (nPort == 0 && nSlot == 0) {
        UpdateControllers();
        UpdateFeel();
    }
    const int nPlayer = PlayerAt(nPort, nSlot);
    const LivePad live = ReadLivePad(nPlayer);
    const unsigned nHeld = HeldButtons(nPlayer) | live.nButtons;
    const unsigned short nWire = static_cast<unsigned short>(~nHeld);
    pData[1] = kDualShockPressureMode;
    pData[2] = static_cast<unsigned char>(nWire >> 8);
    pData[3] = static_cast<unsigned char>(nWire);
    memcpy(pData + 4, live.abSticks, 4);
    for (int nAxis = 0; nAxis < 4; ++nAxis) {
        const unsigned nPush = (nHeld >> (kStickShift + 2 * nAxis)) & 3;
        if (nPush == 1 || nPush == 2) {
            pData[4 + nAxis] = nPush == 1 ? 0x00 : 0xff;
        }
    }
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

// Takes the tunnel's report for player nPlayer at each drawn frame (AppTunnel::SetFrame()). It
// carries the player's colour (components 0 to 1, the colour-blind palette's when it is on), the
// song tick drawn, and the triggers that play a lane (bit 0 L2, bit 1 R2).
void HostPadFrame(
    int nPlayer, float flRed, float flGreen, float flBlue, float flTick, int nTriggers) {
    if (nPlayer < 0 || nPlayer >= kMaxPlayers) {
        return;
    }
    PadFeel &feel = g_feel[nPlayer];
    feel.afColor[0] = flRed;
    feel.afColor[1] = flGreen;
    feel.afColor[2] = flBlue;
    feel.nTriggers = nTriggers;
    // A song tick that stops moving (the pause screen) lets the report go stale.
    if (!feel.bReported || flTick != feel.flTick) {
        feel.nReportedAt = SDL_GetTicks();
    }
    feel.flTick = flTick;
    feel.bReported = true;
}

// Flashes player nPlayer's light bar in the colour of a section the player captured
// (AppTunnel::OnSectionCaptured()).
void HostPadFlash(int nPlayer, float flRed, float flGreen, float flBlue) {
    if (nPlayer < 0 || nPlayer >= kMaxPlayers) {
        return;
    }
    PadFeel &feel = g_feel[nPlayer];
    feel.afFlash[0] = flRed;
    feel.afFlash[1] = flGreen;
    feel.afFlash[2] = flBlue;
    feel.bFlash = true;
    feel.nFlashAt = SDL_GetTicks();
}

// Plays a light tick on the high-frequency motor, over whatever the game's motors do, for a gem
// player nPlayer caught. AppTunnel::OnCatch() calls it only while the game's force feedback runs.
void HostPadTick(int nPlayer) {
    if (nPlayer < 0 || nPlayer >= kMaxPlayers || g_controllers[nPlayer] == nullptr) {
        return;
    }
    const Uint16 nHigh =
        std::max<Uint16>(g_abMotors[nPlayer][0] != 0 ? 0xc000 : 0, kCatchTickStrength);
    SDL_GameControllerRumble(g_controllers[nPlayer],
                             static_cast<Uint16>(g_abMotors[nPlayer][1] * 257),
                             nHigh,
                             kCatchTickMs);
    g_feel[nPlayer].bTick = true;
    g_feel[nPlayer].nTickEnd = SDL_GetTicks() + kCatchTickMs;
}

} // extern "C"
