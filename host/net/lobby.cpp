// Online play and the friends board in the menus. Main > Multiplayer first offers LOCAL, ONLINE,
// SCOREBOARD, and TIPS on the player count screen's four buttons, and LOCAL gives the buttons back
// their counts. ONLINE turns the buttons into the role (HOST or JOIN) and the three words of a room
// code. Up and Down pick a button, Left and Right change it, and Cross opens the room. The lobby
// then lists the room's players on the four buttons, with the code in the title and the status on
// the help line. In the lobby Cross goes to the main menu with the room open (the host picks a song
// there as it does without a lobby), and Triangle exits the room. SCOREBOARD sets the friends
// board's code the same way. The settings file records the last code and role, and rejoining is
// one press. MetLocNumPlayScreen calls the HostOnlinePage functions below, and HostOnlinePoll()
// calls HostOnlinePageFrame().

#include <ctype.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

#include <openssl/rand.h>

#include "app/playsound.h"
#include "gfx/hostframe.h"
#include "met/metbuttonlist.h"
#include "met/methelpscreen.h"
#include "met/metrenderer.h"
#include "met/metscreen.h"
#include "met/metscreentitlescreen.h"
#include "online.h"
#include "os/hxstr.h"
#include "rnd/button.h"
#include "rnd/text.h"
#include "script/scripteval.h"

extern "C" int HostSaveSetting(const char *pszKey, const char *pszValue);    // host/core/settings.c
extern "C" unsigned HostNetRoster(const char *apszNames[4], int anRttMs[4]); // netgame.cpp
extern "C" void HostNetLeave(void);                                          // netgame.cpp
extern "C" int HostBoardCodeChanged(void);                                   // online.cpp
extern "C" int HostBoardStatus(char *pszRank, int nCapacity);                // online.cpp
void HostBreadcrumb(const char *pszName);                                    // host/io/pad.cpp

namespace {

// Short common words, unlike each other when spoken, for a code that is easy to tell a friend.
const char *const kWords[] = {
    "ACE",  "ANT",  "APE",  "ARCH", "BASS", "BEAR", "BELL", "BIRD", "BLUE", "BOLT", "BUG",
    "CAKE", "CAT",  "COIN", "CORN", "CRAB", "CUBE", "DEER", "DISC", "DOG",  "DOVE", "DRUM",
    "DUCK", "ECHO", "EGG",  "ELK",  "FERN", "FIRE", "FISH", "FLAG", "FOX",  "FROG", "GOAT",
    "GOLD", "HAT",  "HAWK", "HILL", "HORN", "ICE",  "INK",  "JAZZ", "JET",  "KITE", "LAMP",
    "LAVA", "LEAF", "LION", "LOCK", "MOON", "MOTH", "NEON", "NEST", "OAK",  "OWL",  "PEAR",
    "PINE", "PLUM", "RAIN", "ROCK", "SAND", "SNOW", "STAR", "WAVE", "WOLF"};
constexpr int kWordCount = sizeof(kWords) / sizeof(kWords[0]);
constexpr int kCodeWords = 3;
constexpr int kButtons = 4; // the player count screen's: 2, 3, and 4 players, then the tips
constexpr int kTipsButton = 3;
constexpr int kMaxPlayers = 4;
constexpr double kNoHostAfterMs = 45000;
constexpr size_t kShownName = 8; // a pill fits about twelve letters: a name, then "YOU" or "34MS"

enum Stage { kStageChoose, kStageLocal, kStageSetup, kStageBoard, kStageLobby, kStageGone };
enum Choice { kChoiceLocal, kChoiceOnline, kChoiceBoard };

struct Page {
    Stage stage = kStageGone;
    MetRenderer *pRenderer = nullptr;
    MetButtonList *pButtons = nullptr;
    const std::vector<HxStr> *pHelpKeys = nullptr; // the screen's help per button
    HxStr title;                                   // the screen's title
    HxStr aLabels[kButtons];                       // the screen's labels
    bool bLabelsSaved = false;
    int nChoice = kChoiceLocal;
    bool bHost = true;
    bool bBoardOn = true;
    int anWords[kCodeWords] = {};
    std::string room; // the open room's code as the lobby shows it
    double flOpenedMs = 0;
    int nSessions = 0;         // rooms opened from the page, for the breadcrumbs
    bool bLinkedCrumb = false; // ONLINE_LINKED_<n> reported for this room
    std::string lastLobby;     // the lobby the last event reported
    std::string lastStatus;    // the status the help line shows
};

Page g_page;

double NowMs() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) * 1e3 + static_cast<double>(now.tv_nsec) / 1e6;
}

void SetLabel(int nButton, const std::string &text) {
    Rnd::Button *pButton = g_page.pButtons->GetButton(nButton);
    if (pButton != nullptr && pButton->mText != nullptr) {
        pButton->mText->SetText(HxStr(text.c_str()));
    }
}

void HelpKey(const HxStr &key) {
    MetHelpScreen::SetText(key, g_page.pRenderer->mAnimationFrame);
}

// The help line shows the gMetHelpStrings entry of a key. Help() adds an entry for each text.
void Help(const std::string &text) {
    static std::map<std::string, std::string> s_keys;
    std::string &key = s_keys[text];
    if (key.empty()) {
        key = "online_" + std::to_string(s_keys.size());
        std::string quoted = "'";
        for (const char c : text) {
            quoted += c == '\'' || c == '\\' ? std::string("\\") + c : std::string(1, c);
        }
        EvalScriptExpression(
            HxStr(("gMetHelpStrings.update({'" + key + "': " + quoted + "'})").c_str()));
    }
    HelpKey(HxStr(key.c_str()));
}

std::string Code() {
    std::string code;
    for (int i = 0; i < kCodeWords; ++i) {
        code += (i == 0 ? "" : " ") + std::string(kWords[g_page.anWords[i]]);
    }
    return code;
}

std::string Lower(std::string text) {
    for (char &c : text) {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

std::string Upper(std::string text) {
    for (char &c : text) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

// The words of a code, when each is one of kWords.
bool ParseCode(const char *pszCode, int anWords[kCodeWords]) {
    char aszWords[kCodeWords + 1][16] = {};
    if (pszCode == nullptr ||
        sscanf(
            pszCode, "%15s %15s %15s %15s", aszWords[0], aszWords[1], aszWords[2], aszWords[3]) !=
            kCodeWords) {
        return false;
    }
    for (int i = 0; i < kCodeWords; ++i) {
        anWords[i] = -1;
        for (int n = 0; n < kWordCount; ++n) {
            anWords[i] = Upper(aszWords[i]) == kWords[n] ? n : anWords[i];
        }
        if (anWords[i] < 0) {
            return false;
        }
    }
    return true;
}

// A code from the settings, or else a random one. Two rooms seldom share a random code.
void LoadCode(const char *pszKey) {
    if (!ParseCode(getenv(pszKey), g_page.anWords)) {
        unsigned char abRandom[kCodeWords];
        RAND_bytes(abRandom, sizeof(abRandom));
        for (int i = 0; i < kCodeWords; ++i) {
            g_page.anWords[i] = abRandom[i] % kWordCount;
        }
    }
}

// The help line is about 45 characters wide.
std::string BoardStatus() {
    const char *pszBoard = getenv("FREQ_BOARD");
    if (pszBoard == nullptr || pszBoard[0] == '\0') {
        return "the board is off, cross sets these words";
    }
    char szRank[256];
    const int nPlayers = HostBoardStatus(szRank, sizeof(szRank));
    // "Board, <song> <difficulty>: #<rank> of <n> (<top three>)" to its song and rank.
    std::string rank = szRank;
    rank = rank.substr(0, rank.find(" ("));
    rank = rank.compare(0, 7, "Board, ") == 0 ? rank.substr(7) : rank;
    const size_t nMark = rank.find(": #");
    if (nMark == std::string::npos) {
        return std::to_string(nPlayers) + " players on the board";
    }
    return Lower(rank.substr(0, nMark)) + ", rank " + rank.substr(nMark + 3);
}

void ShowChoose() {
    g_page.stage = kStageChoose;
    SetLabel(kChoiceLocal, "LOCAL");
    SetLabel(kChoiceOnline, "ONLINE");
    SetLabel(kChoiceBoard, "SCOREBOARD");
    SetLabel(kTipsButton, g_page.aLabels[kTipsButton].mStr);
    g_page.pButtons->SetSelected(g_page.nChoice);
    MetScreenTitleScreen::ReplaceTitle(HxStr("MULTI : SELECT MODE"));
}

void ChooseHelp() {
    static const char *const kHelp[] = {"two to four players on this machine",
                                        "play friends online by a three word code",
                                        "share best scores by a three word code"};
    const int nButton = g_page.pButtons->mSelected;
    if (nButton < kTipsButton) {
        Help(kHelp[nButton]);
    } else {
        HelpKey((*g_page.pHelpKeys)[kTipsButton]);
    }
}

void ShowLocal() {
    g_page.stage = kStageLocal;
    for (int i = 0; i < kButtons; ++i) {
        SetLabel(i, g_page.aLabels[i].mStr);
    }
    g_page.pButtons->SetSelected(0);
    MetScreenTitleScreen::ReplaceTitle(g_page.title);
    HelpKey((*g_page.pHelpKeys)[0]);
}

// The role (or the board's on and off) and the three words, on the four buttons.
void ShowCode() {
    if (g_page.stage == kStageSetup) {
        SetLabel(0, g_page.bHost ? "HOST" : "JOIN");
    } else {
        SetLabel(0, g_page.bBoardOn ? "BOARD ON" : "BOARD OFF");
    }
    for (int i = 0; i < kCodeWords; ++i) {
        SetLabel(1 + i, kWords[g_page.anWords[i]]);
    }
    MetScreenTitleScreen::ReplaceTitle(
        HxStr(g_page.stage == kStageSetup ? "ONLINE : ROOM CODE" : "MULTI : FRIENDS BOARD"));
}

void CodeHelp() {
    const int nButton = g_page.pButtons->mSelected;
    std::string help;
    if (g_page.stage == kStageBoard) {
        help = nButton == 0 ?
                   BoardStatus() :
                   "left and right change word " + std::to_string(nButton) + ", cross saves";
    } else if (nButton == 0) {
        help = g_page.bHost ? "host a room, left and right switch to join" :
                              "join a friend's room, left and right switch";
    } else {
        help = "left and right change word " + std::to_string(nButton) + ", cross " +
               (g_page.bHost ? "opens" : "joins");
    }
    Help(help);
}

void ShowSetup(Stage stage) {
    g_page.stage = stage;
    if (stage == kStageSetup) {
        const char *pszHost = getenv("FREQ_ONLINE_LAST_HOST");
        g_page.bHost = pszHost == nullptr || atoi(pszHost) != 0;
        LoadCode("FREQ_ONLINE_LAST_ROOM");
    } else {
        const char *pszBoard = getenv("FREQ_BOARD");
        g_page.bBoardOn = pszBoard == nullptr || pszBoard[0] != '\0';
        LoadCode("FREQ_BOARD");
    }
    ShowCode();
    g_page.pButtons->SetSelected(1);
    CodeHelp();
}

// The lobby, refreshed each frame: a player on each button, the status on the help line.
void ShowLobby() {
    const char *apszNames[kMaxPlayers] = {};
    int anRttMs[kMaxPlayers] = {};
    const unsigned nSlots = HostOnlineActive() ? HostNetRoster(apszNames, anRttMs) : 0;
    const int nPlayers = __builtin_popcount(nSlots);
    const int nMySlot = HostOnlineSlot();
    std::string status;
    if (!HostOnlineActive()) {
        status = "the room could not open, the port is in use";
    } else if (HostOnlineSignalFailing() && nPlayers < 2) {
        status = "no internet, check the network";
    } else if (g_page.bHost) {
        status = nPlayers < 2 ? "waiting for players, tell friends the code" :
                                "connected, cross picks a song for everybody";
    } else if (nMySlot > 0) {
        status = "connected, the host picks the song";
    } else {
        status = NowMs() - g_page.flOpenedMs < kNoHostAfterMs ?
                     "looking for the host" :
                     "no host yet, check the words with the host";
    }
    std::string event;
    for (int nSlot = 0; nSlot < kMaxPlayers; ++nSlot) {
        if ((nSlots & (1u << nSlot)) == 0) {
            SetLabel(nSlot, nSlot == 0 ? "HOST" : "OPEN");
            continue;
        }
        const std::string name = Upper(apszNames[nSlot]);
        const std::string shown = name.substr(0, kShownName);
        const std::string rtt = std::to_string(anRttMs[nSlot]);
        SetLabel(nSlot, shown + " " + (nSlot == nMySlot ? "YOU" : rtt + "MS"));
        event += (event.empty() ? "" : ",") + name + ":" + (nSlot == nMySlot ? "you" : rtt);
    }
    if (status != g_page.lastStatus) {
        g_page.lastStatus = status;
        Help(status);
    }
    const std::string lobby = status + "|" + event;
    if (lobby != g_page.lastLobby) {
        g_page.lastLobby = lobby;
        HostEvent("online_lobby slot=%d players=%d status=%s list=%s",
                  nMySlot,
                  nPlayers,
                  status.c_str(),
                  event.c_str());
    }
    if (nPlayers >= 2 && !g_page.bLinkedCrumb) {
        g_page.bLinkedCrumb = true;
        HostBreadcrumb(("ONLINE_LINKED_" + std::to_string(g_page.nSessions)).c_str());
    }
}

void EnterLobby() {
    g_page.stage = kStageLobby;
    g_page.bLinkedCrumb = false;
    g_page.lastLobby.clear();
    g_page.lastStatus.clear();
    MetScreenTitleScreen::ReplaceTitle(HxStr(("ONLINE : " + g_page.room).c_str()));
    g_page.pButtons->SetSelected(0);
    ShowLobby();
    HostBreadcrumb(("ONLINE_LOBBY_" + std::to_string(g_page.nSessions)).c_str());
}

void OpenRoom() {
    g_page.room = Code();
    const std::string room = Lower(g_page.room);
    const int bOpened = HostOnlineOpen(room.c_str(), g_page.bHost);
    const int bSaved = HostSaveSetting("FREQ_ONLINE_LAST_ROOM", room.c_str()) &&
                       HostSaveSetting("FREQ_ONLINE_LAST_HOST", g_page.bHost ? "1" : "0");
    ++g_page.nSessions;
    g_page.flOpenedMs = NowMs();
    HostEvent("online_open role=%s room=%s ok=%d saved=%d",
              g_page.bHost ? "host" : "guest",
              room.c_str(),
              bOpened,
              bSaved);
    EnterLobby();
}

void SaveBoard() {
    const std::string code = g_page.bBoardOn ? Lower(Code()) : "";
    const int bSaved = HostSaveSetting("FREQ_BOARD", code.c_str());
    const int bNow = !code.empty() && HostBoardCodeChanged();
    HostEvent("online_board code=%s saved=%d now=%d", code.c_str(), bSaved, bNow);
    Help(code.empty() ? "the board is off from the next start" :
         bNow         ? "the friends board is on" :
                        "the new code applies from the next start");
}

// On ONLINE, the lobby of an open room (from the page, or from FREQ_ONLINE_ROOM at start), else
// the role and the code.
void ShowOnline() {
    if (!HostOnlineActive()) {
        ShowSetup(kStageSetup);
        return;
    }
    if (g_page.nSessions == 0) {
        const char *pszRoom = getenv("FREQ_ONLINE_ROOM");
        g_page.room = Upper(pszRoom != nullptr ? pszRoom : "");
        g_page.bHost = HostOnlineSlot() == 0;
        g_page.flOpenedMs = NowMs();
    }
    EnterLobby();
}

} // namespace

// Shows the first step (LOCAL, ONLINE, SCOREBOARD, and TIPS), from the player count screen's
// EnterAndShow(). The screen's buttons, help per button, and title come back on LOCAL.
void HostOnlinePageEnter(MetRenderer *pRenderer,
                         MetButtonList *pButtons,
                         const std::vector<HxStr> *pHelpKeys,
                         const HxStr &title) {
    g_page.pRenderer = pRenderer;
    g_page.pButtons = pButtons;
    g_page.pHelpKeys = pHelpKeys;
    g_page.title = title;
    for (int i = 0; i < kButtons && !g_page.bLabelsSaved; ++i) {
        Rnd::Button *pButton = pButtons->GetButton(i);
        g_page.aLabels[i] = pButton != nullptr && pButton->mText != nullptr ?
                                pButton->mText->mPreWrapText :
                                HxStr("");
    }
    g_page.bLabelsSaved = true;
    ShowChoose();
    ChooseHelp();
    HostEvent("online_page room_open=%d", HostOnlineActive());
}

// A command on the player count screen. Returns the command the screen handles next as it does
// without the page, or kMetScreenCommandNone when the page handled it.
int HostOnlinePageCommand(int nCommand) {
    const Stage stage = g_page.stage;
    if (stage == kStageLocal || stage == kStageGone) {
        if (stage == kStageLocal && nCommand == kMetScreenCommandBack) {
            ShowChoose();
            ChooseHelp();
            return kMetScreenCommandNone;
        }
        if (nCommand == kMetScreenCommandSelect) {
            g_page.stage = kStageGone;
        }
        return nCommand;
    }
    const int nButton = g_page.pButtons->mSelected;
    switch (nCommand) {
    case kMetScreenCommandPrevious:
    case kMetScreenCommandNext:
        if (nCommand == kMetScreenCommandPrevious) {
            g_page.pButtons->SelectPrevious();
        } else {
            g_page.pButtons->SelectNext();
        }
        if (stage == kStageChoose) {
            ChooseHelp();
        } else if (stage != kStageLobby) {
            CodeHelp();
        }
        return kMetScreenCommandNone;
    case kMetScreenCommandLeft:
    case kMetScreenCommandRight: {
        if (stage != kStageSetup && stage != kStageBoard) {
            return kMetScreenCommandNone;
        }
        const int nStep = nCommand == kMetScreenCommandRight ? 1 : -1;
        if (nButton == 0) {
            g_page.bHost = stage == kStageSetup ? !g_page.bHost : g_page.bHost;
            g_page.bBoardOn = stage == kStageBoard ? !g_page.bBoardOn : g_page.bBoardOn;
        } else {
            int &nWord = g_page.anWords[nButton - 1];
            nWord = (nWord + kWordCount + nStep) % kWordCount;
        }
        PlaySoundByName("SND_MET_KEY1");
        ShowCode();
        CodeHelp();
        HostEvent("online_setup button=%d host=%d code=%s", nButton, g_page.bHost, Code().c_str());
        return kMetScreenCommandNone;
    }
    case kMetScreenCommandSelect:
        if (stage == kStageChoose) {
            g_page.nChoice = nButton < kTipsButton ? nButton : g_page.nChoice;
            if (nButton == kChoiceLocal) {
                ShowLocal();
            } else if (nButton == kChoiceOnline) {
                ShowOnline();
                HostBreadcrumb(g_page.stage == kStageSetup ? "ONLINE_PAGE_SETUP" :
                                                             "ONLINE_PAGE_LOBBY");
            } else if (nButton == kChoiceBoard) {
                ShowSetup(kStageBoard);
                HostBreadcrumb("ONLINE_PAGE_BOARD");
            } else {
                g_page.stage = kStageGone;
                return nCommand; // the tips
            }
        } else if (stage == kStageSetup) {
            OpenRoom();
        } else if (stage == kStageBoard) {
            SaveBoard();
        } else {
            g_page.stage = kStageGone;
            HostEvent("online_to_menus slot=%d", HostOnlineSlot());
            HostBreadcrumb(("ONLINE_MENUS_" + std::to_string(g_page.nSessions)).c_str());
            return kMetScreenCommandBack; // to the main menu, the room open
        }
        return kMetScreenCommandNone;
    case kMetScreenCommandBack:
        if (stage == kStageChoose) {
            g_page.stage = kStageGone;
            return nCommand;
        }
        if (stage == kStageLobby) {
            HostNetLeave();
            HostEvent("online_stop");
            HostBreadcrumb(("ONLINE_STOPPED_" + std::to_string(g_page.nSessions)).c_str());
            ShowSetup(kStageSetup);
            return kMetScreenCommandNone;
        }
        ShowChoose();
        ChooseHelp();
        return kMetScreenCommandNone;
    default:
        return kMetScreenCommandNone;
    }
}

// A guest's song began, and the page stops showing.
void HostOnlinePageHide() {
    g_page.stage = kStageGone;
}

// Refreshes the lobby's players and status once a frame, from HostOnlinePoll().
void HostOnlinePageFrame() {
    if (g_page.stage == kStageLobby) {
        ShowLobby();
    }
}
