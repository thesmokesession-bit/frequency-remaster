// Best runs, the score ghost, and achievements. Every solo game is recorded in memory
// (src/game/gamerecorder.cpp); a new high score keeps it as replays/<level>-<difficulty>.rec in the
// title folder (the folder of FREQ_CONFIG: /app0 on the PS5), with the run's score at each change
// as replays/<level>-<difficulty>.ghost ("<song tick> <score>" lines). Square on the solo results
// screen, or Cross on the run in Options > EXTRAS > BEST RUNS (host/core/extras.cpp), plays the
// saved run back through the attract demo's path. FREQ_GHOST=1 shows the kept run's score at the
// same song tick under the live score. Achievements are kept in achievements.txt in the title
// folder, pop as notifications (a log line on Linux), and are listed in Options > EXTRAS.

#include "progress.h"

#include <algorithm>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "app/application.h"
#include "app/globals.h"
#include "game/campaignstats.h"
#include "game/gamemanagerimpl.h"
#include "game/gameparams.h"
#include "game/gamestats.h"
#include "game/grooveworld.h"
#include "met/albumcache.h"
#include "met/metfrontendstate.h"
#include "met/metpersonadata.h"
#include "met/metsonglists.h"
#include "os/hxstr.h"
#include "sch/tempomap.h"
#include "sch/tickclock.h"

extern "C" void HostNotify(const char *pszMessage) __attribute__((weak)); // host/ps5/ps5.c
extern "C" void HostBoardStart(void);                                     // host/net/online.cpp
extern "C" void
HostBoardBest(const char *pszLevel, int nDifficulty, int nScore, const char *pszName);

namespace {

struct Run {
    std::string level; // empty: this game is not one that can be kept
    int nDifficulty = 0;
    bool bPlayback = false;
    std::vector<std::pair<int, int>> samples; // (song tick, score) at each change
    std::vector<std::pair<int, int>> ghost;   // the kept run's samples
    bool bGhostShown = false;
    std::string recording;
    bool bRecorded = false;
    bool bBestPending = false;
};

Run g_run;
std::string g_pendingReplay;

const char *const kDifficultyNames[] = {"Easy", "Normal", "Expert"};

void Toast(const std::string &message) {
    if (HostNotify != nullptr) {
        HostNotify(message.c_str()); // which logs it too
    } else {
        fprintf(stderr, "notification: %s\n", message.c_str());
    }
}

std::string Folder() {
    const char *pszConfig = getenv("FREQ_CONFIG");
    const std::string config = pszConfig != nullptr ? pszConfig : "";
    const size_t nSlash = config.rfind('/');
    return nSlash == std::string::npos ? "." : config.substr(0, std::max<size_t>(nSlash, 1));
}

std::string RunPath(const std::string &level, int nDifficulty, const char *pszSuffix) {
    return Folder() + "/replays/" + level + "-" + std::to_string(nDifficulty) + pszSuffix;
}

// Host files are read and written by descriptor: fopen() is the game's (host/io/stdio.cpp).
std::string ReadFile(const std::string &path) {
    std::string data;
    const int nFile = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    char abBuffer[65536];
    for (ssize_t nRead; nFile >= 0 && (nRead = read(nFile, abBuffer, sizeof(abBuffer))) > 0;) {
        data.append(abBuffer, nRead);
    }
    if (nFile >= 0) {
        close(nFile);
    }
    return data;
}

// Written beside and renamed over, so a crash leaves the old file or the new one.
bool WriteFile(const std::string &path, const std::string &data) {
    const std::string temporary = path + ".new";
    const int nFile = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (nFile < 0) {
        return false;
    }
    const bool bWritten =
        write(nFile, data.data(), data.size()) == static_cast<ssize_t>(data.size());
    close(nFile);
    return bWritten && rename(temporary.c_str(), path.c_str()) == 0;
}

// The score a saved run ended with (the last line of its .ghost file), or -1 without a saved run.
int KeptScore(const std::string &level, int nDifficulty) {
    const std::string kept = ReadFile(RunPath(level, nDifficulty, ".ghost"));
    const size_t nLast = kept.rfind(' ');
    return nLast != std::string::npos ? atoi(kept.c_str() + nLast + 1) : -1;
}

int SongTick() {
    GrooveWorld *pWorld = Application::shared()->GetWorld();
    Sch::TickClock *pClock = pWorld != nullptr ? pWorld->GetSongClock() : nullptr;
    return pClock != nullptr ? pClock->SongTick() : 0;
}

// The score of a timeline at a song tick: that of its last change at or before the tick.
int ScoreAt(const std::vector<std::pair<int, int>> &timeline, int nTick) {
    const auto it =
        std::upper_bound(timeline.begin(), timeline.end(), std::make_pair(nTick, 0x7fffffff));
    return it == timeline.begin() ? 0 : (it - 1)->second;
}

void SaveBest() {
    g_run.bBestPending = false;
    mkdir((Folder() + "/replays").c_str(), 0755);
    std::string ghost;
    for (const auto &sample : g_run.samples) {
        ghost += std::to_string(sample.first) + " " + std::to_string(sample.second) + "\n";
    }
    if (!WriteFile(RunPath(g_run.level, g_run.nDifficulty, ".rec"), g_run.recording) ||
        !WriteFile(RunPath(g_run.level, g_run.nDifficulty, ".ghost"), ghost)) {
        fprintf(stderr,
                "replay: cannot write %s\n",
                RunPath(g_run.level, g_run.nDifficulty, "").c_str());
        return;
    }
    fprintf(stderr,
            "replay: kept the best run %s (%zu bytes, %zu score changes)\n",
            RunPath(g_run.level, g_run.nDifficulty, ".rec").c_str(),
            g_run.recording.size(),
            g_run.samples.size());
    Toast("Best run saved: " + g_run.level + " (" + kDifficultyNames[g_run.nDifficulty % 3] +
          "). Square on the results screen plays it back.");
}

// ---- achievements ----

struct Achievement {
    const char *pszId;
    const char *pszTitle;
};

const Achievement kAchievements[] = {
    {"stage1", "Stage 1 cleared: every song of stage 1, any difficulty"},
    {"stage2", "Stage 2 cleared: every song of stage 2, any difficulty"},
    {"stage3", "Stage 3 cleared: every song of stage 3 on Normal or Expert"},
    {"stage4", "Stage 4 cleared: every song of stage 4 on Expert"},
    {"stage5", "Stage 5 cleared: every song of stage 5 on Expert"},
    {"secret", "Secret stage unlocked"},
    {"supersecret", "Super secret unlocked"},
    {"easy", "Easy campaign complete"},
    {"normal", "Normal campaign complete"},
    {"expert", "Expert campaign complete"},
    {"expert1", "Stage 1 on Expert"},
    {"expert2", "Stage 2 on Expert"},
    {"expert3", "Stage 3 on Expert"},
    {"expert4", "Stage 4 on Expert"},
    {"expert5", "Stage 5 on Expert"},
    {"allexpert", "Every song on Expert"},
    {"first", "First song cleared"},
    {"ten", "Ten songs cleared"},
    {"flawless", "Flawless: every phrase of a song captured"},
    {"personalbest", "Personal best: beat your own high score on a song"},
    {"watch", "Instant replay: watch one of your best runs"},
    {"remix", "Remix saved"},
    {"godspeed", "Cleared the cut song Godspeed"},
    {"scratchotronic", "Cleared the cut song Scratchotronic"},
    {"board", "On the board: post a best to the friends board"},
};
constexpr size_t kAchievementCount = sizeof(kAchievements) / sizeof(kAchievements[0]);

std::string AchievementsPath() {
    return Folder() + "/achievements.txt";
}

bool Unlocked(const std::string &text, const char *pszId) {
    return text.find(std::string("[x] ") + pszId + " ") != std::string::npos;
}

// The file is the list a player reads: one "[x] <id> <title>" or "[ ] <id> <title>" line each.
std::string ListText(const std::string &old, const char *pszNew) {
    std::string lines;
    size_t nUnlocked = 0;
    for (const Achievement &achievement : kAchievements) {
        const bool bOn = Unlocked(old, achievement.pszId) ||
                         (pszNew != nullptr && strcmp(pszNew, achievement.pszId) == 0);
        nUnlocked += bOn;
        lines += std::string(bOn ? "[x] " : "[ ] ") + achievement.pszId + " " +
                 achievement.pszTitle + "\n";
    }
    return "# Frequency achievements: " + std::to_string(nUnlocked) + " of " +
           std::to_string(kAchievementCount) + "\n" + lines;
}

// The game thread and the board's thread (host/net/online.cpp) both unlock.
void Unlock(const char *pszId) {
    static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&s_lock);
    const std::string old = ReadFile(AchievementsPath());
    if (Unlocked(old, pszId)) {
        pthread_mutex_unlock(&s_lock);
        return;
    }
    const std::string text = ListText(old, pszId);
    WriteFile(AchievementsPath(), text);
    pthread_mutex_unlock(&s_lock);
    size_t nUnlocked = 0;
    for (size_t nAt = text.find("\n[x] "); nAt != std::string::npos;
         nAt = text.find("\n[x] ", nAt + 1)) {
        ++nUnlocked;
    }
    for (const Achievement &achievement : kAchievements) {
        if (strcmp(achievement.pszId, pszId) == 0) {
            Toast(std::string("Achievement unlocked: ") + achievement.pszTitle + " (" +
                  std::to_string(nUnlocked) + "/" + std::to_string(kAchievementCount) + ")");
        }
    }
}

} // namespace

// A file in the title folder (host/net/online.cpp's board cache).
std::string HostTitleFile(const char *pszName) {
    return Folder() + "/" + pszName;
}

std::vector<HostAchievement> HostAchievementList() {
    const std::string text = ReadFile(AchievementsPath());
    std::vector<HostAchievement> list;
    for (const Achievement &achievement : kAchievements) {
        list.push_back({achievement.pszTitle, Unlocked(text, achievement.pszId)});
    }
    return list;
}

std::vector<HostBestRun> HostBestRuns() {
    std::vector<HostBestRun> runs;
    for (const HxStr &name : GetLevelNames()) {
        const std::string level = name.mStr != nullptr ? name.mStr : "";
        for (int nDifficulty = 0; nDifficulty < 3; ++nDifficulty) {
            const int nScore = KeptScore(level, nDifficulty);
            if (nScore >= 0 && access(RunPath(level, nDifficulty, ".rec").c_str(), R_OK) == 0) {
                runs.push_back({level, nDifficulty, nScore});
            }
        }
    }
    return runs;
}

extern "C" {

// From main(): the achievements list in the log, and the friends board.
void HostProgressStart(void) {
    const std::string text = ListText(ReadFile(AchievementsPath()), nullptr);
    fprintf(stderr, "%s", text.c_str());
    HostBoardStart();
}

// A game starts (GameManagerImpl): a solo game a new best can keep (pszLevel set), a playback
// (bPlayback), or neither (pszLevel null).
void HostRunBegin(const char *pszLevel, int nDifficulty, int bPlayback) {
    g_run = Run();
    if (pszLevel == nullptr) {
        return;
    }
    g_run.level = pszLevel;
    g_run.nDifficulty = nDifficulty;
    g_run.bPlayback = bPlayback != 0;
    const char *pszGhost = getenv("FREQ_GHOST");
    const bool bGhostOn = pszGhost != nullptr && strcmp(pszGhost, "1") == 0;
    if (bGhostOn || bPlayback) {
        const std::string text = ReadFile(RunPath(pszLevel, nDifficulty, ".ghost"));
        int nTick;
        int nScore;
        int nUsed;
        for (const char *p = text.c_str(); sscanf(p, "%d %d\n%n", &nTick, &nScore, &nUsed) == 2;
             p += nUsed) {
            g_run.ghost.emplace_back(nTick, nScore);
        }
    }
    g_run.bGhostShown = bGhostOn && !bPlayback && !g_run.ghost.empty();
    if (g_run.bGhostShown) {
        fprintf(stderr,
                "ghost: the kept run of %s-%d (%zu score changes)\n",
                pszLevel,
                nDifficulty,
                g_run.ghost.size());
    }
}

// Once a frame from the score readout (src/app/hudscore.cpp): records the score's changes, and
// returns the kept run's score at this song tick, or -1 when no ghost is shown.
int HostGhostAt(int nScore) {
    if (g_run.level.empty()) {
        return -1;
    }
    if (g_run.samples.empty() ? nScore != 0 : g_run.samples.back().second != nScore) {
        g_run.samples.emplace_back(SongTick(), nScore);
    }
    return g_run.bGhostShown ? ScoreAt(g_run.ghost, SongTick()) : -1;
}

// The game ended (GameManagerImpl::EndGame()). A playback is compared with the kept timeline: the
// same scores in the same order, and the spread of the song ticks they came at (playback less
// live). A playback's presses come FREQ_INPUT_OFFSET_MS early (src/app/recorder.cpp), so its scores
// do too; the readout notices a change at the next frame.
void HostRunEnd(int nScore, int nStreak, float flRatio, int bCompleted) {
    if (g_run.level.empty()) {
        return;
    }
    fprintf(stderr,
            "replay: run end %s-%d playback=%d score=%d streak=%d ratio=%.4f completed=%d "
            "changes=%zu\n",
            g_run.level.c_str(),
            g_run.nDifficulty,
            g_run.bPlayback,
            nScore,
            nStreak,
            flRatio,
            bCompleted,
            g_run.samples.size());
    if (g_run.bGhostShown) {
        fprintf(stderr, "ghost: shown; the kept run ended at %d\n", g_run.ghost.back().second);
    }
    if (!g_run.bPlayback || g_run.ghost.empty()) {
        return;
    }
    size_t nSame = 0;
    int nMinTicks = 0;
    int nMaxTicks = 0;
    for (size_t i = 0; i < std::min(g_run.samples.size(), g_run.ghost.size()); ++i) {
        if (g_run.samples[i].second != g_run.ghost[i].second) {
            break;
        }
        const int nTicks = g_run.samples[i].first - g_run.ghost[i].first;
        nMinTicks = nSame == 0 ? nTicks : std::min(nMinTicks, nTicks);
        nMaxTicks = nSame == 0 ? nTicks : std::max(nMaxTicks, nTicks);
        ++nSame;
    }
    fprintf(stderr,
            "replay: timeline check %zu of %zu changes the same (kept run %zu), ticks off from %d "
            "to %d\n",
            nSame,
            g_run.samples.size(),
            g_run.ghost.size(),
            nMinTicks,
            nMaxTicks);
}

// The recorder's bytes once its end command ran (src/game/gamerecorder.cpp).
void HostRecordingDone(const char *pData, int nSize) {
    if (g_run.level.empty() || g_run.bPlayback) {
        return;
    }
    g_run.recording.assign(pData, nSize);
    g_run.bRecorded = true;
    if (g_run.bBestPending) {
        SaveBest();
    }
}

// CampaignStats::RecordHighScore() took a new high score. It also has to beat the kept run, which
// outlives a lost memory card. The recording may still be finishing; the later of the two writes
// the files.
void HostNewBest(const char *pszLevel, int nDifficulty, int nScore, int nOldScore) {
    if (g_run.level.empty() || g_run.bPlayback || pszLevel == nullptr || g_run.level != pszLevel ||
        g_run.nDifficulty != nDifficulty) {
        return;
    }
    const int nKeptScore = KeptScore(pszLevel, nDifficulty);
    if (nScore <= nKeptScore) {
        fprintf(stderr,
                "replay: the kept run of %s-%d (%d) stays\n",
                pszLevel,
                nDifficulty,
                nKeptScore);
    } else {
        g_run.bBestPending = true;
        if (g_run.bRecorded) {
            SaveBest();
        }
    }
    if (nOldScore > 0) {
        Unlock("personalbest");
    }
    const char *pszName = getenv("FREQ_ONLINE_NAME");
    MetPersonaData *pPersona = MetFrontEndState::shared()->GetFirstPersona();
    if ((pszName == nullptr || pszName[0] == '\0') && pPersona != nullptr) {
        pszName = pPersona->mAppearance.mUserName.mStr;
    }
    HostBoardBest(pszLevel, nDifficulty, nScore, pszName);
}

// Square on the results screen: queue the kept run of this song for the next playback.
int HostWatchBestRun(const char *pszLevel, int nDifficulty) {
    const std::string path = RunPath(pszLevel != nullptr ? pszLevel : "", nDifficulty, ".rec");
    if (access(path.c_str(), R_OK) != 0) {
        Toast("No best run kept for this song yet.");
        return 0;
    }
    g_pendingReplay = path;
    fprintf(stderr, "replay: watching %s\n", path.c_str());
    return 1;
}

int HostReplayPending(void) {
    return !g_pendingReplay.empty();
}

// GameManagerImpl::OnDoPlayback(): the queued run, or null for the attract demo.
const char *HostTakeReplay(void) {
    static std::string s_taken;
    if (g_pendingReplay.empty()) {
        return nullptr;
    }
    s_taken = g_pendingReplay;
    g_pendingReplay.clear();
    Unlock("watch");
    return s_taken.c_str();
}

// The solo results screen (src/met/metsolowinscreen.cpp): achievements, and a hint when this song
// has a kept run.
void HostResultsShown(void) {
    GameManagerImpl *pManager = Application::shared()->GetGameManager();
    const GameParams &params = *pManager->GetParams();
    GameStats *pGame = pManager->GetStats();
    MetPersonaData *pPersona = MetFrontEndState::shared()->GetFirstPersona();
    if (pGame->mCheated != 0 || pPersona == nullptr) {
        return;
    }
    CampaignStats &stats = pPersona->mStats;
    const std::string level = params.mLevelName.mStr != nullptr ? params.mLevelName.mStr : "";
    static const char *const kStages[] = {"stage1", "stage2", "stage3", "stage4", "stage5"};
    static const char *const kExpertStages[] = {
        "expert1", "expert2", "expert3", "expert4", "expert5"};
    for (int nStage = 1; nStage <= 5; ++nStage) {
        if (stats.IsStageCompleteAtAnyDifficulty(nStage)) {
            Unlock(kStages[nStage - 1]);
        }
        if (stats.IsStageComplete(CampaignStats::kDifficultyExpert, nStage)) {
            Unlock(kExpertStages[nStage - 1]);
        }
    }
    static const char *const kCampaigns[] = {"easy", "normal", "expert"};
    for (int nDifficulty = 0; nDifficulty < 3; ++nDifficulty) {
        if (stats.IsDifficultyComplete(nDifficulty)) {
            Unlock(kCampaigns[nDifficulty]);
        }
    }
    if (stats.IsSecretUnlocked()) {
        Unlock("secret");
    }
    if (stats.IsSuperSecretUnlocked()) {
        Unlock("supersecret");
    }
    int nCleared = 0;
    int nAlbum = 0;
    int nExpert = 0;
    for (const HxStr &name : GetLevelNames()) {
        if (!IsAlbumLevel(name)) {
            continue;
        }
        ++nAlbum;
        nExpert += stats.GetLevelBeaten(CampaignStats::kDifficultyExpert, name) != 0;
        nCleared += stats.GetLevelBeaten(0, name) || stats.GetLevelBeaten(1, name) ||
                    stats.GetLevelBeaten(2, name);
    }
    if (nCleared >= 1) {
        Unlock("first");
    }
    if (nCleared >= 10) {
        Unlock("ten");
    }
    if (nAlbum > 0 && nExpert == nAlbum) {
        Unlock("allexpert");
    }
    if (pGame->GetRatio(0) >= 1.0f) {
        Unlock("flawless");
    }
    if (level == "godspeed" || level == "scratchotronic") {
        Unlock(level.c_str());
    }
    fprintf(stderr,
            "achievements: checked %s (%d of %d songs cleared)\n",
            level.c_str(),
            nCleared,
            nAlbum);
    if (access(RunPath(level, params.mDifficulty, ".rec").c_str(), R_OK) == 0) {
        Toast("Square: watch your best run of " + level + ".");
    }
}

// The friends board took a post (host/net/online.cpp), and a remix was saved
// (src/met/metsaveremix.cpp).
void HostAchievement(const char *pszId) {
    Unlock(pszId);
}

} // extern "C"
