#include "app/hudscore.h"

#include "app/application.h"
#include "app/overlay.h"
#include "game/gamemanagerimpl.h"
#include "game/player.h"
#include "os/formatstring.h"
#include "os/hxstr.h"
#include "rnd/font.h"
#include "rnd/manager.h"
#include "rnd/mat.h"
#include "rnd/mesh.h"
#include "rnd/text.h"

#ifdef RESONANCE_HOST
extern "C" int HostGhostAt(int nScore); // host/core/progress.cpp
#endif
#ifndef RESONANCE_HOST
#line 13
#endif

namespace {

// Pending time that marks the readout as up to date.
constexpr float kScoreDrawn = 1.0e9f;

// Pending time that marks a change whose arrival is not yet recorded.
constexpr float kScoreChangeUntimed = -1.0f;

// Time a change settles for before the readout redraws.
constexpr float kRedrawDelay = 600.0f;

// Pending time the constructor starts with, far enough back that the first Update() redraws.
constexpr float kScoreChangeLongAgo = -100000.0f;

} // namespace

HudScore::HudScore(Player *pPlayer, int nIndex) : mText(nullptr) {
    const char *pszLayout =
        g_hudLayoutName.mStr != nullptr ? g_hudLayoutName.mStr : g_szEmptyString;
    mMesh = dynamic_cast<Rnd::Mesh *>(
        Rnd::TheManager.Find(HxStr(Rnd::MakeString("%s score%d.mesh", pszLayout, nIndex))));

    pszLayout = g_hudLayoutName.mStr != nullptr ? g_hudLayoutName.mStr : g_szEmptyString;
    mText = dynamic_cast<Rnd::Text *>(
        Rnd::TheManager.Find(HxStr(Rnd::MakeString("%s score%d.txt", pszLayout, nIndex))));

    pszLayout = g_hudLayoutName.mStr != nullptr ? g_hudLayoutName.mStr : g_szEmptyString;
    Rnd::Font *pFont = dynamic_cast<Rnd::Font *>(
        Rnd::TheManager.Find(HxStr(Rnd::MakeString("%s score%d.font", pszLayout, nIndex))));

    Rnd::Mat *pMat = dynamic_cast<Rnd::Mat *>(
        Rnd::TheManager.Find(HxStr("HUD score font ") + HxStr(pPlayer->mColorName) + ".mat"));
    pFont->SetMat(pMat);

    mMesh->SetShowing(Application::shared()->GetPlayMode() == kPlayModeGame);
    mScore = 0;
    mChangeTime = kScoreChangeLongAgo;
}

void HudScore::Update(float flTime) {
#ifdef RESONANCE_HOST
    // FREQ_GHOST=1: the kept best run's score at this song tick, under the live score, which still
    // redraws only once its change has settled. Solo games only, so one readout.
    const int nGhost = HostGhostAt(mScore);
    if (nGhost >= 0) {
        static int s_nShownScore = 0;
        static int s_nShownGhost = -1;
        if (mChangeTime == kScoreChangeUntimed) {
            mChangeTime = flTime;
        }
        if (mChangeTime != kScoreDrawn && flTime - mChangeTime > kRedrawDelay) {
            s_nShownScore = mScore;
            s_nShownGhost = -1;
            mChangeTime = kScoreDrawn;
        }
        if (nGhost != s_nShownGhost) {
            s_nShownGhost = nGhost;
            mText->SetText(HxStr(Rnd::MakeString("%d\n%d", s_nShownScore, nGhost)));
        }
        return;
    }
#endif
#ifndef RESONANCE_HOST
#line 54
#endif
    if (mChangeTime == kScoreDrawn) {
        return;
    }

    if (mChangeTime == kScoreChangeUntimed) {
        mChangeTime = flTime;
    }

    if (flTime - mChangeTime > kRedrawDelay) {
        mText->SetText(HxStr(Rnd::MakeString("%d", mScore)));
        mChangeTime = kScoreDrawn;
    }
}
