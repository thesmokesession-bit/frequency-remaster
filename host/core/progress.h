// The achievements and best runs that host/core/progress.cpp records, as the EXTRAS screen
// (host/core/extras.cpp) lists them.

#pragma once

#include <string>
#include <vector>

struct HostAchievement {
    const char *pszTitle;
    bool bUnlocked;
};

struct HostBestRun {
    std::string level;
    int nDifficulty;
    int nScore;
};

// Every achievement, in the list's order, read once from achievements.txt.
std::vector<HostAchievement> HostAchievementList();

// Every saved best run, in the order of the game's level list and then by difficulty.
std::vector<HostBestRun> HostBestRuns();

extern "C" int HostWatchBestRun(const char *pszLevel, int nDifficulty);
