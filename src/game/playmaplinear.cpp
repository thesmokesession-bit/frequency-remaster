#include "game/playmaplinear.h"

#include <algorithm>
#include <vector>

#include "os/hxstr.h"
#include "script/testregistry.h"

#ifdef RESONANCE_HOST
extern "C" int HostPracticeLoop(int *pFirst, int *pEnd); // host/audio/audio.cpp
extern "C" void HostPracticeLoopBars(int nFirst, int nEnd);
#endif

#ifndef RESONANCE_HOST
#line 8
#endif
namespace {

// The capacity the constructor reserves in the window vectors.
constexpr std::vector<int>::size_type kInitialCapacity = 8;

#ifdef RESONANCE_HOST
// The end GetEndBar() reports while a practice loop repeats: never reached, so the song plays on.
constexpr int kPracticeEndBar = 1 << 19;
#endif
#ifndef RESONANCE_HOST
#line 12
#endif

// The name SelfTest() registers under, and the label it gives every step.
static const char *const kTestName = "PlayMapLinear";
static const char *const kTestLabel = "";
// The steps, sections, and probe positions SelfTest() uses.
const int kTestSteps[] = {1, 3, 6, 10};
const int kTestSections[] = {3, 2, 1, 0, 3, 2, 2};
const int kTestProbes[] = {0, 2, 4, 8, 9};
const int kTestFirstToggle = 2;
const int kTestSecondToggle = 8;
const int kTestLateProbe = 13;
// SelfTest() constructs its map without the script tables.
constexpr int kSkipStepRings = 0;

// Registers the self-test from the unit's static initialiser.
class SelfTestRegistration {
public:
    SelfTestRegistration() {
        TestRegistry::Register(kTestName, PlayMapLinear::RunSelfTest);
    }
};
const SelfTestRegistration sSelfTestRegistration;

} // namespace

PlayMapLinear::PlayMapLinear(int bLoadStepRings)
    : mTrimmedCount(0), mPatternEnd(0), mStepRings(kSetCount) {
    mWindow.reserve(kInitialCapacity);
    mWindowStarts.reserve(kInitialCapacity);
    if (bLoadStepRings != 0) {
        LoadStepRings();
    }
}

void PlayMapLinear::AppendSection(int nSection) {
    mWindowStarts.push_back(GetLength());
    const Entry entry{nSection, 1};
    mWindow.push_back(entry);
}

void PlayMapLinear::GrowPastLimit(int nLimit) {
    while (!(nLimit < GetLength())) {
        // The extent is re-read on every iteration rather than cached, and GetLength() is called
        // once per element rather than once per pass.
        for (std::vector<Entry>::size_type nIndex = 0; nIndex < mPattern.size(); ++nIndex) {
            mWindowStarts.push_back(GetLength());
            const Entry entry{mPattern[nIndex].mSection, 1};
            mWindow.push_back(entry);
        }
    }

    const std::vector<Entry>::size_type nPassed = mPattern.size();
    // The guard compares a byte offset against an element count. That mismatch is what the binary
    // computes, so the trim only fires once the window is more than eight times the source.
    if ((nPassed * sizeof(Entry)) < mWindow.size()) {
        mWindow.erase(mWindow.begin(), mWindow.begin() + nPassed);
        mWindowStarts.erase(mWindowStarts.begin(), mWindowStarts.begin() + nPassed);
        mTrimmedCount += static_cast<int>(nPassed);
    }
}

inline int PlayMapLinear::WindowIndex(int nBar) {
    GrowPastLimit(nBar);
    const std::vector<int>::iterator it =
        std::upper_bound(mWindowStarts.begin(), mWindowStarts.end(), nBar);
    return static_cast<int>(it - mWindowStarts.begin()) - 1;
}

inline void PlayMapLinear::RestartFrom(std::vector<int>::size_type nFirst) {
    for (std::vector<Entry>::size_type nIndex = nFirst; nIndex < mWindow.size(); ++nIndex) {
        const Entry &previous = mWindow[nIndex - 1];
        mWindowStarts[nIndex] =
            mWindowStarts[nIndex - 1] + (mSectionLengths[previous.mSection] * previous.mRepeats);
    }
}

std::vector<int> &PlayMapLinear::FindBarsPlaying(int nStart, int nMin, int nEnd) {
    GrowPastLimit(nEnd);
    mFoundBars.clear();
#ifdef RESONANCE_HOST
    int nLoopFirst, nLoopEnd;
    if (HostPracticeLoop(&nLoopFirst, &nLoopEnd) != 0) {
        for (int nBar = nMin; nBar < nEnd; ++nBar) {
            if (MapBar(nBar) == nStart) {
                mFoundBars.push_back(nBar);
            }
        }
        return mFoundBars;
    }
#endif
#ifndef RESONANCE_HOST
#line 91
#endif
    const int nStep = FindStepIndex(nStart);
    const int nLength = mSectionLengths[nStep];
    const int nOffset = nStart - mSteps[nStep];

    std::vector<int>::iterator start =
        std::upper_bound(mWindowStarts.begin(), mWindowStarts.end(), nMin);
    if (mWindowStarts.begin() < start) {
        --start;
    }
    // Unbounded by the end of the window. GrowPastLimit() has grown it past nEnd.
    for (; *start < nEnd; ++start) {
        const Entry &entry = mWindow[start - mWindowStarts.begin()];
        if (entry.mSection != nStep) {
            continue;
        }
        int nPosition = *start + nOffset;
        for (int nPass = 0; nPass < entry.mRepeats; ++nPass) {
            if (nPosition >= nMin) {
                if (nPosition >= nEnd) {
                    break;
                }
                mFoundBars.push_back(nPosition);
            }
            if (nPosition >= nEnd) {
                break;
            }
            nPosition += nLength;
        }
    }
    return mFoundBars;
}

int PlayMapLinear::EndLoop(int nBar) {
    GrowPastLimit(nBar);
    const int nIndex = WindowIndex(nBar);
    Entry &entry = mWindow[nIndex];
    if (entry.mRepeats != kRepeatForever) {
        return 0;
    }
    entry.mRepeats = ((nBar - mWindowStarts[nIndex]) / mSectionLengths[entry.mSection]) + 1;
    RestartFrom(nIndex + 1);
    return 1;
}

int PlayMapLinear::StartLoop(int nBar) {
    GrowPastLimit(nBar);
    const int nIndex = WindowIndex(nBar);
    mWindow[nIndex].mRepeats = kRepeatForever;
    RestartFrom(nIndex + 1);
    return 1;
}

int PlayMapLinear::SelfTest() {
    PlayMapLinear map(kSkipStepRings);
    for (const int nStep : kTestSteps) {
        map.AddStep(nStep, HxStr(kTestLabel));
    }
    for (const int nSection : kTestSections) {
        map.AppendSection(nSection);
    }
    // Yes, the binary discards every result.
    for (const int nProbe : kTestProbes) {
        map.MapBar(nProbe);
    }
    map.ToggleLoop(kTestFirstToggle);
    map.MapBar(kTestLateProbe);
    map.ToggleLoop(kTestSecondToggle);
    map.MapBar(kTestLateProbe);
    return 1;
}

int PlayMapLinear::RunSelfTest() {
    return SelfTest();
}

PlayMapLinear::~PlayMapLinear() {
}

void PlayMapLinear::RecordPattern() {
    mPattern = mWindow;
    mPatternEnd = GetLength();
}

int PlayMapLinear::GetEndBar() {
#ifdef RESONANCE_HOST
    int nLoopFirst, nLoopEnd;
    if (HostPracticeLoop(&nLoopFirst, &nLoopEnd) != 0) {
        return kPracticeEndBar;
    }
#endif
#ifndef RESONANCE_HOST
#line 175
#endif
    return mPatternEnd;
}

int PlayMapLinear::GetNumSections() const {
    return static_cast<int>(mPattern.size());
}

int PlayMapLinear::GetPatternSection(int nIndex) {
    return mPattern[nIndex].mSection;
}

#ifdef RESONANCE_HOST
// Practice loop: from the loop's end on, time bars play the loop's bars again, so the music, the
// gems and the phrases (all mapped through here) repeat while the song clock runs on. A loop of
// phrases, the entries of the song's recorded pattern, becomes the bars they span, and so does the
// phrase that plays at a bar (the pause menu's Loop Section).
int PlayMapLinear::PracticeBar(int nBar) {
    int nFirst, nEnd;
    int nKind = HostPracticeLoop(&nFirst, &nEnd);
    if (nKind == 3) {
        int nPhrase = 0;
        for (int nPhraseEnd = 0; nPhrase + 1 < static_cast<int>(mPattern.size()); ++nPhrase) {
            nPhraseEnd += mSectionLengths[mPattern[nPhrase].mSection] * mPattern[nPhrase].mRepeats;
            if (nFirst < nPhraseEnd) {
                break;
            }
        }
        nFirst = nPhrase;
        nEnd = nPhrase + 1;
        nKind = 2;
    }
    if (nKind == 2) {
        int nFirstBar = 0;
        int nEndBar = 0;
        for (int i = 0; i < static_cast<int>(mPattern.size()) && i < nEnd; ++i) {
            const int nLength = mSectionLengths[mPattern[i].mSection] * mPattern[i].mRepeats;
            nFirstBar += i < nFirst ? nLength : 0;
            nEndBar += nLength;
        }
        nFirst = nFirstBar;
        nEnd = nEndBar;
        HostPracticeLoopBars(nFirst, nEnd);
    }
    if (nKind == 0 || nEnd <= nFirst || nBar < nEnd) {
        return nBar;
    }
    return nFirst + (nBar - nFirst) % (nEnd - nFirst);
}
#endif

#ifndef RESONANCE_HOST
#line 186
#endif
int PlayMapLinear::MapBar(int nBar) {
#ifdef RESONANCE_HOST
    nBar = PracticeBar(nBar);
#endif
#ifndef RESONANCE_HOST
#line 187
#endif
    GrowPastLimit(nBar);
    const int nIndex = WindowIndex(nBar);
    const int nSection = mWindow[nIndex].mSection;
    return mSteps[nSection] + ((nBar - mWindowStarts[nIndex]) % mSectionLengths[nSection]);
}

int PlayMapLinear::GetLength() const {
    if (mWindowStarts.empty()) {
        return 0;
    }
    const Entry &last = mWindow.back();
    return mWindowStarts.back() + (mSectionLengths[last.mSection] * last.mRepeats);
}

int PlayMapLinear::MapToLinkedStep(int nPosition, int nSet) {
    const int nStep = FindStepIndex(nPosition);
    const std::vector<StepPair> &pairs = mStepRings[nSet];
    for (std::vector<StepPair>::const_iterator it = pairs.begin(); it != pairs.end(); ++it) {
        if (it->mStep == nStep) {
            return (mSteps[it->mPartner] - mSteps[nStep]) + nPosition;
        }
    }
    return nPosition;
}

int PlayMapLinear::GetPatternIndex(int nBar) {
#ifdef RESONANCE_HOST
    nBar = PracticeBar(nBar); // a practice loop's section, and its sound banks (Gamer::OnBar())
#endif
#ifndef RESONANCE_HOST
#line 213
#endif
    GrowPastLimit(nBar);
    return static_cast<int>(WindowIndex(nBar) % mPattern.size());
}

int PlayMapLinear::GetAbsoluteSectionIndex(int nBar) {
#ifdef RESONANCE_HOST
    nBar = PracticeBar(nBar); // a practice loop's section, and its sound banks (Gamer::OnBar())
#endif
#ifndef RESONANCE_HOST
#line 218
#endif
    GrowPastLimit(nBar);
    return WindowIndex(nBar) + mTrimmedCount;
}

int PlayMapLinear::IsLooping(int nBar) {
    GrowPastLimit(nBar);
    return mWindow[WindowIndex(nBar)].mRepeats == kRepeatForever;
}

int PlayMapLinear::ToggleLoop(int nBar) {
    if (EndLoop(nBar) != 0) {
        return 1;
    }
    StartLoop(nBar); // Yes, the binary discards this call's result.
    return 0;
}
