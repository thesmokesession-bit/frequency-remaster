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
#include "rnd/mesh.h"
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
    Application::shared()->GetWatchdogTimer()->PostIn(
        pCommand, Sch::Time{nInNs}, g_clickCommand, 0);
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
        snprintf(
            szText, sizeof(szText), "TAP %d", kCalibrationTaps - static_cast<int>(g_lags.size()));
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

// The remaster's settings page, Circle on this screen, lists every setting under section
// headings, ten lines at a time, on the screen's panel stretched to fit. Up and Down pick a
// setting, L1 and R1 jump to the previous or next section, and Left and Right change it and save
// it in the settings file at once (HostSaveSetting). Cross runs an action row's function. Circle
// or Triangle closes the page. Under the list are the setting's help, when it takes effect (a
// setting read at start-up from the next start, the others from the next song), and the
// position in the list. The first value of a list is the one an unset key has. A row with a step
// is a number of ms within plus or minus its range of its default.
constexpr int kCircleCommand = 7; // MetCommandMap's code for Circle
constexpr int kL1Command = 11;
constexpr int kR1Command = 13;

static const char *const kHint = "SQUARE: CALIBRATE\nCIRCLE: SETTINGS";

struct HostSetting {
    const char *pszKey, *pszLabel;
    bool bAtStart;
    const char *apszValues[7], *apszNames[7];
    const char *pszHelp;
    int nRange = 0, nStep = 0, nDefault = 0;
    // An action row has no values, and Cross runs pfnAction. Its value shows apszNames[0] until
    // then, and after it apszNames[1] for a return of 0 or apszNames[2] for any other.
    int (*pfnAction)(void) = nullptr;
};

#ifdef __PROSPERO__
static const char *const kHdFolder = "/app0/hd";
#else
static const char *const kHdFolder = "hd";
#endif

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
    {"FREQ_FILTER", "FILTER", false, {"off", "bloom", "crt"}, {"OFF", "BLOOM", "CRT"},
     "A GLOW ON THE NEON, OR TV SCANLINES"},
    {"FREQ_GHOST", "SCORE GHOST", false, {"0", "1"}, {"OFF", "ON"},
     "YOUR BEST RUN SCORE, UNDER YOUR SCORE"},
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
    {"FREQ_TEX_REPLACE", "HD TEXTURES", true, {"", kHdFolder}, {"OFF", "ON"},
     "DRAWS THE HD TEXTURES IN THE HD FOLDER"},
    {"FREQ_UPDATE", "AUTO UPDATE", true, {"1", "0"}, {"ON", "OFF"},
     "ON THE PS5, EACH START FETCHES A NEW BUILD"},
};
// clang-format on
constexpr int kHostSettingCount = sizeof(kHostSettings) / sizeof(kHostSettings[0]);

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

// The page's sections in order, each a heading and then its keys. A table row missing here is
// listed under kOtherHeading at the end.
// clang-format off
const char *const kSectionKeys[] = {
    "DISPLAY", "FREQ_SCALE", "FREQ_FPS", "FREQ_WIDESCREEN", "FREQ_HUD_EDGES", "FREQ_SCALE_EFFECTS",
    "FREQ_TEX_FILTER", "FREQ_TEX_REPLACE", "FREQ_COLOR_DEPTH", "FREQ_AA", "FREQ_TRAILS",
    "FREQ_FILTER",
    "SOUND", "FREQ_SURROUND", "FREQ_SPATIAL", "FREQ_SPU_INTERP",
    "TIMING", "FREQ_AUDIO_OFFSET_MS", "FREQ_VIDEO_OFFSET_MS", "FREQ_INPUT_OFFSET_MS",
    "CONTROLLER", "FREQ_AUTOPAUSE", "FREQ_LIGHTBAR", "FREQ_TRIGGERS", "FREQ_RUMBLE_LEAD_MS",
    "GAMEPLAY", "FREQ_UNLOCK_ALL", "FREQ_BONUS_SONGS", "FREQ_GHOST", "FREQ_WATCH", "FREQ_PRACTICE",
    "FREQ_PRACTICE_SPEED",
    "ACCESSIBILITY", "FREQ_NO_FAIL", "FREQ_TIMING_WINDOW", "FREQ_REDUCE_FLASH", "FREQ_GEM_PALETTE",
    "SYSTEM", "FREQ_FAST_BOOT", "FREQ_UPDATE",
};
// clang-format on
static const char *const kOtherHeading = "OTHER";
constexpr int kPageLines = 10;

// One line of the page, a setting or a section heading (nSetting negative).
struct PageLine {
    int nSetting;
    const char *pszHeading;
};

std::vector<PageLine> g_lines;
bool g_bHostSettings = false;
int g_nLine = 1; // the selected line, always a setting
int g_nTop = 0;  // the first line on the screen
int g_nActionSetting = -1;
int g_nActionResult = 0;

void BuildLines() {
    if (!g_lines.empty()) {
        return;
    }
    std::vector<bool> placed(kHostSettingCount, false);
    for (const char *pszKey : kSectionKeys) {
        if (strncmp(pszKey, "FREQ_", 5) != 0) {
            g_lines.push_back({-1, pszKey});
        }
        for (int i = 0; i < kHostSettingCount; ++i) {
            if (!placed[i] && strcmp(kHostSettings[i].pszKey, pszKey) == 0) {
                placed[i] = true;
                g_lines.push_back({i, nullptr});
            }
        }
    }
    for (int i = 0; i < kHostSettingCount; ++i) {
        if (!placed[i]) {
            if (g_lines.back().pszHeading != kOtherHeading) {
                g_lines.push_back({-1, kOtherHeading});
            }
            g_lines.push_back({i, nullptr});
        }
    }
}

int LineCount() {
    return static_cast<int>(g_lines.size());
}

// The next setting's line from nLine in the direction nStep, round the end of the list.
int StepRow(int nLine, int nStep) {
    do {
        nLine = (nLine + nStep + LineCount()) % LineCount();
    } while (g_lines[nLine].nSetting < 0);
    return nLine;
}

// The heading of the section nStep sections from the one with nLine.
int StepSection(int nLine, int nStep) {
    while (g_lines[nLine].nSetting >= 0) {
        --nLine; // the first line is a heading
    }
    do {
        nLine = (nLine + nStep + LineCount()) % LineCount();
    } while (g_lines[nLine].nSetting >= 0);
    return nLine;
}

// Scroll the least that shows the selection with a line either side of it.
void ScrollToSelection() {
    g_nTop = std::clamp(g_nTop, g_nLine + 2 - kPageLines, g_nLine - 1);
    g_nTop = std::clamp(g_nTop, 0, std::max(LineCount() - kPageLines, 0));
}

void SelectSetting(const char *pszKey) {
    for (int nLine = 0; nLine < LineCount(); ++nLine) {
        if (g_lines[nLine].nSetting >= 0 &&
            strcmp(kHostSettings[g_lines[nLine].nSetting].pszKey, pszKey) == 0) {
            g_nLine = nLine;
        }
    }
    ScrollToSelection();
}

HxStr ValueText(int nSetting) {
    const HostSetting &setting = kHostSettings[nSetting];
    if (setting.pfnAction != nullptr) {
        const char *pszStatus = setting.apszNames[nSetting != g_nActionSetting ? 0 :
                                                  g_nActionResult == 0         ? 1 :
                                                                                 2];
        return HxStr(pszStatus != nullptr ? pszStatus : "");
    }
    if (setting.nStep != 0) {
        char szText[16];
        snprintf(szText, sizeof(szText), "%d MS", NumberValue(setting));
        return HxStr(szText);
    }
    return HxStr(setting.apszNames[CurrentValue(setting)]);
}

// The layout, in the panel mesh's units (about one PS2 pixel) from its origin, x to the right and
// z up. The panel grows by kGrow* at its edges; its corner pieces move with them unscaled.
constexpr float kGrowLeft = 130.0f;
constexpr float kGrowUp = 64.0f;
constexpr float kGrowDown = 86.0f;
constexpr float kLeftX = -148.0f;
constexpr float kRightX = 312.0f;
constexpr float kPillRightX = 296.0f; // clear of the scroll arrows and the panel's corners
constexpr float kFirstLineZ = 106.0f;
constexpr float kLinePitch = 26.0f;
// A row's pill (its origin is off its centre), its arrows, and its value, at kRowScale of the
// screen's row; the scroll arrows sit right of the pills.
constexpr float kRowScale = 0.8f;
constexpr float kPillX = 228.0f;
constexpr float kLeftArrowX = 111.0f;
constexpr float kRightArrowX = 286.0f;
constexpr float kValueX = 201.0f;
constexpr float kScrollX = 309.0f;
constexpr float kScrollScale = 0.6f;
constexpr float kHeadingGap = 8.0f;
// Headings take the screen title's font, as the title screen draws it over every screen.
static const char *const kTitleText = "fst_title.txt";
// Below the list are a rule, the help lines, and the footer (when it applies, the position).
constexpr float kSeparatorZ = -146.0f;
constexpr float kHelpZ = -156.0f;
constexpr float kFooterZ = -200.0f;
constexpr float kSmallScale = 0.7f;
constexpr int kSelectedState = 1; // a Rnd::Button state, as MetButtonList sets it

// The screen's objects the page uses. The first row's pill, arrows, and value mark the selected
// setting, the page copies the second row's for the others, and the stock rows are hidden while
// the page is up.
static const char *const kPanelMesh = "pan_game_options.mesh";
static const char *const kPillMesh = "nop_but_06_01.mesh";
static const char *const kLeftArrowMesh = "nop_arr_left_01.mesh";
static const char *const kRightArrowMesh = "nop_arr_right_01.mesh";
static const char *const kValueText = "nop_01.txt";
static const char *const kRuleMesh = "nop_hr2.mesh";
static const char *const kOtherPill = "nop_but_06_02.mesh";
static const char *const kOtherLeftArrow = "nop_arr_left_02.mesh";
static const char *const kOtherRightArrow = "nop_arr_right_02.mesh";
static const char *const kOtherValue = "nop_02.txt";
static const char *const kHiddenObjects[] = {kAudioLabel,
                                             kForceFeedbackLabel,
                                             kOtherValue,
                                             kOtherPill,
                                             kOtherLeftArrow,
                                             kOtherRightArrow,
                                             "nop_but_06stem_01.mesh",
                                             "nop_but_06stem_02.mesh",
                                             "nop_hr1.mesh",
                                             kRuleMesh,
                                             kHintText};

template <class T>
T *FindAs(const char *pszName) {
    return dynamic_cast<T *>(Rnd::TheManager.Find(HxStr(pszName)));
}

void Show(const char *pszName, int nShowing) {
    if (Rnd::Drawable *pDrawable = FindAs<Rnd::Drawable>(pszName)) {
        pDrawable->SetShowing(nShowing);
    }
}

// The page's copy of the screen's object pszSource, made once as pszFormat with nIndex in it,
// drawn just before pszSource.
template <class T>
T *PageObject(const char *pszSource, const char *pszFormat, int nIndex) {
    char szName[48];
    snprintf(szName, sizeof(szName), pszFormat, nIndex);
    if (T *pObject = FindAs<T>(szName)) {
        return pObject;
    }
    T *pSource = FindAs<T>(pszSource);
    T *pObject = dynamic_cast<T *>(Rnd::TheManager.Create(pSource->ClassName(), HxStr(szName)));
    pObject->Copy(pSource, 0);
    pObject->SetShowing(0);
    if (Rnd::Drawable *pDrawParent = pSource->Drawable::Parent()) {
        pDrawParent->AddDraw(pObject, pSource);
    }
    if (Rnd::Transformable *pTransParent = pSource->Transformable::Parent()) {
        pTransParent->AddTrans(pObject);
    }
    return pObject;
}

struct PageSlot {
    Rnd::Text *pLabel, *pHeading, *pValue;
    Rnd::Mesh *pPill, *pLeftArrow, *pRightArrow, *pRule;
};

PageSlot FindSlot(int nSlot) {
    return {PageObject<Rnd::Text>(kAudioLabel, "host_page_label_%d", nSlot),
            PageObject<Rnd::Text>(kAudioLabel, "host_page_heading_%d", nSlot),
            PageObject<Rnd::Text>(kOtherValue, "host_page_value_%d", nSlot),
            PageObject<Rnd::Mesh>(kOtherPill, "host_page_pill_%d", nSlot),
            PageObject<Rnd::Mesh>(kOtherLeftArrow, "host_page_left_%d", nSlot),
            PageObject<Rnd::Mesh>(kOtherRightArrow, "host_page_right_%d", nSlot),
            PageObject<Rnd::Mesh>(kRuleMesh, "host_page_rule_%d", nSlot)};
}

// A screen object's transform as the page opened, and its place in panel units then.
struct Anchor {
    float afLocal[4][4];
    float flX, flZ;
};

enum AnchorId { kAnchorLabel, kAnchorValue, kAnchorPill, kAnchorLeft, kAnchorRight, kAnchorRule };
const char *const kAnchorNames[] = {
    kAudioLabel, kValueText, kPillMesh, kLeftArrowMesh, kRightArrowMesh, kRuleMesh};
Anchor g_anchors[std::size(kAnchorNames)];
std::vector<Rnd::MeshVert> g_panelVerts; // the panel's own shape while the page stretches it
float g_flRuleMinX = 0.0f;
float g_flRuleWidth = 1.0f;

// Put pObject at (flX, flZ) in panel units, flScale times the size of the anchor's object.
void Place(Rnd::Transformable *pObject, AnchorId nAnchor, float flX, float flZ, float flScale) {
    const Anchor &anchor = g_anchors[nAnchor];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            pObject->mLocalXfm[i][j] = anchor.afLocal[i][j] * flScale;
        }
    }
    pObject->mLocalXfm[3][0] = anchor.afLocal[3][0] + flX - anchor.flX;
    pObject->mLocalXfm[3][1] = anchor.afLocal[3][1];
    pObject->mLocalXfm[3][2] = anchor.afLocal[3][2] + flZ - anchor.flZ;
    pObject->mDirty = 1;
}

// A copy of the screen's rule from flFromX to flToX.
void PlaceRule(Rnd::Mesh *pRule, float flFromX, float flToX, float flZ) {
    const float flStretch = (flToX - flFromX) / g_flRuleWidth;
    Place(pRule, kAnchorRule, flFromX - g_flRuleMinX * flStretch, flZ, 1.0f);
    for (int j = 0; j < 3; ++j) {
        pRule->mLocalXfm[0][j] *= flStretch;
    }
}

// Turn a left arrow a quarter to point up (nUp 1) or down (-1).
void PlaceScrollArrow(Rnd::Mesh *pArrow, float flZ, int nUp) {
    Place(pArrow, kAnchorLeft, kScrollX, flZ, kScrollScale);
    for (int i = 0; i < 2; ++i) {
        const float flX = pArrow->mLocalXfm[i][0];
        pArrow->mLocalXfm[i][0] = nUp * pArrow->mLocalXfm[i][2];
        pArrow->mLocalXfm[i][2] = -nUp * flX;
    }
}

void ShowPage() {
    Rnd::Font *pLabelFont = FindAs<Rnd::Text>(kAudioLabel)->GetFont();
    Rnd::Font *pSelectedFont = FindAs<Rnd::Button>(kAudioButton)->mFonts[kSelectedState];
    for (int nSlot = 0; nSlot < kPageLines; ++nSlot) {
        const PageSlot slot = FindSlot(nSlot);
        const int nLine = g_nTop + nSlot;
        const float flZ = kFirstLineZ - kLinePitch * nSlot;
        const bool bHeading = nLine < LineCount() && g_lines[nLine].nSetting < 0;
        const bool bRow = nLine < LineCount() && !bHeading;
        const bool bOther = bRow && nLine != g_nLine;
        slot.pHeading->SetShowing(bHeading);
        slot.pRule->SetShowing(bHeading);
        slot.pLabel->SetShowing(bRow);
        slot.pValue->SetShowing(bOther);
        slot.pPill->SetShowing(bOther);
        slot.pLeftArrow->SetShowing(bOther);
        slot.pRightArrow->SetShowing(bOther);
        if (bHeading) {
            const char *pszHeading = g_lines[nLine].pszHeading;
            slot.pHeading->SetText(HxStr(pszHeading));
            Place(slot.pHeading, kAnchorLabel, kLeftX, flZ, kRowScale);
            const float flWidth =
                slot.pHeading->GetFontWidth(pszHeading, static_cast<int>(strlen(pszHeading)));
            PlaceRule(slot.pRule, kLeftX + flWidth * kRowScale + kHeadingGap, kPillRightX, flZ);
        }
        if (!bRow) {
            continue;
        }
        const int nSetting = g_lines[nLine].nSetting;
        slot.pLabel->SetFont(bOther ? pLabelFont : pSelectedFont);
        slot.pLabel->SetText(HxStr(kHostSettings[nSetting].pszLabel));
        Place(slot.pLabel, kAnchorLabel, kLeftX, flZ, kRowScale);
        Rnd::Text *pValue = bOther ? slot.pValue : FindAs<Rnd::Text>(kValueText);
        pValue->SetText(ValueText(nSetting));
        Place(pValue, kAnchorValue, kValueX, flZ, kRowScale);
        Place(bOther ? slot.pPill : FindAs<Rnd::Mesh>(kPillMesh),
              kAnchorPill,
              kPillX,
              flZ,
              kRowScale);
        Place(bOther ? slot.pLeftArrow : FindAs<Rnd::Mesh>(kLeftArrowMesh),
              kAnchorLeft,
              kLeftArrowX,
              flZ,
              kRowScale);
        Place(bOther ? slot.pRightArrow : FindAs<Rnd::Mesh>(kRightArrowMesh),
              kAnchorRight,
              kRightArrowX,
              flZ,
              kRowScale);
    }

    Rnd::Mesh *pUp = PageObject<Rnd::Mesh>(kOtherLeftArrow, "host_page_up", 0);
    Rnd::Mesh *pDown = PageObject<Rnd::Mesh>(kOtherLeftArrow, "host_page_down", 0);
    pUp->SetShowing(g_nTop > 0);
    pDown->SetShowing(g_nTop + kPageLines < LineCount());
    PlaceScrollArrow(pUp, kFirstLineZ, 1);
    PlaceScrollArrow(pDown, kFirstLineZ - kLinePitch * (kPageLines - 1), -1);

    const HostSetting &setting = kHostSettings[g_lines[g_nLine].nSetting];
    Rnd::Text *pHelp = PageObject<Rnd::Text>(kAudioLabel, "host_page_help", 0);
    pHelp->SetText(HxStr(setting.pszHelp));
    Rnd::Text *pApplies = PageObject<Rnd::Text>(kAudioLabel, "host_page_applies", 0);
    pApplies->SetText(HxStr(setting.pfnAction != nullptr ? "APPLIES  NOW" :
                            setting.bAtStart             ? "APPLIES  NEXT START" :
                                                           "APPLIES  NEXT SONG"));
    int nRow = 0;
    for (int nLine = 0; nLine <= g_nLine; ++nLine) {
        nRow += g_lines[nLine].nSetting >= 0;
    }
    char szPosition[32];
    snprintf(szPosition, sizeof(szPosition), "L1 R1 SECTION   %d / %d", nRow, kHostSettingCount);
    PageObject<Rnd::Text>(kAudioLabel, "host_page_position", 0)->SetText(HxStr(szPosition));
    HostEvent("setting_row %s start=%d top=%d help=%s",
              setting.pszKey,
              setting.bAtStart,
              g_nTop,
              setting.pszHelp);
}

void OpenPage() {
    BuildLines();
    g_nActionSetting = -1;
    Rnd::Mesh *pPanel = FindAs<Rnd::Mesh>(kPanelMesh);
    for (size_t i = 0; i < std::size(kAnchorNames); ++i) {
        const Rnd::Transformable *pObject = FindAs<Rnd::Transformable>(kAnchorNames[i]);
        memcpy(g_anchors[i].afLocal, pObject->mLocalXfm, sizeof(g_anchors[i].afLocal));
        g_anchors[i].flX = pObject->mWorldXfm[3][0] - pPanel->mWorldXfm[3][0];
        g_anchors[i].flZ = pObject->mWorldXfm[3][2] - pPanel->mWorldXfm[3][2];
    }
    const std::vector<Rnd::MeshVert> &rule = FindAs<Rnd::Mesh>(kRuleMesh)->mVertsOwner->mVerts;
    const auto [pMin, pMax] = std::minmax_element(
        rule.begin(), rule.end(), [](const Rnd::MeshVert &a, const Rnd::MeshVert &b) {
            return a.mPoint.x < b.mPoint.x;
        });
    g_flRuleMinX = pMin->mPoint.x;
    g_flRuleWidth = pMax->mPoint.x - pMin->mPoint.x;

    // Each panel vertex left of the middle moves left, and each above or below it up or down.
    std::vector<Rnd::MeshVert> &verts = pPanel->mVertsOwner->mVerts;
    g_panelVerts = verts;
    float flMinX = verts[0].mPoint.x, flMaxX = flMinX, flMinY = verts[0].mPoint.y, flMaxY = flMinY;
    for (const Rnd::MeshVert &vert : verts) {
        flMinX = std::min(flMinX, vert.mPoint.x);
        flMaxX = std::max(flMaxX, vert.mPoint.x);
        flMinY = std::min(flMinY, vert.mPoint.y);
        flMaxY = std::max(flMaxY, vert.mPoint.y);
    }
    const float flUp = pPanel->mWorldXfm[1][2] < 0.0f ? -1.0f : 1.0f; // the mesh's y on the screen
    for (Rnd::MeshVert &vert : verts) {
        vert.mPoint.x -= vert.mPoint.x < (flMinX + flMaxX) / 2 ? kGrowLeft : 0.0f;
        vert.mPoint.y += (vert.mPoint.y - (flMinY + flMaxY) / 2) * flUp > 0.0f ? flUp * kGrowUp :
                                                                                 -flUp * kGrowDown;
    }
    pPanel->SyncChanged(Rnd::Mesh::kSyncPoints);

    for (const char *pszName : kHiddenObjects) {
        Show(pszName, 0);
    }
    for (int nSlot = 0; nSlot < kPageLines; ++nSlot) {
        const PageSlot slot = FindSlot(nSlot);
        slot.pLabel->SetAlign(Rnd::Text::kTextAlignLeft | Rnd::Text::kTextAlignMiddle);
        slot.pHeading->SetAlign(Rnd::Text::kTextAlignLeft | Rnd::Text::kTextAlignMiddle);
        if (Rnd::Text *pTitle = FindAs<Rnd::Text>(kTitleText)) {
            slot.pHeading->SetFont(pTitle->GetFont());
        }
    }
    Rnd::Text *pHelp = PageObject<Rnd::Text>(kAudioLabel, "host_page_help", 0);
    Rnd::Text *pApplies = PageObject<Rnd::Text>(kAudioLabel, "host_page_applies", 0);
    Rnd::Text *pPosition = PageObject<Rnd::Text>(kAudioLabel, "host_page_position", 0);
    Rnd::Mesh *pSeparator = PageObject<Rnd::Mesh>(kRuleMesh, "host_page_separator", 0);
    pHelp->SetShowing(1);
    pApplies->SetShowing(1);
    pPosition->SetShowing(1);
    pSeparator->SetShowing(1);
    pHelp->SetAlign(Rnd::Text::kTextAlignLeft | Rnd::Text::kTextAlignTop);
    pHelp->SetWordWrap(1);
    pHelp->SetWrapWidth((kRightX - kLeftX) / kSmallScale);
    pApplies->SetAlign(Rnd::Text::kTextAlignLeft | Rnd::Text::kTextAlignMiddle);
    pPosition->SetAlign(Rnd::Text::kTextAlignRight | Rnd::Text::kTextAlignMiddle);
    Place(pHelp, kAnchorLabel, kLeftX, kHelpZ, kSmallScale);
    Place(pApplies, kAnchorLabel, kLeftX, kFooterZ, kSmallScale);
    Place(pPosition, kAnchorLabel, kPillRightX, kFooterZ, kSmallScale);
    PlaceRule(pSeparator, kLeftX, kRightX, kSeparatorZ);
    ScrollToSelection();
    ShowPage();
    HostEvent("setting_page open lines=%d rows=%d", LineCount(), kHostSettingCount);
}

void ClosePage() {
    for (int nSlot = 0; nSlot < kPageLines; ++nSlot) {
        const PageSlot slot = FindSlot(nSlot);
        slot.pLabel->SetShowing(0);
        slot.pHeading->SetShowing(0);
        slot.pValue->SetShowing(0);
        slot.pPill->SetShowing(0);
        slot.pLeftArrow->SetShowing(0);
        slot.pRightArrow->SetShowing(0);
        slot.pRule->SetShowing(0);
    }
    for (const char *pszName : {"host_page_up",
                                "host_page_down",
                                "host_page_help",
                                "host_page_applies",
                                "host_page_position",
                                "host_page_separator"}) {
        Show(pszName, 0);
    }
    Rnd::Mesh *pPanel = FindAs<Rnd::Mesh>(kPanelMesh);
    pPanel->mVertsOwner->mVerts = g_panelVerts;
    pPanel->SyncChanged(Rnd::Mesh::kSyncPoints);
    for (AnchorId nAnchor : {kAnchorValue, kAnchorPill, kAnchorLeft, kAnchorRight}) {
        Rnd::Transformable *pObject = FindAs<Rnd::Transformable>(kAnchorNames[nAnchor]);
        memcpy(pObject->mLocalXfm, g_anchors[nAnchor].afLocal, sizeof(pObject->mLocalXfm));
        pObject->mDirty = 1;
    }
    for (const char *pszName : kHiddenObjects) {
        Show(pszName, 1);
    }
    HostEvent("setting_page close");
}

bool HostSettingsCommand(int nCommand) {
    if (!g_bHostSettings) {
        if (nCommand != kCircleCommand || g_bCalibrating) {
            return false;
        }
        g_bHostSettings = true;
        OpenPage();
        return true;
    }
    if (nCommand == kCircleCommand || nCommand == kMetScreenCommandBack) {
        g_bHostSettings = false;
        ClosePage();
        return nCommand == kCircleCommand; // Back closes the screen as well
    }
    const int nSetting = g_lines[g_nLine].nSetting;
    const HostSetting &setting = kHostSettings[nSetting];
    const int nCount = ValueCount(setting);
    switch (nCommand) {
    case kMetScreenCommandPrevious:
        g_nLine = StepRow(g_nLine, -1);
        break;
    case kMetScreenCommandNext:
        g_nLine = StepRow(g_nLine, 1);
        break;
    case kL1Command:
    case kR1Command: {
        g_nTop = StepSection(g_nLine, nCommand == kL1Command ? -1 : 1);
        g_nLine = StepRow(g_nTop, 1);
        PlaySoundByName("SND_MET_HIGH"); // as Up and Down sound
        HostEvent("setting_section %s", g_lines[g_nTop].pszHeading);
        break;
    }
    case kMetScreenCommandSelect:
        if (setting.pfnAction == nullptr) {
            return true;
        }
        g_nActionResult = setting.pfnAction();
        g_nActionSetting = nSetting;
        HostEvent("setting_action %s result=%d", setting.pszKey, g_nActionResult);
        break;
    case kMetScreenCommandLeft:
    case kMetScreenCommandRight: {
        if (setting.pfnAction != nullptr) {
            return true;
        }
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
        HostEvent("setting %s=%s saved=%d", setting.pszKey, pszValue, bSaved);
        break;
    }
    default:
        return true; // the screen's own buttons wait while the settings are up
    }
    ScrollToSelection();
    ShowPage();
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
    const int nCommand = pCommand->mCommand;
    Rnd::Text *pCalibrationLabel =
        dynamic_cast<Rnd::Text *>(Rnd::TheManager.Find(HxStr(kAudioLabel)));
    HxStr calibrationLabel =
        MetConfigText(kMetStrPangameAudioLbl, kPromptConfigCode, kAudioLabelKey);
    if (g_bHostSettings) {
        // On the page, Square measures the input offset on its row.
        if (nCommand == kSquareCommand && !g_bCalibrating) {
            SelectSetting("FREQ_INPUT_OFFSET_MS");
            ShowPage();
        }
        pCalibrationLabel = FindSlot(g_nLine - g_nTop).pLabel;
        calibrationLabel = HxStr(kHostSettings[g_lines[g_nLine].nSetting].pszLabel);
    }
    if (CalibrationCommand(
            nCommand, pCalibrationLabel, mRows->GetButton(kRowAudio)->mText, calibrationLabel)) {
        return;
    }
    const bool bWasOpen = g_bHostSettings;
    if (!bWasOpen && nCommand == kCircleCommand && !g_bCalibrating) {
        // The first row's pill marks the selected setting, and the page copies the second's.
        mRows->SetSelected(kRowAudio);
    }
    const bool bHandled = HostSettingsCommand(nCommand);
    if (!bWasOpen && g_bHostSettings) {
        MetHelpScreen::SetText(HxStr(""), mRenderer->mAnimationFrame);
    } else if (bWasOpen && !g_bHostSettings) {
        UpdateOptionLabels();
        MetHelpScreen::SetText(mHelpKeys[mRows->mSelected], mRenderer->mAnimationFrame);
    } else if (bHandled &&
               (nCommand == kMetScreenCommandLeft || nCommand == kMetScreenCommandRight)) {
        StartRepeatingSound(mRenderer->mAnimationFrame,
                            kArrowFlashInterval,
                            (nCommand == kMetScreenCommandLeft ? mLeftArrows : mRightArrows)[0],
                            kArrowFlashCycles);
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
