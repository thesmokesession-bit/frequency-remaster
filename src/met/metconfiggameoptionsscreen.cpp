#include "met/metconfiggameoptionsscreen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#ifdef RESONANCE_HOST
static const char *const kHintText = "host_calibrate_hint.txt";
#endif
#ifndef RESONANCE_HOST
#line 62
#endif
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

// The remaster's settings, Circle on this screen: Up and Down pick one, Left and Right change it,
// and each change is kept in the settings file at once (HostSaveSetting). Circle or Triangle
// leaves. The first row shows the setting and its value, the second when it takes effect (a
// setting read at start-up from the next start, the others from the next song), and the hint
// line below them its help. The first value of a list is the one an unset key has. A row with a
// step is a number of ms within plus or minus its range of its default.
constexpr int kCircleCommand = 7; // MetCommandMap's code for Circle

static const char *const kHint = "SQUARE: CALIBRATE\nCIRCLE: SETTINGS";

struct HostSetting {
    const char *pszKey, *pszLabel;
    bool bAtStart;
    const char *apszValues[7], *apszNames[7];
    const char *pszHelp;
    int nRange = 0, nStep = 0, nDefault = 0;
};

// clang-format off
const HostSetting kHostSettings[] = {
    {"FREQ_SCALE", "SHARPNESS", true, {"2", "3", "4", "5", "6"}, {"2X", "3X", "4X", "5X", "6X"},
     "TIMES THE PS2 PIXELS, 5X FILLS A 4K TV"},
    {"FREQ_FPS", "FRAME RATE", true, {"60", "120"}, {"60", "120"},
     "120 FOR A 120 HZ TV, 60 AS THE ORIGINAL"},
    {"FREQ_WIDESCREEN", "WIDESCREEN", true, {"0", "1"}, {"OFF", "ON"},
     "A 16:9 PICTURE, OFF FOR THE ORIGINAL 4:3"},
    {"FREQ_HUD_EDGES", "HUD AT EDGES", true, {"0", "1"}, {"OFF", "ON"},
     "IN 16:9 THE HUD SITS AT THE SCREEN EDGES"},
    {"FREQ_SCALE_EFFECTS", "EFFECT SIZE", true, {"on", "off"}, {"SCALED", "THIN"},
     "LINES AND BLUR AT PS2 SIZE, OR ONE PIXEL"},
    {"FREQ_TEX_FILTER", "TEXTURES", true, {"enhanced", "original"}, {"ENHANCED", "ORIGINAL"},
     "ENHANCED STOPS FAR WALLS SHIMMERING"},
    {"FREQ_COLOR_DEPTH", "COLOUR DEPTH", true, {"full", "original"}, {"FULL", "ORIGINAL"},
     "FULL REMOVES BANDING IN GLOWS AND TRAILS"},
    {"FREQ_AA", "SMOOTH EDGES", true, {"off", "on"}, {"OFF", "ON"},
     "FXAA ON EDGES, SOFTENS HUD TEXT A LITTLE"},
    {"FREQ_TRAILS", "TRAILS", true, {"1", "0.5", "0"}, {"FULL", "HALF", "OFF"},
     "STRENGTH OF THE MOTION TRAILS AND BLUR"},
    {"FREQ_REDUCE_FLASH", "REDUCE FLASH", false, {"0", "0.5", "1"}, {"OFF", "HALF", "NO FLASH"},
     "DIMS THE WHITE FLASH OF EACH CAUGHT GEM"},
    {"FREQ_GEM_PALETTE", "GEM COLOURS", true, {"original", "cb"}, {"ORIGINAL", "COLOURBLIND"},
     "COLOURBLIND KEEPS RED AND GREEN APART"},
    {"FREQ_SURROUND", "SURROUND 5.1", true, {"0", "1"}, {"OFF", "ON"},
     "SOUND ON 5.1 SPEAKERS, OFF FOR STEREO"},
    {"FREQ_SPATIAL", "SPATIAL MIX", true, {"0", "1"}, {"OFF", "ON"},
     "WITH 5.1, EACH LANE PLAYS FROM ITS PLACE"},
    {"FREQ_SPU_INTERP", "SOUND FILTER", true, {"gaussian", "linear", "cubic", "sinc"},
     {"GAUSSIAN", "LINEAR", "CUBIC", "SINC"}, "GAUSSIAN SOUNDS AS THE PS2, SINC CLEAREST"},
    {"FREQ_AUDIO_OFFSET_MS", "AUDIO OFFSET", true, {}, {},
     "PLAYS THE SOUND LATER, MINUS FOR EARLIER", 250, 5},
    {"FREQ_VIDEO_OFFSET_MS", "VIDEO OFFSET", false, {}, {},
     "MOVE UNTIL GEMS MEET THE CATCHER ON THE BEAT", 250, 5},
    {"FREQ_INPUT_OFFSET_MS", "INPUT OFFSET", false, {}, {},
     "JUDGES PRESSES EARLIER, SQUARE MEASURES IT", 250, 5},
    {"FREQ_NO_FAIL", "NO FAIL", false, {"0", "1"}, {"OFF", "ON"},
     "THE SONG GOES ON WHEN ENERGY RUNS OUT"},
    {"FREQ_TIMING_WINDOW", "TIMING WINDOW", false, {"1.0", "1.5", "2.0"}, {"1X", "1.5X", "2X"},
     "WIDER FORGIVES EARLY AND LATE PRESSES"},
    {"FREQ_UNLOCK_ALL", "UNLOCK ALL", false, {"0", "1"}, {"OFF", "ON"},
     "EVERY ARENA AND SONG OPEN, SCORES STILL SAVE"},
    {"FREQ_BONUS_SONGS", "BONUS SONGS", true, {"1", "0"}, {"ON", "OFF"},
     "THE TWO CUT SONGS ON THE STAGE SCREEN"},
    {"FREQ_FAST_BOOT", "FAST BOOT", true, {"1", "0"}, {"ON", "OFF"},
     "SKIPS THE SONY SCREEN AND INTRO MOVIE"},
    {"FREQ_WATCH", "WATCH MODE", false, {"0", "1"}, {"OFF", "ON"},
     "THE GAME PLAYS ITSELF, NO SCORE IS SAVED"},
    {"FREQ_PRACTICE", "PRACTICE", true, {"0", "1"}, {"OFF", "ON"},
     "NO ENERGY DRAIN OR SAVING, CREATE RESTARTS"},
    {"FREQ_PRACTICE_SPEED", "PRACTICE SPEED", true, {"1", "0.9", "0.75", "0.5"},
     {"100%", "90%", "75%", "50%"}, "SLOWER AT THE SAME PITCH, A PRACTICE RUN"},
    {"FREQ_AUTOPAUSE", "AUTO-PAUSE", true, {"1", "0"}, {"ON", "OFF"},
     "PAUSES WHEN A PAD DROPS OR ON THE PS BUTTON"},
    {"FREQ_LIGHTBAR", "LIGHT BAR", false, {"1", "0"}, {"ON", "OFF"},
     "THE LIGHT BAR SHOWS YOUR PLAYER COLOUR"},
    {"FREQ_TRIGGERS", "TRIGGERS", false, {"1", "0"}, {"ON", "OFF"},
     "L2 AND R2 CLICK WHEN THEY PLAY A LANE"},
    {"FREQ_RUMBLE_LEAD_MS", "RUMBLE LEAD", false, {}, {},
     "STARTS THE BEAT RUMBLE THIS EARLY", 90, 10, 90},
};
// clang-format on
constexpr int kHostSettingCount = sizeof(kHostSettings) / sizeof(kHostSettings[0]);

bool g_bHostSettings = false;
int g_nHostSetting = 0;

int ValueCount(const HostSetting &setting) {
    int n = 0;
    while (n < 7 && setting.apszValues[n] != nullptr) {
        ++n;
    }
    return n;
}

// The setting's current value as an index into its list (the first when unset or unknown; for
// the scale, the PS5's own default of 5x).
int CurrentValue(const HostSetting &setting) {
    const char *pszValue = getenv(setting.pszKey);
    for (int i = 0; pszValue != nullptr && i < ValueCount(setting); ++i) {
        if (strcmp(pszValue, setting.apszValues[i]) == 0) {
            return i;
        }
    }
    return strcmp(setting.pszKey, "FREQ_SCALE") == 0 ? 3 : 0;
}

int NumberValue(const HostSetting &setting) {
    const char *pszValue = getenv(setting.pszKey);
    return pszValue != nullptr ? atoi(pszValue) : setting.nDefault;
}

// The screen's texts the settings take over: both rows' labels and values, and the hint line.
struct HostPageTexts {
    Rnd::Text *pLabel, *pValue, *pMarkLabel, *pMark, *pHint;
};

void ShowHostSetting(const HostPageTexts &texts) {
    const HostSetting &setting = kHostSettings[g_nHostSetting];
    char szText[16];
    texts.pLabel->SetText(HxStr(setting.pszLabel));
    if (setting.nStep != 0) {
        snprintf(szText, sizeof(szText), "%d MS", NumberValue(setting));
        texts.pValue->SetText(HxStr(szText));
    } else {
        texts.pValue->SetText(HxStr(setting.apszNames[CurrentValue(setting)]));
    }
    texts.pMarkLabel->SetText(HxStr("APPLIES"));
    texts.pMark->SetText(HxStr(setting.bAtStart ? "NEXT START" : "NEXT SONG"));
    if (texts.pHint != nullptr) {
        texts.pHint->SetText(HxStr(setting.pszHelp));
    }
    HostEvent("setting_row %s start=%d help=%s", setting.pszKey, setting.bAtStart, setting.pszHelp);
}

bool HostSettingsCommand(int nCommand, const HostPageTexts &texts) {
    if (!g_bHostSettings) {
        if (nCommand != kCircleCommand || g_bCalibrating) {
            return false;
        }
        g_bHostSettings = true;
        ShowHostSetting(texts);
        return true;
    }
    if (nCommand == kCircleCommand || nCommand == kMetScreenCommandBack) {
        g_bHostSettings = false;
        return nCommand == kCircleCommand; // Back leaves the screen as well
    }
    const HostSetting &setting = kHostSettings[g_nHostSetting];
    const int nCount = ValueCount(setting);
    switch (nCommand) {
    case kMetScreenCommandPrevious:
        g_nHostSetting = (g_nHostSetting + kHostSettingCount - 1) % kHostSettingCount;
        break;
    case kMetScreenCommandNext:
        g_nHostSetting = (g_nHostSetting + 1) % kHostSettingCount;
        break;
    case kMetScreenCommandLeft:
    case kMetScreenCommandRight: {
        char szNumber[16];
        const char *pszValue = szNumber;
        if (setting.nStep != 0) {
            const int nStep = nCommand == kMetScreenCommandLeft ? -setting.nStep : setting.nStep;
            snprintf(szNumber,
                     sizeof(szNumber),
                     "%d",
                     std::clamp(NumberValue(setting) + nStep,
                                setting.nDefault - setting.nRange,
                                setting.nDefault + setting.nRange));
        } else {
            const int nStep = nCommand == kMetScreenCommandLeft ? nCount - 1 : 1;
            pszValue = setting.apszValues[(CurrentValue(setting) + nStep) % nCount];
        }
        const int bSaved = HostSaveSetting(setting.pszKey, pszValue);
        PlaySoundByName("SND_MET_KEY1");
        HostEvent("setting %s=%s saved=%d", setting.pszKey, pszValue, bSaved);
        break;
    }
    default:
        return true; // the screen's own buttons wait while the settings are up
    }
    ShowHostSetting(texts);
    return true;
}

} // namespace
#endif
#ifndef RESONANCE_HOST
#line 84
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

#ifdef RESONANCE_HOST
    // The calibration hint: a copy of the second row's label, one row further down.
    Rnd::Text *pFirstLabel = dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kAudioLabel)));
    Rnd::Text *pSecondLabel =
        dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kForceFeedbackLabel)));
    if (pFirstLabel != nullptr && pSecondLabel != nullptr &&
        Rnd::TheManager.Find(HxStr(kHintText)) == nullptr) {
        Rnd::Text *pHint = Rnd::Text::sNew(HxStr(kHintText));
        pHint->Copy(pSecondLabel, 0);
        for (int i = 0; i < 3; ++i) {
            pHint->mLocalXfm[3][i] =
                2.0f * pSecondLabel->mLocalXfm[3][i] - pFirstLabel->mLocalXfm[3][i];
        }
        // Smaller than the labels and wrapped at the panel's edge, the width of this many
        // characters at the labels' size: two lines for a setting's help.
        constexpr float kHintScale = 0.8f;
        static const char *const kPanelWidthText = "MOVE UNTIL GEMS MEET";
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                pHint->mLocalXfm[i][j] *= kHintScale;
            }
        }
        pHint->SetWordWrap(1);
        pHint->SetWrapWidth(
            pHint->GetFontWidth(kPanelWidthText, static_cast<int>(strlen(kPanelWidthText))) /
            kHintScale);
        pHint->mDirty = 1;
        pHint->SetShowing(1); // the label it copies is hidden until the screen shows it
        pHint->SetText(HxStr(kHint));
        if (Rnd::Drawable *pDrawParent = pSecondLabel->Drawable::Parent()) {
            pDrawParent->AddDraw(pHint, pSecondLabel);
        }
        if (Rnd::Transformable *pTransParent = pSecondLabel->Transformable::Parent()) {
            pTransParent->AddTrans(pHint);
        }
    }
#endif
#ifndef RESONANCE_HOST
#line 108
#endif
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
    const HostPageTexts texts = {
        dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kAudioLabel))),
        mRows->GetButton(kRowAudio)->mText,
        dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kForceFeedbackLabel))),
        mRows->GetButton(kRowForceFeedback)->mText,
        dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kHintText)))};
    const bool bHostSettings = g_bHostSettings;
    const bool bHandled = HostSettingsCommand(pCommand->mCommand, texts);
    if (bHostSettings && !g_bHostSettings) {
        texts.pLabel->SetText(
            MetConfigText(kMetStrPangameAudioLbl, kPromptConfigCode, kAudioLabelKey));
        texts.pMarkLabel->SetText(
            MetConfigText(kMetStrPangameForceLbl, kPromptConfigCode, kForceFeedbackLabelKey));
        if (texts.pHint != nullptr) {
            texts.pHint->SetText(HxStr(kHint));
        }
        UpdateOptionLabels(); // both rows' own values back
    }
    if (bHandled) {
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
