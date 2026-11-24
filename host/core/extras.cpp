// The EXTRAS screen opens from the fifth button of Options on the host. ACHIEVEMENTS lists every
// achievement (unlocked ones lit, locked ones grey, the description on the help line). BEST RUNS
// lists each saved run (song, difficulty, score), and Cross plays it as Square does on the results
// screen. The screen draws with the remix list of the memory card screens (the container it shares
// with MetRemixDelScreen) and the song panel of MetRemixDataScreen. Its prompts are added to the
// script's gMetHelpStrings, and the help screen formats them by key.

#include <map>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "app/application.h"
#include "game/gamemanagerimpl.h"
#include "gfx/hostframe.h"
#include "met/albumcache.h"
#include "met/listdataprovider.h"
#include "met/methelpscreen.h"
#include "met/metloadgamescreen.h"
#include "met/metremixdatascreen.h"
#include "met/metremixrecord.h"
#include "met/metrenderer.h"
#include "met/metscreen.h"
#include "met/metscreentitlescreen.h"
#include "met/metsonglists.h"
#include "met/scrollinglist.h"
#include "os/hxstr.h"
#include "progress.h"
#include "rnd/font.h"
#include "rnd/manager.h"
#include "rnd/mesh.h"
#include "rnd/text.h"
#include "rnd/view.h"
#include "script/configquery.h"
#include "script/scripteval.h"

void HostBreadcrumb(const char *pszName); // host/io/pad.cpp

namespace {

const char *const kScreenKey = "MetExtrasScreen";
const char *const kScreenName = "mcrd";
const char *const kDirectory = "metagame/Shared";
const char *const kContainerName = "memcard_remix_del";

// The remix list's objects and geometry, as MetRemixDelScreen builds its list.
const char *const kLineView = "mcrd_line.view";
const char *const kHighlightMesh = "mcrd_hilite.mesh";
const char *const kUpArrowMesh = "mcrd_up.mesh";
const char *const kDownArrowMesh = "mcrd_down.mesh";
const char *const kListTitleText = "mcrd_listpan_title.txt";
const char *const kLitFont = "font1_pink_2";
const char *const kDimFont = "font1_pinkgrey_2";
constexpr int kRowPitch = 22;
constexpr int kRowCount = 15;
// A row wider than this text is drawn smaller, to this width, to stay inside the panel.
const char *const kWidestRow = "Super secret unlocked";

const char *const kHelpScreen = "MetHelpScreen";
const char *const kDataScreen = "MetRemixDataScreen";
const char *const kTitleScreen = "MetScreenTitleScreen";
const char *const kRightGizmoScreen = "MetRightGizmoScreen";
const char *const kOptionsScreen = "MetConfigOptionsButtonsScreen";
const char *const kLoadGameScreen = "MetLoadGameScreen";

// The song panel's heading, which reads REMIX DATA for the remix screens.
const char *const kDataTitleText = "mcrl_remixpan_title.txt";
const char *const kRemixDataTitle = "REMIX DATA";
const char *const kRunDataTitle = "BEST RUN";

constexpr int kSongTitleConfigCode = 0x325;

const char *const kBackPreset = "only_back_title";
const char *const kAcceptPreset = "standard_title";
const char *const kWatchPreset = "extras_watch";

enum Page {
    kPageMenu,
    kPageAchievements,
    kPageRuns,
};

struct Row {
    std::string text;
    bool bLit;
    std::string help; // the row's key in gMetHelpStrings
};

void SetDataTitle(const char *pszTitle) {
    if (Rnd::Text *pTitle =
            dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kDataTitleText)))) {
        pTitle->SetText(HxStr(pszTitle));
    }
}

// A Python string literal of text.
std::string Quoted(const std::string &text) {
    std::string quoted = "'";
    for (const char c : text) {
        quoted += c == '\'' || c == '\\' ? std::string("\\") + c : std::string(1, c);
    }
    return quoted + "'";
}

std::string Grouped(int nValue) {
    std::string digits = std::to_string(nValue);
    for (int nAt = static_cast<int>(digits.size()) - 3; nAt > 0; nAt -= 3) {
        digits.insert(nAt, ",");
    }
    return digits;
}

} // namespace

class MetExtrasScreen : public MetScreen, public ListDataProvider {
public:
    explicit MetExtrasScreen(MetRenderer *pRenderer)
        : MetScreen(pRenderer, 0, HxStr(kScreenName), HxStr(kDirectory), HxStr(kContainerName)) {
        mShowsLoadedDrawables = 0;
    }

    ~MetExtrasScreen() override {
        delete mList;
    }

    void EnterAndShow() override {
        SetShowing(0);
        mWatching = false;
        PushNamedScreen(HxStr(kHelpScreen));
        mList = new ScrollingList(
            this,
            kRowPitch,
            kRowCount,
            dynamic_cast<Rnd::View *>(Rnd::TheManager.Find(HxStr(kLineView))),
            dynamic_cast<Rnd::Mesh *>(Rnd::TheManager.Find(HxStr(kHighlightMesh))),
            dynamic_cast<Rnd::Mesh *>(Rnd::TheManager.Find(HxStr(kUpArrowMesh))),
            dynamic_cast<Rnd::Mesh *>(Rnd::TheManager.Find(HxStr(kDownArrowMesh))),
            0);
        MetScreenTitleScreen::SetTitle(HxStr("EXTRAS"));
        ShowPage(kPageMenu, 0);
        MetScreen::EnterAndShow();
    }

    void HandleCommand(const MetScreenCommand *pCommand) override {
        const int nSelected = mList->getSelected();
        switch (pCommand->mCommand) {
        case kMetScreenCommandPrevious:
            if (nSelected > 0) {
                mList->scrollUp();
                ShowSelection();
            }
            break;
        case kMetScreenCommandNext:
            if (nSelected + 1 < static_cast<int>(mRows.size())) {
                mList->scrollDown();
                ShowSelection();
            }
            break;
        case kMetScreenCommandSelect:
            if (mPage == kPageMenu) {
                ShowPage(nSelected == 0 ? kPageAchievements : kPageRuns, 0);
            } else if (mPage == kPageRuns && !mRuns.empty() &&
                       HostWatchBestRun(mRuns[nSelected].level.c_str(),
                                        mRuns[nSelected].nDifficulty) != 0) {
                mWatching = true;
                ActivateNamedPanel(HxStr(""));
                ExitScreenByName(HxStr(kTitleScreen));
                ExitScreenByName(HxStr(kHelpScreen));
                ExitScreenByName(HxStr(kDataScreen));
                BeginExit();
            }
            break;
        case kMetScreenCommandBack:
            if (mPage != kPageMenu) {
                ShowPage(kPageMenu, mPage == kPageAchievements ? 0 : 1);
                break;
            }
            ActivateNamedPanel(HxStr(""));
            BeginExit();
            break;
        default:
            break;
        }
    }

    void OnExitFinished() override {
        delete mList;
        mList = nullptr;
        mBases.clear();
        SetDataTitle(kRemixDataTitle);
        if (mWatching) {
            // The run plays as the attract demo does, and the game returns to the title screen.
            static_cast<MetLoadGameScreen *>(FindScreenByName(HxStr(kLoadGameScreen)))
                ->mDemoPlayback = 1;
            PushNamedScreen(HxStr(kLoadGameScreen));
            ActivateNamedPanel(HxStr(kLoadGameScreen));
            return;
        }
        PushNamedScreen(HxStr(kRightGizmoScreen));
        PushNamedScreen(HxStr(kOptionsScreen));
        ActivateNamedPanel(HxStr(kOptionsScreen));
    }

    int ProvideText(int nItem, int nColumn, Rnd::Text *pText, int) override {
        const bool bRow = nColumn == 0 && nItem < static_cast<int>(mRows.size());
        Rnd::Font *pFont = dynamic_cast<Rnd::Font *>(
            Rnd::TheManager.Find(HxStr(bRow && mRows[nItem].bLit ? kLitFont : kDimFont)));
        if (pFont != nullptr && pFont != pText->GetFont()) {
            pText->SetFont(pFont);
        }
        const std::string text = bRow ? mRows[nItem].text : std::string();
        pText->SetText(HxStr(text.c_str()));
        auto basis = mBases.find(pText);
        if (basis == mBases.end()) {
            basis = mBases.emplace(pText, Basis{}).first;
            memcpy(basis->second.af, pText->mLocalXfm, sizeof(basis->second.af));
        }
        const float flWidth = pText->GetFontWidth(text.c_str(), static_cast<int>(text.size()));
        const float flFit = pText->GetFontWidth(kWidestRow, static_cast<int>(strlen(kWidestRow)));
        const float flScale = flWidth > flFit ? flFit / flWidth : 1.0f;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                pText->mLocalXfm[i][j] = basis->second.af[i][j] * flScale;
            }
        }
        pText->mDirty = 1;
        return 1;
    }

    int ProvideMesh(int, int, Rnd::Mesh *, int) override {
        return 1;
    }

private:
    // Fills the list with a page's rows and selects row nSelected. The rows' help lines go to
    // gMetHelpStrings first.
    void ShowPage(Page page, int nSelected) {
        const Page previous = mPage;
        mPage = page;
        mRows.clear();
        std::string tab;
        std::string title;
        const char *pszPreset = kAcceptPreset;
        if (page == kPageMenu) {
            const std::vector<HostAchievement> achievements = HostAchievementList();
            int nUnlocked = 0;
            for (const HostAchievement &achievement : achievements) {
                nUnlocked += achievement.bUnlocked;
            }
            const int nRuns = static_cast<int>(HostBestRuns().size());
            mRows.push_back({"ACHIEVEMENTS",
                             true,
                             std::to_string(nUnlocked) + " of " +
                                 std::to_string(achievements.size()) + " unlocked"});
            mRows.push_back({"BEST RUNS",
                             true,
                             nRuns == 0 ? std::string("win a song to keep your best run") :
                                          std::to_string(nRuns) + (nRuns == 1 ? " run" : " runs") +
                                              " kept: watch your best of each song"});
            tab = "EXTRAS";
            title = "EXTRAS";
        } else if (page == kPageAchievements) {
            int nUnlocked = 0;
            for (const HostAchievement &achievement : HostAchievementList()) {
                const std::string text = achievement.pszTitle;
                const size_t nColon = text.find(": ");
                const char *pszStatus = achievement.bUnlocked ? "unlocked" : "not unlocked yet";
                mRows.push_back({text.substr(0, nColon),
                                 achievement.bUnlocked,
                                 nColon != std::string::npos ? text.substr(nColon + 2) :
                                                               std::string(pszStatus)});
                nUnlocked += achievement.bUnlocked;
                fprintf(stderr,
                        "extras: achievement %zu unlocked=%d %s\n",
                        mRows.size(),
                        achievement.bUnlocked,
                        achievement.pszTitle);
            }
            fprintf(stderr, "extras: achievements %d of %zu unlocked\n", nUnlocked, mRows.size());
            tab = std::to_string(nUnlocked) + " OF " + std::to_string(mRows.size());
            title = "ACHIEVEMENTS";
            pszPreset = kBackPreset;
        } else {
            mRuns = HostBestRuns();
            for (const HostBestRun &run : mRuns) {
                const HxStr song = QueryConfigString(kSongTitleConfigCode, run.level.c_str());
                const HxStr difficulty = DifficultyName(run.nDifficulty);
                mRows.push_back({std::string(song.mStr != nullptr ? song.mStr : run.level.c_str()),
                                 true,
                                 std::string(difficulty.mStr != nullptr ? difficulty.mStr : "") +
                                     "  <F2>" + Grouped(run.nScore) + "  <F1>points"});
                fprintf(stderr,
                        "extras: run %s-%d score=%d\n",
                        run.level.c_str(),
                        run.nDifficulty,
                        run.nScore);
            }
            fprintf(stderr, "extras: best runs %zu\n", mRuns.size());
            if (mRuns.empty()) {
                mRows.push_back({"No runs kept yet", false, "win a song to keep your best run"});
            }
            tab = "RUNS";
            title = "BEST RUNS";
            pszPreset = mRuns.empty() ? kBackPreset : kWatchPreset;
        }

        std::string update = "gMetHelpStrings.update({" + Quoted(kWatchPreset) +
                             ": '<FC> d <F2>WATCH    <FC> b <F2>BACK'";
        for (size_t i = 0; i < mRows.size(); ++i) {
            const std::string key = "extras_" + std::to_string(page) + "_" + std::to_string(i);
            update += ", " + Quoted(key) + ": " + Quoted(mRows[i].help);
            mRows[i].help = key;
        }
        EvalScriptExpression(HxStr((update + "})").c_str()));

        if (Rnd::Text *pTitle =
                dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kListTitleText)))) {
            pTitle->SetText(HxStr(tab.c_str()));
            pTitle->SetShowing(1);
        }
        MetScreenTitleScreen::ReplaceTitle(HxStr(title.c_str()));
        if (page == kPageRuns && !mRuns.empty()) {
            PushNamedScreen(HxStr(kDataScreen));
            SetDataTitle(kRunDataTitle);
        } else if (previous == kPageRuns) {
            ExitScreenByName(HxStr(kDataScreen));
            SetDataTitle(kRemixDataTitle);
        }
        MetHelpScreen::SelectPreset(HxStr(pszPreset));
        mList->setItemCount(static_cast<int>(mRows.size()));
        mList->setSelected(nSelected);
        mList->refresh();
        ShowSelection();
        static const char *const kPageBreadcrumbs[] = {
            "EXTRAS_MENU", "EXTRAS_ACHIEVEMENTS", "EXTRAS_BEST_RUNS"};
        HostBreadcrumb(kPageBreadcrumbs[page]);
    }

    void ShowSelection() {
        const int nSelected = mList->getSelected();
        MetHelpScreen::SetText(HxStr(mRows[nSelected].help.c_str()), mRenderer->mAnimationFrame);
        if (mPage == kPageRuns && !mRuns.empty()) {
            mRecord = MetRemixRecord();
            mRecord.levelName = HxStr(mRuns[nSelected].level.c_str());
            mRecord.dateTime = DifficultyName(mRuns[nSelected].nDifficulty);
            mRecord.albumNumber = GetAlbumJukeboxValue();
            static_cast<MetRemixDataScreen *>(FindScreenByName(HxStr(kDataScreen)))
                ->ShowRecord(&mRecord);
        }
    }

    Page mPage = kPageMenu;
    std::vector<Row> mRows;
    std::vector<HostBestRun> mRuns;
    MetRemixRecord mRecord;
    ScrollingList *mList = nullptr;
    // Each row text's basis as the list made it, which ProvideText() scales.
    struct Basis {
        float af[3][Rnd::kXfmRowFloatCount];
    };
    std::map<Rnd::Text *, Basis> mBases;
    bool mWatching = false;
};

// Opens the screen from the fifth button of Options (src/met/metconfigoptionsbuttonsscreen.cpp),
// registering it on first use.
void HostOpenExtras(MetScreen *pFrom, MetRenderer *pRenderer) {
    if (MetScreen::FindScreenByName(HxStr(kScreenKey)) == nullptr) {
        MetScreen::ScreenRegistry()[HxStr(kScreenKey)] =
            MetScreenEntry(new MetExtrasScreen(pRenderer));
    }
    pFrom->PushNamedScreen(HxStr(kScreenKey));
    pFrom->ActivateNamedPanel(HxStr(kScreenKey));
}

// The help line of the fifth button of Options, as a gMetHelpStrings key.
HxStr HostExtrasPrompt() {
    EvalScriptExpression(HxStr("gMetHelpStrings.update({'extras': 'achievements and best runs'})"));
    return HxStr("extras");
}
