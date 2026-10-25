#include "met/metconfiggameoptionsscreen.h"

#include <cstdio>
#include <vector>

#include "app/application.h"
#include "game/forcefeedbackmgr.h"
#include "game/globalsettings.h"
#include "game/grooveworld.h"
#include "met/metfrontendstate.h"
#include "met/metglobalsettingssaverscreen.h"
#include "met/methelpscreen.h"
#include "met/metrenderer.h"
#include "met/metscreentitlescreen.h"
#include "met/metstrings.h"
#include "mid/tick.h"
#include "os/hxstr.h"
#include "rnd/button.h"
#include "rnd/manager.h"
#include "rnd/text.h"
#include "script/configquery.h"
#include "synth/ps2hardsynth.h"

namespace {

static const char *const kScreenName = "nop";
static const char *const kDirectory = "metagame/shared";
static const char *const kContainerName = "net_options_pangame";

constexpr int kPromptConfigCode = 0x258;
constexpr int kTitleConfigCode = 0x269;

// The two rows, in MetButtonList order.
enum Row {
    kRowAudio = 0,
    kRowForceFeedback = 1,
    kRowCount = 2,
};

// MetScreen::mExitChoice on exit, read back by OnExitFinished().
constexpr int kExitCancelled = 0;
constexpr int kExitApplied = 2;

constexpr int kFirstRow = 0;
constexpr int kFrontEndFlagSet = 1;
constexpr int kArrowFlashCycles = 2;
constexpr float kArrowFlashInterval = 30.0f;
constexpr int kArrowNameBufferSize = 112;

static const char *const kAudioRowKey = "pangame_audio";
static const char *const kForceFeedbackRowKey = "pangame_force_feedback";

static const char *const kAudioLabel = "nop_audio.txt";
static const char *const kAudioLabelKey = "pangame_audio_lbl";
static const char *const kForceFeedbackLabel = "nop_feedback.txt";
static const char *const kForceFeedbackLabelKey = "pangame_force_lbl";
static const char *const kAudioButton = "pangame_audio.but";
static const char *const kForceFeedbackButton = "pangame_force_feedback.but";
// Counted from 1.
static const char *const kLeftArrowFormat = "arr_left_0%i.but";
static const char *const kRightArrowFormat = "arr_right_0%i.but";

static const char *const kTitleKey = "pangame_options";
static const char *const kTabPreset = "pangame_tab_text";

static const char *const kStereoText = "STEREO";
static const char *const kMonoText = "MONO";
static const char *const kOnText = "ON";
static const char *const kOffText = "OFF";

static const char *const kPauseGameScreen = "MetPauseSoloGameScreen";
static const char *const kPauseRemixScreen = "MetPauseSoloRemixScreen";
static const char *const kHelpScreen = "MetHelpScreen";
static const char *const kTitleScreen = "MetScreenTitleScreen";
static const char *const kRightGizmoScreen = "MetRightGizmoScreen";
static const char *const kOptionsButtonsScreen = "MetConfigOptionsButtonsScreen";

inline Rnd::Button *FindArrow(const char *pszFormat, int nNumber) {
    char szName[kArrowNameBufferSize];
    sprintf(szName, pszFormat, nNumber);
    return dynamic_cast<Rnd::Button *>(Rnd::TheManager.Find(HxStr(szName)));
}

} // namespace

#ifdef RESONANCE_HOST
#include <algorithm>
#include <cstdlib>

#include "app/playsound.h"
#include "app/timeclock.h"
#include "gfx/hostframe.h"
#include "sch/cmdid.h"
#include "sch/command.h"

extern "C" int HostSaveSetting(const char *pszKey, const char *pszValue); // host/core/settings.c

namespace {

// Latency calibration, Square on this screen. The game's own key click sounds every 30 NTSC blanks;
// the player taps any button but Triangle with it, which cancels. After kCalibrationTaps taps the
// median of tap minus nearest click is the judging offset, FREQ_INPUT_OFFSET_MS, kept in the
// settings file. Times are the watchdog's, as presses in a song are.
constexpr long long kClickNs = 500500000;
constexpr int kCalibrationTaps = 12;
constexpr int kSquareCommand = 8; // MetCommandMap's code for Square

bool g_bCalibrating = false;
long long g_nFirstClickNs = 0;
std::vector<long long> g_lags;
Sch::CmdID g_clickCommand;

long long WatchdogTimerNs() {
    return Application::shared()->GetWatchdogTimer()->Now();
}

class CalibrationClickCmd : public Sch::Command {
public:
    virtual int CmdID() {
        return sCmdID;
    }
    virtual void Execute();
    virtual void Print(std::ostream &stream) {
        stream << "{Calibration/Click}";
    }
    static int sCmdID;
};

int CalibrationClickCmd::sCmdID;

void PostClick(long long nInNs) {
    CalibrationClickCmd *pCommand = new CalibrationClickCmd;
    g_clickCommand.mValue = -2; // unallocated
    Application::shared()->GetWatchdogTimer()->PostIn(pCommand, Sch::Time{nInNs}, g_clickCommand, 0);
    pCommand->Release();
}

void CalibrationClickCmd::Execute() {
    PlaySoundByName("SND_MET_KEY1");
    HostEvent("calclick ns=%lld", WatchdogTimerNs());
    PostClick(kClickNs);
}

void StopCalibration() {
    g_bCalibrating = false;
    Application::shared()->GetWatchdogTimer()->Withdraw(g_clickCommand);
}

// Handle a command while calibrating, or Square to start; false leaves it to the screen.
bool CalibrationCommand(int nCommand, Rnd::Text *pLabel, Rnd::Text *pValue, const HxStr &label) {
    char szText[32];
    if (!g_bCalibrating) {
        if (nCommand != kSquareCommand) {
            return false;
        }
        g_bCalibrating = true;
        g_lags.clear();
        g_nFirstClickNs = WatchdogTimerNs() + kClickNs;
        PostClick(kClickNs);
        pLabel->SetText(HxStr("LATENCY"));
        snprintf(szText, sizeof(szText), "TAP %d", kCalibrationTaps);
        pValue->SetText(HxStr(szText));
        return true;
    }
    if (nCommand == kMetScreenCommandBack) {
        StopCalibration();
        pLabel->SetText(label);
        return false; // and the screen leaves as Back does
    }
    const long long nSince = WatchdogTimerNs() - g_nFirstClickNs;
    if (nSince < -kClickNs / 2) {
        return true; // before the first click
    }
    long long nLag = (nSince % kClickNs + kClickNs) % kClickNs;
    if (nLag >= kClickNs / 2) {
        nLag -= kClickNs;
    }
    g_lags.push_back(nLag);
    HostEvent("caltap lag_ns=%lld", nLag);
    if (static_cast<int>(g_lags.size()) < kCalibrationTaps) {
        snprintf(szText, sizeof(szText), "TAP %d", kCalibrationTaps - static_cast<int>(g_lags.size()));
        pValue->SetText(HxStr(szText));
        return true;
    }
    StopCalibration();
    pLabel->SetText(label);
    std::sort(g_lags.begin(), g_lags.end());
    const long long nMedian = (g_lags[kCalibrationTaps / 2 - 1] + g_lags[kCalibrationTaps / 2]) / 2;
    const int nMs = static_cast<int>((nMedian + (nMedian < 0 ? -500000 : 500000)) / 1000000);
    snprintf(szText, sizeof(szText), "%d", nMs);
    const int bSaved = HostSaveSetting("FREQ_INPUT_OFFSET_MS", szText);
    HostEvent("calibration lag_ms=%d median_ns=%lld saved=%d", nMs, nMedian, bSaved);
    snprintf(szText, sizeof(szText), "LAG %d MS", nMs);
    pValue->SetText(HxStr(szText));
    return true;
}

} // namespace
#endif

#ifndef RESONANCE_HOST
#line 85
#endif
MetConfigGameOptionsScreen::MetConfigGameOptionsScreen(MetRenderer *pRenderer, int nPriority)
    : MetScreenMultiSoundBank(
          pRenderer, nPriority, HxStr(kScreenName), HxStr(kDirectory), HxStr(kContainerName)),
      mRows(nullptr) {
    mRows = new MetButtonList();
    mHelpKeys.push_back(MetText(kMetStrHPangameAudio, kAudioRowKey));
    mHelpKeys.push_back(MetText(kMetStrHPangameForceFeedback, kForceFeedbackRowKey));
}

void MetConfigGameOptionsScreen::ResolveContainerViews() {
    MetScreenMultiSoundBank::ResolveContainerViews();

    // Yes, the binary does not test either label for null.
    dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kAudioLabel)))
        ->SetText(MetConfigText(kMetStrPangameAudioLbl, kPromptConfigCode, kAudioLabelKey));
    dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kForceFeedbackLabel)))
        ->SetText(MetConfigText(kMetStrPangameForceLbl, kPromptConfigCode, kForceFeedbackLabelKey));

    mRows->Add(HxStr(kAudioButton),
               MetConfigText(kMetStrPangameAudio, kPromptConfigCode, kAudioRowKey));
    mRows->Add(HxStr(kForceFeedbackButton),
               MetConfigText(kMetStrPangameForceFeedback, kPromptConfigCode, kForceFeedbackRowKey));

    mLeftArrows.resize(kRowCount);
    mRightArrows.resize(kRowCount);
    for (int nRow = 0; nRow < kRowCount; ++nRow) {
        mLeftArrows[nRow] = FindArrow(kLeftArrowFormat, nRow + 1);
        mRightArrows[nRow] = FindArrow(kRightArrowFormat, nRow + 1);
    }
}

MetConfigGameOptionsScreen::~MetConfigGameOptionsScreen() {
    delete mRows;
}

void MetConfigGameOptionsScreen::HandleCommand(const MetScreenCommand *pCommand) {
#ifdef RESONANCE_HOST
    if (CalibrationCommand(pCommand->mCommand,
                           dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kAudioLabel))),
                           mRows->GetButton(kRowAudio)->mText,
                           MetConfigText(kMetStrPangameAudioLbl, kPromptConfigCode, kAudioLabelKey))) {
        return;
    }
#endif
#ifndef RESONANCE_HOST
#line 121
#endif
    switch (pCommand->mCommand) {
    case kMetScreenCommandPrevious:
        mRows->SelectPrevious();
        MetHelpScreen::SetText(mHelpKeys[mRows->mSelected], mRenderer->mAnimationFrame);
        break;

    case kMetScreenCommandNext:
        mRows->SelectNext();
        MetHelpScreen::SetText(mHelpKeys[mRows->mSelected], mRenderer->mAnimationFrame);
        break;

    case kMetScreenCommandLeft: {
        const int nRow = mRows->mSelected;
        StartRepeatingSound(
            mRenderer->mAnimationFrame, kArrowFlashInterval, mLeftArrows[nRow], kArrowFlashCycles);
        ToggleOption(nRow);
        break;
    }

    case kMetScreenCommandRight: {
        const int nRow = mRows->mSelected;
        StartRepeatingSound(
            mRenderer->mAnimationFrame, kArrowFlashInterval, mRightArrows[nRow], kArrowFlashCycles);
        ToggleOption(nRow);
        break;
    }

    case kMetScreenCommandSelect:
        ActivateNamedPanel(HxStr(""));
        ApplyOptions();
        mExitChoice = kExitApplied;
        ExitScreenByName(HxStr(kTitleScreen));
        ExitScreenByName(HxStr(kHelpScreen));
        BeginExit();
        break;

    case kMetScreenCommandBack:
        mExitChoice = kExitCancelled;
        if (MetFrontEndState::shared()->mReturnScreen == kPauseGameScreen ||
            MetFrontEndState::shared()->mReturnScreen == kPauseRemixScreen) {
            ExitScreenByName(HxStr(kHelpScreen));
            ExitScreenByName(HxStr(kTitleScreen));
        }
        BeginExit();
        break;

    default:
        break;
    }
}

void MetConfigGameOptionsScreen::EnterAndShow() {
    mRows->SetSelected(kFirstRow);
    MetScreenTitleScreen::SetTitle(
        MetConfigText(kMetStrTPangameOptions, kTitleConfigCode, kTitleKey));
    MetHelpScreen::SetText(mHelpKeys[mRows->mSelected], mRenderer->mAnimationFrame);
#ifdef VIDEO_STANDARD_PAL
    if (MetFrontEndState::shared()->mReturnScreen == kPauseGameScreen ||
        MetFrontEndState::shared()->mReturnScreen == kPauseRemixScreen) {
        MetHelpScreen::SelectPreset(GetMetString(kMetStrHStandardTitle));
    } else {
        MetHelpScreen::SelectPreset(GetMetString(kMetStrHPangameTabText));
    }
#else
    MetHelpScreen::SelectPreset(HxStr(kTabPreset));
#endif
    mOptions = GlobalSettings::shared()->mGameOptions;
    UpdateOptionLabels();
    MetScreenMultiSoundBank::EnterAndShow();
}

void MetConfigGameOptionsScreen::UpdateOptionLabels() {
    mRows->GetButton(kRowAudio)->mText->SetText(mOptions.mStereo != 0 ?
                                                    MetText(kMetStrGsStereo, kStereoText) :
                                                    MetText(kMetStrGsMono, kMonoText));
    mRows->GetButton(kRowForceFeedback)
        ->mText->SetText(mOptions.mForceFeedback != 0 ? MetText(kMetStrGsFfOn, kOnText) :
                                                        MetText(kMetStrGsFfOff, kOffText));
}

void MetConfigGameOptionsScreen::OnExitFinished() {
    if (MetFrontEndState::shared()->mReturnScreen == kPauseGameScreen ||
        MetFrontEndState::shared()->mReturnScreen == kPauseRemixScreen) {
        if (MetFrontEndState::shared()->mUsingMemcard == kFrontEndFlagSet &&
            mExitChoice != kExitCancelled) {
            MetFrontEndState::shared()->mSettingsDirty = kFrontEndFlagSet;
        }
        if (MetFrontEndState::shared()->mReturnScreen == kPauseGameScreen) {
            PushNamedScreen(HxStr(kPauseGameScreen));
            ActivateNamedPanel(HxStr(kPauseGameScreen));
        } else {
            ExitScreenByName(HxStr(kHelpScreen));
            PushNamedScreen(HxStr(kPauseRemixScreen));
            ActivateNamedPanel(HxStr(kPauseRemixScreen));
        }
        MetFrontEndState::shared()->mReturnScreen = HxStr("");
        return;
    }

    if (mExitChoice == kExitCancelled) {
        PushNamedScreen(HxStr(kRightGizmoScreen));
        PushNamedScreen(HxStr(kOptionsButtonsScreen));
        ActivateNamedPanel(HxStr(kOptionsButtonsScreen));
        return;
    }

    GlobalSettings::shared()->mGameOptions = mOptions;
    std::vector<HxStr> screens;
    screens.push_back(HxStr(kOptionsButtonsScreen));
    screens.push_back(HxStr(kRightGizmoScreen));
    screens.push_back(HxStr(kHelpScreen));
    MetGlobalSettingsSaverScreen::StartSave(screens);
}

MetConfigGameOptionsScreen *MetConfigGameOptionsScreen::New(MetRenderer *pRenderer, int nPriority) {
    return new MetConfigGameOptionsScreen(pRenderer, nPriority);
}

void MetConfigGameOptionsScreen::ToggleOption(int nRow) {
    if (nRow == kRowAudio) {
        mOptions.mStereo ^= 1;
    } else if (nRow == kRowForceFeedback) {
        mOptions.mForceFeedback ^= 1;
    }
    UpdateOptionLabels();
}

void MetConfigGameOptionsScreen::ApplyOptions() {
    GlobalSettings::shared()->mGameOptions = mOptions;
    Application::shared()->GetSynth()->SetStereo(mOptions.mStereo);
    GrooveWorld *pWorld = Application::shared()->GetWorld();
    if (pWorld != nullptr) {
        pWorld->mForceFeedback->SetEnabled(mOptions.mForceFeedback);
        pWorld->mForceFeedback->StartMetronome(Sch::Tick(0));
    }
}
