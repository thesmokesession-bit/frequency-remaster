// The game's network mode (kGameModeNet) over the room of host/net/online.cpp. The image keeps the
// in-game half of the original online play: local players, phrase managers, the gamer, and the
// powerups send packets to the world's net sink, and NetPlayers, phrase managers, the track
// selector, and the world take the other machines' packets from its net source. The net manager
// that carried them (and the lobby) is not in the image, so this file is one: a song the host
// starts with guests in the room starts on every machine, and the packets cross the room.
// src/game/gamemanagerimpl.cpp calls the Host* functions below.
//
// Room messages, the first byte their kind:
//   profile  guest to host: the guest's name
//   start    host to all: the game's number, its players (id, slot, name), and its GameParams
//   ready    guest to host: the game's level is loaded
//   go       host to all: start the song, after the delay given for each slot (ms)
//   packet   any to all: the game's number, a packet type, and the packet's saveGuts() bytes
//   roster   host to all: for each slot, whether it is in the room, the host's round trip to it
//            (ms, two bytes), and its name; the online page lists it

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "app/application.h"
#include "app/msgsink.h"
#include "app/msgsource.h"
#include "app/renderer.h"
#include "game/freqappearance.h"
#include "game/gamemanagerimpl.h"
#include "game/gameparams.h"
#include "game/grooveworld.h"
#include "game/idable.h"
#include "game/leveldata.h"
#include "game/netplayer.h"
#include "game/playmap.h"
#include "game/trackselector.h"
#include "met/metfrontendstate.h"
#include "met/metpersonadata.h"
#include "msg/begingamelocalmsg.h"
#include "msg/bumppacket.h"
#include "msg/caughtphrasepacket.h"
#include "msg/cripplepacket.h"
#include "msg/phrasepacket.h"
#include "msg/trackselectpacket.h"
#include "msg/updatescorepacket.h"
#include "online.h"
#include "os/hxstr.h"
#include "stream/iobmemstream.h"

void HostOnlinePageFrame(); // lobby.cpp
void HostOnlinePageHide();

namespace {

enum RoomMessage : unsigned char {
    kProfile = 1,
    kStart = 2,
    kReady = 3,
    kGo = 4,
    kPacket = 5,
    kRoster = 6,
};

constexpr int kMaxPlayers = kIDableInitialSlots;
constexpr int kNameLength = FreqAppearance::kRecordNameLength;
constexpr int kMaxParams = 600;
constexpr int kWaitMs = 30000;
constexpr int kMaxScoreDelta = 1 << 20;
constexpr int kMaxBar = 1 << 16;
constexpr int kDifficultyCount = 3;
constexpr size_t kMaxEarly = 256;
// AddPersonaPlayers() colours players by identifier the same way.
const char *const kColors[kMaxPlayers] = {"green", "purple", "yellow", "red"};

struct Seat {
    bool bUsed = false;
    int nSlot = 0;
    std::string name;
};

// The game the room plays now, and its players by identifier.
struct NetGame {
    bool bActive = false;
    unsigned char nId = 0;
    Seat seats[kMaxPlayers];
};

// What the validator measures packets against: the level's track count and its phrase steps.
struct Bounds {
    int nTracks = 0;
    int nSteps = 0;
};

NetGame g_game;
std::string g_aGuestNames[kMaxPlayers];
unsigned char g_nNextGameId = 1;
unsigned g_nLastLinked = 0;
bool g_bLoadPending = false;
std::vector<std::string> g_early; // packet messages that came before this machine's world
Bounds g_testBounds;              // FREQ_SELFTEST=online, which has no level
int g_nAccepted = 0;
int g_nRefused = 0;
Seat g_roster[kMaxPlayers]; // the room's players by slot, nSlot their round trip to the host
std::string g_lastRoster;   // the roster message the host sent last

GameManagerImpl *Manager() {
    return Application::shared()->GetGameManager();
}

bool Carried(int nType) {
    return nType == g_nTrackSelectPacketType || nType == g_nCaughtPhrasePacketType ||
           nType == g_nUpdateScorePacketType || nType == g_nCripplePacketType ||
           nType == g_nBumpPacketType;
}

void Send(const std::string &message) {
    HostOnlineSend(message.data(), static_cast<int>(message.size()));
}

// The game's packets to the room. The world's players, gamer, and track graphs send everything
// they send to this sink, so only the packet classes that cross pass.
class NetSink : public MsgSink {
public:
    virtual void DispatchPriv(Message *pMsg) {
        const int nType = pMsg->Type();
        if (!g_game.bActive || !Carried(nType)) {
            return;
        }
        IOBMemStream stream;
        pMsg->saveGuts(stream);
        std::string message(1, static_cast<char>(kPacket));
        message += static_cast<char>(g_game.nId);
        message += static_cast<char>(nType & 0xff);
        message += static_cast<char>(nType >> 8);
        message.append(stream.Buffer(), stream.Tell());
        Send(message);
    }
};

NetSink *Sink() {
    static NetSink *s_pSink = new NetSink;
    return s_pSink;
}

MsgSource *Source() {
    static MsgSource *s_pSource = new MsgSource;
    return s_pSource;
}

int Word(const unsigned char *pData, int nIndex) {
    const unsigned char *pWord = pData + 4 * nIndex;
    return static_cast<int>(static_cast<uint32_t>(pWord[0]) | static_cast<uint32_t>(pWord[1]) << 8 |
                            static_cast<uint32_t>(pWord[2]) << 16 |
                            static_cast<uint32_t>(pWord[3]) << 24);
}

bool ValidPlayer(int nId, bool bNullAllowed) {
    if (nId == kIDableUnregistered) {
        return bNullAllowed;
    }
    return nId >= 0 && nId < static_cast<int>(IDable<Player>::sObjects.size()) &&
           IDable<Player>::sObjects[nId] != nullptr;
}

bool InRange(int nValue, int nEnd) {
    return nValue >= 0 && nValue < nEnd;
}

// The trust boundary for game packets: the type, the exact length, and every player, track, and
// step the handlers index with, checked on the bytes before the packet class reads them (Gem and
// IDablePtr index the player table unchecked, CripplePacket sizes a vector from the wire). The
// words after Packet's four are each class's saveGuts() order.
bool ValidPacket(int nType, const unsigned char *pData, int nSize, const Bounds &bounds) {
    const int nTracks = std::min(bounds.nTracks, kTrackSelectorChannelCount);
    const auto words = [nSize](int nCount) { return nSize == 4 * nCount; };
    if (nType == g_nTrackSelectPacketType) {
        return words(8) && ValidPlayer(Word(pData, 5), false) && InRange(Word(pData, 6), nTracks) &&
               InRange(Word(pData, 7), kTrackSelectorSlotCount);
    }
    if (nType == g_nCaughtPhrasePacketType) {
        return words(8) && ValidPlayer(Word(pData, 4), true) && InRange(Word(pData, 5), nTracks) &&
               InRange(Word(pData, 6), bounds.nSteps);
    }
    if (nType == g_nUpdateScorePacketType) {
        return words(6) && InRange(Word(pData, 4), kMaxPlayers) &&
               abs(Word(pData, 5)) <= kMaxScoreDelta;
    }
    if (nType == g_nBumpPacketType) {
        return words(7) && ValidPlayer(Word(pData, 4), false) && InRange(Word(pData, 5), kMaxBar) &&
               InRange(Word(pData, 6), nTracks);
    }
    if (nType == g_nCripplePacketType) {
        if (nSize < 24 || !InRange(Word(pData, 5), kMaxPlayers + 1) || !words(6 + Word(pData, 5)) ||
            !ValidPlayer(Word(pData, 4), false)) {
            return false;
        }
        for (int i = 0; i < Word(pData, 5); ++i) {
            if (!ValidPlayer(Word(pData, 6 + i), false)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

Bounds CurrentBounds() {
    if (g_testBounds.nTracks != 0) {
        return g_testBounds;
    }
    GrooveWorld *pWorld = Manager()->GetWorld();
    if (pWorld == nullptr || pWorld->GetLevel() == nullptr || pWorld->GetPlayMap() == nullptr) {
        return g_testBounds;
    }
    Bounds bounds;
    bounds.nTracks = pWorld->GetLevel()->TrackCount();
    bounds.nSteps = pWorld->GetPlayMap()->GetLength();
    return bounds;
}

// A packet message from the room into the game's net source, if it holds.
void OnPacket(const unsigned char *pData, int nSize) {
    if (nSize < 4 || pData[1] != g_game.nId || !g_game.bActive) {
        ++g_nRefused;
        return;
    }
    // A guest still loading keeps what the host's world sends as it starts (the players' first
    // track selections), for HostNetWaitStart() to pass on once this world exists.
    if (g_testBounds.nTracks == 0 && Manager()->GetWorld() == nullptr) {
        if (g_early.size() < kMaxEarly) {
            g_early.emplace_back(reinterpret_cast<const char *>(pData), nSize);
        }
        return;
    }
    const int nType = pData[2] | pData[3] << 8;
    const unsigned char *pBytes = pData + 4;
    const int nBytes = nSize - 4;
    Message *pMsg = Carried(nType) && ValidPacket(nType, pBytes, nBytes, CurrentBounds()) ?
                        Message::NewMessage(nType) :
                        nullptr;
    if (pMsg == nullptr) {
        ++g_nRefused;
        return;
    }
    IOBMemStream in(pBytes, nBytes);
    in.Seek(0, kStreamSeekSet);
    pMsg->restoreGuts(in);
    IOBMemStream out;
    pMsg->saveGuts(out);
    if (in.Fail() || in.Tell() != nBytes || out.Tell() != nBytes ||
        memcmp(out.Buffer(), pBytes, nBytes) != 0) {
        ++g_nRefused;
        delete pMsg;
        return;
    }
    ++g_nAccepted;
    Source()->Send(pMsg);
    delete pMsg;
}

bool ValidName(const std::string &name) {
    if (name.empty() || name.size() > kNameLength) {
        return false;
    }
    for (unsigned char c : name) {
        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

// FREQ_ONLINE_NAME, else the Freq that plays (or the first on the memory card).
std::string LocalName() {
    const char *pszName = getenv("FREQ_ONLINE_NAME");
    // FREQ_SELFTEST=online runs with no game manager.
    MetPersonaData *pPersona =
        Manager() != nullptr ? MetFrontEndState::shared()->GetFirstPersona() : nullptr;
    if (pPersona == nullptr && !MetPersonaData::loadList()->empty()) {
        pPersona = MetPersonaData::loadList()->front();
    }
    if ((pszName == nullptr || pszName[0] == '\0') && pPersona != nullptr) {
        pszName = pPersona->mAppearance.mUserName.mStr;
    }
    std::string name = pszName != nullptr ? pszName : "";
    name = name.substr(0, kNameLength);
    return ValidName(name) ? name : "Player " + std::to_string(HostOnlineSlot() + 1);
}

// A level or arena name from the host: what the disc's own names are made of, nothing that
// climbs out of a folder.
bool ValidLevelName(const HxStr &name) {
    static const char kAllowed[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.";
    const std::string text = name.mStr != nullptr ? std::string(name.mStr, name.mLen) : "";
    return text.size() < 64 && text.find("..") == std::string::npos &&
           text.find_first_not_of(kAllowed) == std::string::npos;
}

std::string StartMessage(const NetGame &game, const GameParams &params) {
    std::string message(1, static_cast<char>(kStart));
    message += static_cast<char>(game.nId);
    int nPlayers = 0;
    for (const Seat &seat : game.seats) {
        nPlayers += seat.bUsed ? 1 : 0;
    }
    message += static_cast<char>(nPlayers);
    for (int nId = 0; nId < kMaxPlayers; ++nId) {
        const Seat &seat = game.seats[nId];
        if (seat.bUsed) {
            message += static_cast<char>(nId);
            message += static_cast<char>(seat.nSlot);
            message += static_cast<char>(seat.name.size());
            message += seat.name;
        }
    }
    IOBMemStream stream;
    params.Save(&stream);
    message.append(stream.Buffer(), stream.Tell());
    return message;
}

// The trust boundary for a start message: its seats and its GameParams.
bool ParseStart(const unsigned char *pData, int nSize, NetGame *pGame, GameParams *pParams) {
    *pGame = NetGame();
    if (nSize < 3 || pData[2] < 1 || pData[2] > kMaxPlayers) {
        return false;
    }
    pGame->nId = pData[1];
    int nAt = 3;
    for (int i = 0; i < pData[2]; ++i) {
        if (nAt + 3 > nSize) {
            return false;
        }
        const int nId = pData[nAt];
        const int nSlot = pData[nAt + 1];
        const int nLength = pData[nAt + 2];
        if (!InRange(nId, kMaxPlayers) || pGame->seats[nId].bUsed || !InRange(nSlot, kMaxPlayers) ||
            nAt + 3 + nLength > nSize) {
            return false;
        }
        Seat &seat = pGame->seats[nId];
        seat.bUsed = true;
        seat.nSlot = nSlot;
        seat.name.assign(reinterpret_cast<const char *>(pData) + nAt + 3, nLength);
        if (!ValidName(seat.name)) {
            return false;
        }
        nAt += 3 + nLength;
    }
    // GameParams::Save(): two length-prefixed names, then seven words.
    const int nParams = nSize - nAt;
    const unsigned char *pParamBytes = pData + nAt;
    if (nParams < 8 || nParams > kMaxParams || !InRange(Word(pParamBytes, 0), 64)) {
        return false;
    }
    const int nArenaAt = 4 + Word(pParamBytes, 0);
    if (nArenaAt + 4 > nParams) {
        return false;
    }
    const int nArena = pParamBytes[nArenaAt] | pParamBytes[nArenaAt + 1] << 8 |
                       pParamBytes[nArenaAt + 2] << 16 | pParamBytes[nArenaAt + 3] << 24;
    if (!InRange(nArena, 64) || nArenaAt + 4 + nArena + 7 * 4 != nParams) {
        return false;
    }
    IOBMemStream stream(pParamBytes, nParams);
    stream.Seek(0, kStreamSeekSet);
    pParams->Load(&stream);
    // The difficulty indexes the level converter's spans.
    return !stream.Fail() && pParams->mPlayMode == kPlayModeGame && pParams->mJukeboxMode == 0 &&
           InRange(pParams->mDifficulty, kDifficultyCount) && ValidLevelName(pParams->mLevelName) &&
           ValidLevelName(pParams->mArenaName);
}

// A guest's start: the host's song, with this machine's seats as its players. The level loads
// as MetLoadGameScreen loads one, and the game begins in HostOnlinePoll() once it has.
void OnStart(const unsigned char *pData, int nSize) {
    GameManagerImpl *pManager = Manager();
    NetGame game;
    GameParams params;
    if (pManager->GetWorld() != nullptr || g_bLoadPending) {
        fprintf(stderr, "online: the host started a song while this machine plays one\n");
        return;
    }
    if (!ParseStart(pData, nSize, &game, &params)) {
        fprintf(stderr, "online: refused a start message\n");
        return;
    }
    pManager->ClearPersonas();
    for (const Seat &seat : game.seats) {
        if (seat.bUsed && seat.nSlot == HostOnlineSlot()) {
            MetPersonaData persona;
            persona.mAppearance.mUserName = HxStr(seat.name.c_str());
            pManager->AddPersona(persona);
        }
    }
    if (pManager->GetPersonas()->empty()) {
        fprintf(stderr,
                "online: the host's song has no seat for this machine (the room is full)\n");
        return;
    }
    params.mNetGame = 1;
    g_early.clear();
    HostOnlinePageHide();
    pManager->SetParams(params);
    pManager->SetGameMode(kGameModeNet);
    g_game = game;
    g_game.bActive = true;
    Renderer::LoadCommon();
    Renderer::LoadLevel(params);
    g_bLoadPending = true;
    fprintf(stderr,
            "online: the host started %s in %s\n",
            params.mLevelName.mStr,
            params.mArenaName.mStr);
}

std::string RosterMessage() {
    std::string message(1, static_cast<char>(kRoster));
    const unsigned nLinked = HostOnlineLinked() | 1u;
    for (int nSlot = 0; nSlot < kMaxPlayers; ++nSlot) {
        const bool bIn = (nLinked & (1u << nSlot)) != 0;
        const int nRtt = bIn ? std::min(HostOnlineRttMs(nSlot), 0xffff) : 0;
        std::string name;
        if (bIn) {
            name = nSlot == 0                    ? LocalName() :
                   !g_aGuestNames[nSlot].empty() ? g_aGuestNames[nSlot] :
                                                   "Player " + std::to_string(nSlot + 1);
        }
        message += static_cast<char>(bIn);
        message += static_cast<char>(nRtt & 0xff);
        message += static_cast<char>(nRtt >> 8);
        message += static_cast<char>(name.size());
        message += name;
    }
    return message;
}

// The trust boundary for a roster: four well-formed entries, the host's among them.
bool ParseRoster(const unsigned char *pData, int nSize, Seat aRoster[kMaxPlayers]) {
    int nAt = 1;
    for (int nSlot = 0; nSlot < kMaxPlayers; ++nSlot) {
        if (nAt + 4 > nSize || nAt + 4 + pData[nAt + 3] > nSize || pData[nAt] > 1) {
            return false;
        }
        Seat &seat = aRoster[nSlot];
        seat.bUsed = pData[nAt] != 0;
        seat.nSlot = pData[nAt + 1] | pData[nAt + 2] << 8;
        seat.name.assign(reinterpret_cast<const char *>(pData) + nAt + 4, pData[nAt + 3]);
        if (seat.bUsed ? !ValidName(seat.name) : !seat.name.empty()) {
            return false;
        }
        nAt += 4 + pData[nAt + 3];
    }
    return nAt == nSize && aRoster[0].bUsed;
}

// One room message, outside the start barrier.
void OnMessage(const unsigned char *pData, int nSize, int nFrom) {
    if (nSize < 1) {
        return;
    }
    const bool bHost = HostOnlineSlot() == 0;
    if (pData[0] == kPacket) {
        OnPacket(pData, nSize);
    } else if (pData[0] == kProfile && bHost && InRange(nFrom, kMaxPlayers)) {
        const std::string name(reinterpret_cast<const char *>(pData) + 1, nSize - 1);
        g_aGuestNames[nFrom] = ValidName(name) ? name : "";
    } else if (pData[0] == kStart && !bHost && nFrom == 0) {
        OnStart(pData, nSize);
    } else if (pData[0] == kRoster && !bHost && nFrom == 0) {
        Seat roster[kMaxPlayers];
        if (ParseRoster(pData, nSize, roster)) {
            std::copy(roster, roster + kMaxPlayers, g_roster);
        }
    }
}

unsigned char g_abMessage[HOST_ONLINE_MAX_MESSAGE];

std::string Saved(Message *pMsg) {
    IOBMemStream stream;
    pMsg->saveGuts(stream);
    return std::string(stream.Buffer(), stream.Tell());
}

// FREQ_SELFTEST=online: one packet of each class that crosses, between players 0 to 3.
std::vector<Message *> TestPackets() {
    Player *const *ppPlayers = IDable<Player>::sObjects.data();
    return {new TrackSelectPacket(Sch::Tick(960), ppPlayers[0], 3, 1),
            new CaughtPhrasePacket(ppPlayers[1], 2, 5),
            new UpdateScorePacket(0, 250),
            new CripplePacket(ppPlayers[0], {ppPlayers[1], ppPlayers[2]}),
            new BumpPacket(ppPlayers[1], 7, 4)};
}

// A packet message of the given type whose bytes are the words, little-endian.
std::string Forged(int nType, std::vector<int> words, unsigned char nGame = 1) {
    std::string message{static_cast<char>(kPacket),
                        static_cast<char>(nGame),
                        static_cast<char>(nType & 0xff),
                        static_cast<char>(nType >> 8)};
    for (const int nWord : words) {
        for (int i = 0; i < 4; ++i) {
            message += static_cast<char>((static_cast<uint32_t>(nWord) >> (8 * i)) & 0xff);
        }
    }
    return message;
}

// FREQ_SELFTEST=online: a start message reads back as written, and forged ones are refused.
bool StartChecks() {
    NetGame game;
    game.nId = 3;
    game.seats[0] = Seat{true, 0, "Host"};
    game.seats[1] = Seat{true, 1, "Guest"};
    GameParams params;
    params.mLevelName = HxStr("song1");
    params.mArenaName = HxStr("arena1");
    params.mPlayMode = kPlayModeGame;
    params.mDifficulty = 2;
    const auto parses = [](const std::string &message, NetGame *pGame, GameParams *pParams) {
        return ParseStart(reinterpret_cast<const unsigned char *>(message.data()),
                          static_cast<int>(message.size()),
                          pGame,
                          pParams);
    };
    NetGame parsed;
    GameParams back;
    const std::string good = StartMessage(game, params);
    bool bHolds = parses(good, &parsed, &back) && parsed.nId == 3 && back == params &&
                  parsed.seats[1].bUsed && parsed.seats[1].nSlot == 1 &&
                  parsed.seats[1].name == "Guest" && !parsed.seats[2].bUsed;

    std::vector<std::string> forged = {good.substr(0, good.size() - 1), good + "x"};
    GameParams other = params;
    other.mLevelName = HxStr("../x");
    forged.push_back(StartMessage(game, other));
    other = params;
    other.mDifficulty = 7;
    forged.push_back(StartMessage(game, other));
    other = params;
    other.mPlayMode = kPlayModeJam;
    forged.push_back(StartMessage(game, other));
    NetGame badName = game;
    badName.seats[1].name = "a\nb";
    forged.push_back(StartMessage(badName, params));
    std::string twice = good;
    twice[3 + 3 + 4] = 0; // the second seat claims identifier 0 as well
    forged.push_back(twice);
    for (const std::string &message : forged) {
        bHolds = bHolds && !parses(message, &parsed, &back);
    }
    return bHolds;
}

class RecordingSink : public MsgSink {
public:
    virtual void DispatchPriv(Message *pMsg) {
        mSeen.push_back(Saved(pMsg));
    }

    std::vector<std::string> mSeen;
};

} // namespace

extern "C" {

// Once a frame: the room's messages, a guest's name for the host, and a guest's level load.
void HostOnlinePoll(void) {
    const unsigned nLinked = HostOnlineLinked();
    if (nLinked != g_nLastLinked && HostOnlineSlot() > 0 && nLinked != 0) {
        Send(std::string(1, static_cast<char>(kProfile)) + LocalName());
    }
    g_nLastLinked = nLinked;
    if (HostOnlineSlot() == 0) {
        const std::string roster = RosterMessage();
        if (roster != g_lastRoster && nLinked != 0) {
            Send(roster);
        }
        g_lastRoster = roster;
        ParseRoster(reinterpret_cast<const unsigned char *>(roster.data()),
                    static_cast<int>(roster.size()),
                    g_roster);
    }
    int nFrom = -1;
    for (int nSize; (nSize = HostOnlineReceive(g_abMessage, sizeof(g_abMessage), &nFrom)) > 0;) {
        OnMessage(g_abMessage, nSize, nFrom);
    }
    float flProgress;
    if (g_bLoadPending && Renderer::PollCommon(&flProgress) != 0 &&
        Renderer::PollLevel(&flProgress) != 0) {
        g_bLoadPending = false;
        BeginGameLocalMsg begin;
        Manager()->QueueMessage(&begin);
    }
    HostOnlinePageFrame();
}

// Before a song's world is made: the host turns a song it starts with guests in the room into a
// net game and tells the guests. Remixes, the tutorial, and the jukebox stay on this machine.
void HostNetBeginGame(void) {
    GameManagerImpl *pManager = Manager();
    if (pManager->GetGameMode() == kGameModeNet) {
        return; // a guest's, from OnStart()
    }
    g_game.bActive = false;
    const GameParams &params = *pManager->GetParams();
    const int nMode = pManager->GetGameMode();
    const char *pszLevel = params.mLevelName.mStr != nullptr ? params.mLevelName.mStr : "";
    if (HostOnlineSlot() != 0 || HostOnlineLinked() == 0 || params.mPlayMode != kPlayModeGame ||
        params.mJukeboxMode != 0 || strncmp(pszLevel, "tutorial", 8) == 0 ||
        (nMode != kGameModeSolo && nMode != kGameModeLocal)) {
        return;
    }
    NetGame game;
    game.nId = g_nNextGameId++;
    int nPlayers = 0;
    for (MetPersonaData *pPersona : *pManager->GetPersonas()) {
        const HxStr &name = pPersona->mAppearance.mUserName;
        Seat &seat = game.seats[nPlayers];
        seat.bUsed = true;
        seat.name =
            std::string(name.mStr != nullptr ? name.mStr : "", name.mLen).substr(0, kNameLength);
        if (!ValidName(seat.name)) {
            seat.name = "Player " + std::to_string(nPlayers + 1);
        }
        if (++nPlayers == kMaxPlayers) {
            break;
        }
    }
    const int nHostPlayers = nPlayers;
    for (int nSlot = 1; nSlot < kMaxPlayers && nPlayers < kMaxPlayers; ++nSlot) {
        if ((HostOnlineLinked() & (1u << nSlot)) != 0) {
            Seat &seat = game.seats[nPlayers++];
            seat.bUsed = true;
            seat.nSlot = nSlot;
            seat.name = !g_aGuestNames[nSlot].empty() ? g_aGuestNames[nSlot] :
                                                        "Player " + std::to_string(nPlayers);
        }
    }
    if (nPlayers == nHostPlayers) {
        fprintf(stderr, "online: no seat left for a guest; the song plays here only\n");
        return;
    }
    Send(StartMessage(game, params));
    HostOnlineSetOpen(0);
    pManager->SetGameMode(kGameModeNet);
    g_game = game;
    g_game.bActive = true;
    fprintf(
        stderr, "online: room song %u, %d players (%d here)\n", game.nId, nPlayers, nHostPlayers);
}

// In place of AddPersonaPlayers() in a net game: the world's net IO, this machine's seats as local
// players in persona order, and the others' as NetPlayers. Returns 0 outside a net game.
int HostNetAddPlayers(void) {
    if (!g_game.bActive) {
        return 0;
    }
    GameManagerImpl *pManager = Manager();
    GrooveWorld *pWorld = pManager->GetWorld();
    pWorld->SetNetIO(Sink(), Source());
    static FreqAppearance *s_apAppearances[kMaxPlayers];
    const std::vector<MetPersonaData *> &personas = *pManager->GetPersonas();
    const int nMySlot = HostOnlineSlot();
    size_t nLocal = 0;
    for (int nId = 0; nId < kMaxPlayers; ++nId) {
        const Seat &seat = g_game.seats[nId];
        if (!seat.bUsed) {
            continue;
        }
        if (seat.nSlot == nMySlot && nLocal < personas.size()) {
            pWorld->AddLocalPlayer(
                nId, static_cast<int>(nLocal), nId, HxStr(kColors[nId]), personas[nLocal]);
            ++nLocal;
        } else if (seat.nSlot != nMySlot) {
            if (s_apAppearances[nId] == nullptr) {
                s_apAppearances[nId] = new FreqAppearance;
            }
            s_apAppearances[nId]->mUserName = HxStr(seat.name.c_str());
            pWorld->AddNetPlayer(nId, nId, HxStr(kColors[nId]), s_apAppearances[nId]);
        }
    }
    return 1;
}

// Before the song's play command: every machine has loaded, then all start together. The host
// waits for each guest's ready, tells each how long to wait so that the go reaches every machine
// at once (half the slowest round trip, less half its own), and waits that long itself.
void HostNetWaitStart(void) {
    if (!g_game.bActive) {
        return;
    }
    const bool bHost = HostOnlineSlot() == 0;
    unsigned nWaiting = 0;
    for (const Seat &seat : g_game.seats) {
        nWaiting |= seat.bUsed && seat.nSlot != 0 ? 1u << seat.nSlot : 0;
    }
    std::vector<std::string> early;
    early.swap(g_early);
    for (const std::string &message : early) {
        OnPacket(reinterpret_cast<const unsigned char *>(message.data()),
                 static_cast<int>(message.size()));
    }
    if (!bHost) {
        Send(std::string{static_cast<char>(kReady), static_cast<char>(g_game.nId)});
    }
    int nDelayMs = 0;
    bool bGo = false;
    for (int nWaited = 0; nWaited < kWaitMs && !bGo; nWaited += 2) {
        int nFrom = -1;
        const int nSize = HostOnlineReceive(g_abMessage, sizeof(g_abMessage), &nFrom);
        if (nSize == 0) {
            usleep(2000);
        } else if (bHost && nSize == 2 && g_abMessage[0] == kReady &&
                   g_abMessage[1] == g_game.nId && InRange(nFrom, kMaxPlayers)) {
            nWaiting &= ~(1u << nFrom);
        } else if (!bHost && nSize == 2 + 2 * kMaxPlayers && nFrom == 0 && g_abMessage[0] == kGo &&
                   g_abMessage[1] == g_game.nId) {
            const int nSlot = HostOnlineSlot();
            if (InRange(nSlot, kMaxPlayers)) {
                nDelayMs = g_abMessage[2 + 2 * nSlot] | g_abMessage[3 + 2 * nSlot] << 8;
            }
            bGo = true;
        } else if (nSize > 0) {
            OnMessage(g_abMessage, nSize, nFrom);
        }
        nWaiting &= HostOnlineLinked();
        if (bHost && nWaiting == 0) {
            int nSlowestMs = 0;
            for (int nSlot = 1; nSlot < kMaxPlayers; ++nSlot) {
                nSlowestMs = std::max(nSlowestMs, HostOnlineRttMs(nSlot));
            }
            std::string go{static_cast<char>(kGo), static_cast<char>(g_game.nId)};
            for (int nSlot = 0; nSlot < kMaxPlayers; ++nSlot) {
                const int nRtt = nSlot == 0 ? 0 : HostOnlineRttMs(nSlot);
                const int nWait = std::min(1000, (nSlowestMs - nRtt) / 2);
                go += static_cast<char>(nWait & 0xff);
                go += static_cast<char>(nWait >> 8);
            }
            Send(go);
            nDelayMs = std::min(1000, nSlowestMs / 2);
            bGo = true;
        }
    }
    if (!bGo) {
        fprintf(stderr, "online: the other machines did not answer; the song starts anyway\n");
    }
    usleep(static_cast<useconds_t>(nDelayMs) * 1000);
    fprintf(stderr, "online: room song %u starts (waited %d ms)\n", g_game.nId, nDelayMs);
}

// The players for the online page, with each player's name in apszNames (valid until the next
// poll) and the round trip in anRttMs (this machine's to the host, or the host's to a guest).
// Returns the slots in the room, one bit each, or 0 when this machine is in no room or a guest
// has no host.
unsigned HostNetRoster(const char *apszNames[kMaxPlayers], int anRttMs[kMaxPlayers]) {
    const int nMySlot = HostOnlineSlot();
    unsigned nSlots = 0;
    for (int nSlot = 0; nSlot < kMaxPlayers && nMySlot >= 0; ++nSlot) {
        const Seat &seat = g_roster[nSlot];
        nSlots |= seat.bUsed ? 1u << nSlot : 0;
        apszNames[nSlot] = seat.name.c_str();
        anRttMs[nSlot] = nSlot == 0 && nMySlot > 0 ? HostOnlineRttMs(0) : seat.nSlot;
    }
    return nSlots;
}

// The online page exited the room. The next room starts with no names.
void HostNetLeave(void) {
    HostOnlineClose();
    for (int i = 0; i < kMaxPlayers; ++i) {
        g_aGuestNames[i].clear();
        g_roster[i] = Seat();
    }
    g_lastRoster.clear();
    g_nLastLinked = 0;
}

// After a net game's world is gone: the end screens are the local multiplayer ones (the image has
// no MetNetEndScreen), and the host takes new guests again.
void HostNetEndGame(void) {
    if (!g_game.bActive) {
        return;
    }
    g_game.bActive = false;
    Manager()->SetGameMode(kGameModeLocal);
    HostOnlineSetOpen(1);
}

// FREQ_SELFTEST=online, in place of the game (no disc): the host sends a packet of each class
// that crosses through the net sink, then forged ones; the guest takes them through the same
// validator into the net source and checks what arrives against the same packets made here.
int HostOnlineSelfTest(void) {
    for (int nWaited = 0; HostOnlineLinked() == 0; nWaited += 10) {
        if (nWaited > 30000) {
            fprintf(stderr, "online: self-test: no link\n");
            return 1;
        }
        usleep(10000);
    }
    for (int nId = 0; nId < kMaxPlayers; ++nId) {
        new NetPlayer(nId, nId, HxStr(kColors[nId]), nullptr);
    }
    g_testBounds.nTracks = 8;
    g_testBounds.nSteps = 64;
    g_game.bActive = true;
    g_game.nId = 1;
    const std::vector<Message *> packets = TestPackets();
    if (HostOnlineSlot() == 0) {
        for (Message *pMsg : packets) {
            Sink()->Dispatch(pMsg);
        }
        const std::vector<std::string> forged = {
            Forged(g_nTrackSelectPacketType, {0, 0, -1, -1, 960, 9, 3, 1}),  // no player 9
            Forged(g_nTrackSelectPacketType, {0, 0, -1, -1, 960, -1, 3, 1}), // -1 indexes the table
            Forged(g_nCripplePacketType, {2, 3, -1, -1, 0, 1000, 1}),        // a huge count
            Forged(g_nUpdateScorePacketType, {0, 0, -1, -1, 0, 250, 0}),     // a word too many
            Forged(g_nCaughtPhrasePacketType, {0, 0, -1, -1, 1, 2, 64, -1}), // past the last step
            Forged(g_nBumpPacketType, {0, 0, -1, -1, 1, 7, 8}),              // past the last track
            Forged(g_nPhrasePacketType, {0, 0, -1, -1, 0}),                  // a class not carried
            Forged(700, {0}),                                                // no such class
            Forged(g_nUpdateScorePacketType, {0, 0, -1, -1, 0, 250}, 2),     // another game's
        };
        for (const std::string &message : forged) {
            Send(message);
        }
        for (int nWaited = 0; nWaited < 30000; nWaited += 10) {
            int nFrom = -1;
            const int nSize = HostOnlineReceive(g_abMessage, sizeof(g_abMessage), &nFrom);
            if (nSize == 2 && g_abMessage[0] == kReady) {
                printf("ONLINE_SELFTEST_SENT %zu packets, %zu forged\n",
                       packets.size(),
                       forged.size());
                return 0;
            }
            usleep(10000);
        }
        fprintf(stderr, "online: self-test: the guest did not finish\n");
        return 1;
    }
    RecordingSink sink;
    Source()->AddSink(&sink);
    constexpr int kForged = 9;
    for (int nWaited = 0; nWaited < 30000 && g_nAccepted + g_nRefused < 5 + kForged;
         nWaited += 10) {
        HostOnlinePoll();
        usleep(10000);
    }
    bool bSame = sink.mSeen.size() == packets.size();
    for (size_t i = 0; bSame && i < packets.size(); ++i) {
        bSame = sink.mSeen[i] == Saved(packets[i]);
    }
    Send(std::string{static_cast<char>(kReady), 1});
    usleep(1000 * 1000);
    const bool bStart = StartChecks();
    fprintf(stderr,
            "online: self-test: %d accepted, %d refused, contents %s, start messages %s\n",
            g_nAccepted,
            g_nRefused,
            bSame ? "equal" : "differ",
            bStart ? "hold" : "fail");
    if (!bSame || !bStart || g_nAccepted != 5 || g_nRefused != kForged) {
        return 1;
    }
    printf("ONLINE_SELFTEST_RECEIVED %d packets equal, %d forged refused, start checks hold\n",
           g_nAccepted,
           g_nRefused);
    return 0;
}

} // extern "C"
