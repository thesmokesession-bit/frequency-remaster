#include "app/tnlcamerarig.h"

#include <cstring>

#include "math/plane.h"
#include "math/transform.h"
#include "math/transformops.h"
#include "os/formatstring.h"
#include "os/hxstr.h"
#include "rnd/manager.h"
#include "rnd/transanim.h"
#include "rnd/transformable.h"
#include "rnd/view.h"

namespace {

// Slots in each of the two vectors, one per possible player.
constexpr int kSlotCount = 4;

// Frames the ramp takes to cross from one pose to the other.
constexpr float kRampFrames = 480.0f;

constexpr float kPlayPitch = 0.488692194f; // 28 degrees.
constexpr float kPlayFov = 1.39626336f;    // 80 degrees.

// Row of a transform that stores the translation.
constexpr int kXfmRowTranslation = 3;

// Give one camera the blended field of view and screen rectangle and rebuild its projection.
inline void ApplyLens(Rnd::Cam *pCam, float flFov, const Rnd::Cam::Rect &rect) {
    pCam->SetFrustum(pCam->GetNearPlane(), pCam->GetFarPlane(), flFov);
    pCam->mScreenRect = rect;
    pCam->UpdateProjection();
}

// Show or hide the outer cameras and views of players 2 to 4.
inline void ShowOtherPlayers(const std::vector<Rnd::Cam *> &outerCams,
                             const std::vector<Rnd::View *> &localViews,
                             int nShowing) {
    for (int i = 1; i < kSlotCount; ++i) {
        if (outerCams[i] != nullptr) {
            outerCams[i]->SetShowing(nShowing);
        }
        if (localViews[i] != nullptr) {
            localViews[i]->SetShowing(nShowing);
        }
    }
}

} // namespace

#ifdef RESONANCE_HOST
extern "C" int HostPhotoCamera(float *pflYaw, float *pflPitch, float *pflZoom); // host/io/pad.cpp

namespace {

// Photo mode turns "tnl cam1" about a point this far ahead of it.
constexpr float kPhotoPivot = 1.0f;

// Photo mode hides the head-up display and the menus over it.
const char *const kPhotoHidden[] = {"hud.view", "met top view"};
constexpr int kPhotoHiddenCount = sizeof(kPhotoHidden) / sizeof(kPhotoHidden[0]);

// What photo mode changes, as it found it. ponytail: one record, as one rig draws at a time.
struct PhotoSaved {
    bool bOn;
    Transform xfm;
    float afFov[3];
    Rnd::Cam::Rect aRect[3];
    Rnd::Drawable *apHidden[kPhotoHiddenCount];
    int anShowing[kPhotoHiddenCount];
} g_photo;

static_assert(sizeof(Transform) == sizeof(Rnd::Cam::mLocalXfm), "a local transform is 4 rows");

// Drives the cameras while the host's photo mode runs (HostPhotoCamera()). pCam ("tnl cam1") turns
// about a point ahead of it and the three cameras' field of view scales, with the head-up display
// and the menus hidden. When photo mode ends, all of it is put back as it was. Returns whether
// photo mode drives the cameras this frame.
bool PhotoFrame(Rnd::Cam *pCam, Rnd::Cam *pZoomCam, Rnd::Cam *pOuterCam) {
    Rnd::Cam *const apCams[3] = {pCam, pZoomCam, pOuterCam};
    float flYaw;
    float flPitch;
    float flZoom;
    const bool bOn = HostPhotoCamera(&flYaw, &flPitch, &flZoom) != 0;
    if (bOn && !g_photo.bOn) {
        std::memcpy(&g_photo.xfm, apCams[0]->mLocalXfm, sizeof(g_photo.xfm));
        for (int i = 0; i < 3; ++i) {
            g_photo.afFov[i] = apCams[i]->GetFov();
            g_photo.aRect[i] = apCams[i]->mScreenRect;
        }
        for (int i = 0; i < kPhotoHiddenCount; ++i) {
            g_photo.apHidden[i] =
                dynamic_cast<Rnd::Drawable *>(Rnd::TheManager.Find(HxStr(kPhotoHidden[i])));
            if (g_photo.apHidden[i] != nullptr) {
                g_photo.anShowing[i] = g_photo.apHidden[i]->GetShowing();
                g_photo.apHidden[i]->SetShowing(0);
            }
        }
    } else if (!bOn && g_photo.bOn) {
        std::memcpy(apCams[0]->mLocalXfm, &g_photo.xfm, sizeof(g_photo.xfm));
        apCams[0]->mDirty = 1;
        for (int i = 0; i < 3; ++i) {
            ApplyLens(apCams[i], g_photo.afFov[i], g_photo.aRect[i]);
        }
        for (int i = 0; i < kPhotoHiddenCount; ++i) {
            if (g_photo.apHidden[i] != nullptr) {
                g_photo.apHidden[i]->SetShowing(g_photo.anShowing[i]);
            }
        }
    }
    g_photo.bOn = bOn;
    if (!bOn) {
        return false;
    }

    // The basis SetFrame() writes from Rnd::MakeRotMatrix() has no scale. The camera looks along
    // its Y row.
    Vector3 angles;
    Vector3 scale;
    Mat34DecomposeEulerScale(&g_photo.xfm.mBasisX.x, &angles.x, &scale.x);
    angles.x += flPitch;
    angles.z += flYaw;
    Transform xfm = g_photo.xfm;
    Rnd::MakeRotMatrix(&angles.x, &xfm.mBasisX.x);
    xfm.mTranslation.x += (g_photo.xfm.mBasisY.x - xfm.mBasisY.x) * kPhotoPivot;
    xfm.mTranslation.y += (g_photo.xfm.mBasisY.y - xfm.mBasisY.y) * kPhotoPivot;
    xfm.mTranslation.z += (g_photo.xfm.mBasisY.z - xfm.mBasisY.z) * kPhotoPivot;
    std::memcpy(apCams[0]->mLocalXfm, &xfm, sizeof(xfm));
    apCams[0]->mDirty = 1;
    for (int i = 0; i < 3; ++i) {
        ApplyLens(apCams[i], g_photo.afFov[i] * flZoom, g_photo.aRect[i]);
    }
    return true;
}

} // namespace
#endif
#ifndef RESONANCE_HOST
#line 50
#endif

TnlCameraRig::TnlCameraRig(int nPlayerCount, int nSkipIntro)
    : mPlayerCount(nPlayerCount), mState(kStateSettled) {
    mIntroPos.w = 1.0f;
    mPlayPos.w = 1.0f;
    mRamp.SetParams(0.0f, 1.0f, kRampFrames);

    mMainCams[kMainCamNormal] = dynamic_cast<Rnd::Cam *>(Rnd::TheManager.Find(HxStr("tnl cam1")));
    mMainCams[kMainCamZoom] = dynamic_cast<Rnd::Cam *>(Rnd::TheManager.Find(HxStr("tnl cam1z")));
    mOuterCams.resize(kSlotCount, nullptr);
    mLocalViews.resize(kSlotCount, nullptr);
    for (int i = 0; i < nPlayerCount; ++i) {
        mOuterCams[i] = dynamic_cast<Rnd::Cam *>(
            Rnd::TheManager.Find(HxStr(Rnd::MakeString("outer cam%d", i + 1))));
        mLocalViews[i] = dynamic_cast<Rnd::View *>(
            Rnd::TheManager.Find(HxStr(Rnd::MakeString("tnl local%d.view", i + 1))));
    }

    Rnd::TransAnim *pIntro = dynamic_cast<Rnd::TransAnim *>(
        Rnd::TheManager.Find(HxStr(Rnd::MakeString("tnl cam intro%d.tnm", mPlayerCount))));
    Transform xfm;
    Vector3 angles;
    Vector3 scale;
    // The binary also writes the translation's w, which EvalFrame() always overwrites. It
    // overwrites the basis rows' w only when it builds a follow-path basis.
    xfm.mBasisX.w = 1.0f;
    xfm.mBasisY.w = 1.0f;
    xfm.mBasisZ.w = 1.0f;
    angles.w = 1.0f;
    scale.w = 1.0f;
    pIntro->EvalFrame(0.0f, &xfm.mBasisX.x, 1);
    Mat34DecomposeEulerScale(&xfm.mBasisX.x, &angles.x, &scale.x);

    mIntroPitch = angles.x;
    mIntroPos = xfm.mTranslation;
    mIntroRect = mMainCams[kMainCamNormal]->mScreenRect;
    mIntroFov = mMainCams[kMainCamNormal]->GetFov();
    mPlayPitch = kPlayPitch;
    mPlayPos = Vector3{0.0f, -0.5f, 0.0f, 1.0f};
    mPlayRect = Rnd::Cam::Rect{0.0f, 0.1f, 1.0f, 0.8f};
    mPlayFov = kPlayFov;

    if (nSkipIntro != 0) {
        mRamp.Jump(1.0f);
        ZoomIn();
    }
}

void TnlCameraRig::ZoomIn() {
    mRamp.SetTarget(1.0f);
    mState = kStateZoomedIn;
    ShowOtherPlayers(mOuterCams, mLocalViews, 0);
}

void TnlCameraRig::ZoomOut() {
    mRamp.SetTarget(0.0f);
    mState = kStateZoomingOut;
}

void TnlCameraRig::SetFrame(float flTime) {
#ifdef RESONANCE_HOST
    if (PhotoFrame(mMainCams[kMainCamNormal], mMainCams[kMainCamZoom], mOuterCams[0])) {
        return;
    }
#endif
#ifndef RESONANCE_HOST
#line 110
#endif
    if (mRamp.Execute(flTime) == 0) {
        if (mState == kStateSettled) {
            return;
        }
        if (mState == kStateZoomingOut) {
            ShowOtherPlayers(mOuterCams, mLocalViews, 1);
        }
        mState = kStateSettled;
        return;
    }

    const float flPitch = (mPlayPitch - mIntroPitch) * mRamp.Val() + mIntroPitch;
    const float flWeight = mRamp.Val();
    Vector3 pos;
    pos.x = mPlayPos.x * flWeight + mIntroPos.x * (1.0f - flWeight);
    pos.y = mPlayPos.y * flWeight + mIntroPos.y * (1.0f - flWeight);
    pos.z = mPlayPos.z * flWeight + mIntroPos.z * (1.0f - flWeight);
    pos.w = mPlayPos.w;
    Rnd::Cam::Rect rect;
    InterpolateFourFloats(&mIntroRect.x, &mPlayRect.x, &rect.x, mRamp.Val());
    const float flFov = (mPlayFov - mIntroFov) * mRamp.Val() + mIntroFov;

    const Vector3 pitchAngles{flPitch, 0.0f, 0.0f, 1.0f};
    Vector3 basis[3];
    for (Vector3 &row : basis) {
        row.w = 1.0f;
    }
    Rnd::MakeRotMatrix(&pitchAngles.x, &basis[0].x);

    Rnd::Cam *pCam = mMainCams[kMainCamNormal];
    std::memcpy(pCam->mLocalXfm, basis, sizeof(basis));
    pCam->mDirty = 1;
    std::memcpy(pCam->mLocalXfm[kXfmRowTranslation], &pos, sizeof(pos));
    pCam->mDirty = 1;

    for (Rnd::Cam *pCam : mMainCams) {
        ApplyLens(pCam, flFov, rect);
    }
    ApplyLens(mOuterCams[0], flFov, rect);
}
