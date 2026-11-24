#include "met/metpausesologamescreen.h"

#include "app/application.h"
#include "game/gamemanagerimpl.h"
#include "game/gameparams.h"
#include "met/metfrontendstate.h"
#include "met/metstrings.h"
#include "os/formatstring.h"
#include "os/hxstr.h"
#include "rnd/manager.h"
#include "rnd/text.h"
#ifdef RESONANCE_HOST
#include "game/grooveworld.h"
#include "gfx/hostframe.h"
#include "sch/tickclock.h"

extern "C" void HostPhotoStart();                        // host/io/pad.cpp
extern "C" int HostPracticeArmed(void);                  // host/audio/audio.cpp
extern "C" int HostPracticeLoop(int *pFirst, int *pEnd); // host/audio/audio.cpp
extern "C" int HostPracticeLoopHere(int nBar);           // host/audio/audio.cpp

namespace {

// The host adds two rows, Photo Mode on L1 and, while practising, Loop Section on R1. Loop Section
// restarts the song with the playing phrase looped (Next Section once a loop is set, to loop the
// phrase after it). Each row is a copy of the last row's label with its button drawn from the
// controller font ("e" is L1 and "g" is R1 in the help strings' glyph letters). The rows are
// spread between the first and last of the original five.
constexpr int kCommandPhotoMode = 11;   // L1
constexpr int kCommandLoopSection = 13; // R1
const char *const kGlyphTexts[] = {"psgr_start.txt",
                                   "psgr_triangle.txt",
                                   "psgr_x.txt",
                                   "psgr_square.txt",
                                   "psgr_circle.txt",
                                   "psgr_l1.txt",
                                   "psgr_r1.txt"};
const char *const kNewGlyphs[] = {"e", "g"};
const char *const kNewLabels[] = {"psgr_opt6.txt", "psgr_opt7.txt"};
constexpr int kHostRowCount = 7;
constexpr float kTicksPerBar = 1920.0f;
float g_flFirstRow;
float g_flLastRow;

Rnd::Text *FindText(const char *pszName) {
    return dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(pszName)));
}

// A copy of pSource under pszName, drawn and moved with it.
Rnd::Text *CopyText(Rnd::Text *pSource, const char *pszName) {
    Rnd::Text *pCopy = FindText(pszName);
    if (pCopy != nullptr) {
        return pCopy;
    }
    pCopy = Rnd::Text::sNew(HxStr(pszName));
    pCopy->Copy(pSource, 0);
    if (Rnd::Drawable *pDrawParent = pSource->Drawable::Parent()) {
        pDrawParent->AddDraw(pCopy, pSource);
    }
    if (Rnd::Transformable *pTransParent = pSource->Transformable::Parent()) {
        pTransParent->AddTrans(pCopy);
    }
    return pCopy;
}

HxStr LoopLabel() {
    int nFirst, nEnd;
    return HxStr(HostPracticeLoop(&nFirst, &nEnd) != 0 ? "Next Section" : "Loop Section");
}

} // namespace
#endif
#ifndef RESONANCE_HOST
#line 11
#endif

namespace {

static const char *const kScreenName = "psgr";
static const char *const kDirectory = "metagame/Transition";
static const char *const kContainerName = "pause_solo";
static const char *const kPanelName = "MetPauseSoloGameScreen";

// Counted from 1.
static const char *const kOptionTextFormat = "psgr_opt%d.txt";
static const char *const kPausedText = "psgr_paused.txt";

static const char *const kRemixHeadingKey = "pause_remix";
static const char *const kGameHeadingKey = "pause_game";
static const char *const kGameLabelsKey = "pause_solo_game";
static const char *const kRemixLabelsKey = "pause_solo_remix";

static const char *const kNoText = "";
static const char *const kTitleScreen = "MetScreenTitleScreen";
static const char *const kControllerScreen = "MetConfigControllerScreen";
static const char *const kGameOptionsScreen = "MetConfigGameOptionsScreen";
static const char *const kHelpScreen = "MetHelpScreen";

constexpr int kOptionCount = 5;

constexpr int kPromptConfigCode = 0x258;
constexpr int kLabelsConfigCode = 0x259;

// Command codes past MetScreenCommandCode's range. Their names are inferred from the screens
// they open.
constexpr int kCommandGameOptions = 7;
constexpr int kCommandController = 8;
constexpr int kCommandIgnored = 9;
constexpr int kCommandResume = 10;

} // namespace

MetPauseSoloGameScreen::MetPauseSoloGameScreen(MetRenderer *pRenderer, int nPriority)
    : MetPauseBaseScreen(
          pRenderer, nPriority, HxStr(kScreenName), HxStr(kDirectory), HxStr(kContainerName)),
      mLeavingForConfig(0) {
    mReturnPanel = kPanelName;
}

void MetPauseSoloGameScreen::ResolveContainerViews() {
    MetScreen::ResolveContainerViews();
    for (int i = 1; i <= kOptionCount; ++i) {
        Rnd::Text *pOption = dynamic_cast<Rnd::Text *>(
            Rnd::TheManager.Find(HxStr(Rnd::MakeString(kOptionTextFormat, i))));
        mOptionTexts.push_back(pOption);
    }
#ifdef RESONANCE_HOST
    g_flFirstRow = mOptionTexts.front()->mLocalXfm[3][2];
    g_flLastRow = mOptionTexts.back()->mLocalXfm[3][2];
    Rnd::Text *pLabel = mOptionTexts.back();
    Rnd::Text *pGlyph = FindText(kGlyphTexts[kOptionCount - 1]);
    for (int i = 0; i < kHostRowCount - kOptionCount; ++i) {
        mOptionTexts.push_back(CopyText(pLabel, kNewLabels[i]));
        CopyText(pGlyph, kGlyphTexts[kOptionCount + i])->SetText(HxStr(kNewGlyphs[i]));
    }
#endif
#ifndef RESONANCE_HOST
#line 62
#endif
}

void MetPauseSoloGameScreen::HandleCommand(const MetScreenCommand *pCommand) {
    switch (pCommand->mCommand) {
    case kMetScreenCommandSelect:
    case kMetScreenCommandBack:
    case kCommandResume:
        MetPauseBaseScreen::HandleCommand(pCommand);
        return;
    case kCommandController:
        PlayPauseSound(pCommand->mPadIndex);
        mLeavingForConfig = 1;
        mExitAction = kExitController;
        break;
    case kCommandGameOptions:
        PlayPauseSound(pCommand->mPadIndex);
        mExitAction = kExitGameOptions;
        mLeavingForConfig = 1;
        break;
#ifdef RESONANCE_HOST
    case kCommandPhotoMode:
        PlayPauseSound(pCommand->mPadIndex);
        HostPhotoStart();
        return;
    case kCommandLoopSection: {
        Sch::TickClock *pClock = Application::shared()->GetWorld()->GetSongClock();
        if (HostPracticeLoopHere(static_cast<int>(pClock->SongTick() / kTicksPerBar)) == 0) {
            return;
        }
        PlayPauseSound(pCommand->mPadIndex);
        mExitAction = kExitRestart; // a practice run restarts at once, now with the loop
        break;
    }
#endif
#ifndef RESONANCE_HOST
#line 81
#endif
    case kCommandIgnored:
    default:
        return;
    }
    ActivateNamedPanel(HxStr(kNoText));
    BeginExit();
}

void MetPauseSoloGameScreen::EnterAndShow() {
    GameParams params(*Application::shared()->GetGameManager()->GetParams());
    Rnd::Text *pPaused = dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kPausedText)));

    HxStr heading = params.mPlayMode == kPlayModeJam ?
                        MetConfigText(kMetStrPauseRemix, kPromptConfigCode, kRemixHeadingKey) :
                        MetConfigText(kMetStrPauseGame, kPromptConfigCode, kGameHeadingKey);
    pPaused->SetText(heading);

    mOptionLabels.clear();
#ifdef VIDEO_STANDARD_PAL
    mOptionLabels.push_back(GetMetString(kMetStrPauseResume));
    mOptionLabels.push_back(GetMetString(kMetStrPauseQuit));
    if (params.mPlayMode == kPlayModeGame) {
        mOptionLabels.push_back(GetMetString(kMetStrPauseRestart));
    }
    mOptionLabels.push_back(GetMetString(kMetStrPauseConfig));
    mOptionLabels.push_back(GetMetString(kMetStrPauseSettings));
#else
    QueryConfigStrings(&mOptionLabels,
                       kLabelsConfigCode,
                       params.mPlayMode == kPlayModeGame ? kGameLabelsKey : kRemixLabelsKey);
#endif
#ifdef RESONANCE_HOST
    mOptionLabels.resize(kOptionCount);
    mOptionLabels.push_back(HxStr("Photo Mode"));
    mOptionLabels.push_back(HostPracticeArmed() != 0 ? LoopLabel() : HxStr(""));
#endif
#ifndef RESONANCE_HOST
#line 112
#endif
    // Yes, the binary copies the labels here and again in the base slot.
    for (int i = 0; i < kOptionCount; ++i) {
        mOptionTexts[i]->SetText(mOptionLabels[i]);
    }
    MetPauseBaseScreen::EnterAndShow();
#ifdef RESONANCE_HOST
    const int nRows = HostPracticeArmed() != 0 ? kHostRowCount : kHostRowCount - 1;
    for (int i = 0; i < kHostRowCount; ++i) {
        Rnd::Text *pGlyph = FindText(kGlyphTexts[i]);
        const float flRow = g_flFirstRow + (g_flLastRow - g_flFirstRow) * i / (nRows - 1);
        for (Rnd::Text *pText : {mOptionTexts[i], pGlyph}) {
            pText->mLocalXfm[3][2] = flRow;
            pText->mDirty = 1;
            pText->SetShowing(i < nRows);
        }
    }
    HostEvent("pause rows=%d", nRows);
#endif
#ifndef RESONANCE_HOST
#line 117
#endif
    mLeavingForConfig = 0;
}

void MetPauseSoloGameScreen::OnExitFinished() {
    if (mLeavingForConfig == 0) {
        MetPauseBaseScreen::OnExitFinished();
        return;
    }

    const char *pszConfigScreen;
    if (mExitAction == kExitController) {
        pszConfigScreen = kControllerScreen;
    } else if (mExitAction == kExitGameOptions) {
        pszConfigScreen = kGameOptionsScreen;
    } else {
        return;
    }
    MetFrontEndState::shared()->mReturnScreen = HxStr(kPanelName);
    PushNamedScreen(HxStr(kTitleScreen));
    PushNamedScreen(HxStr(pszConfigScreen));
    PushNamedScreen(HxStr(kHelpScreen));
    ActivateNamedPanel(HxStr(pszConfigScreen));
}

MetPauseSoloGameScreen *MetPauseSoloGameScreen::New(MetRenderer *pRenderer, int nPriority) {
    return new MetPauseSoloGameScreen(pRenderer, nPriority);
}
