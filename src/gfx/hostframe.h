#pragma once

// The host's frame pacing (host/gfx-null/gfx.cpp). With FREQ_FPS=120 the frame loop runs twice a
// field, so the frame code that steps once a frame steps by HOST_PER_FIELD(step), the step scaled
// by the fields the frame spans (1 at 60 Hz), and the frame code that draws random numbers or
// counts whole frames does so only on the frames HostFrameEndsField() marks, once a field.
// On the PlayStation 2 both are the frame's own step.

#ifdef RESONANCE_HOST
extern "C" unsigned HostFrameTick(void);
extern "C" unsigned HostWaitFrameTick(unsigned nTarget);
extern "C" float HostFrameFields(void);
extern "C" int HostFrameEndsField(void);
extern "C" void HostEvent(const char *pszFormat, ...); // FREQ_EVENTLOG
#define HOST_PER_FIELD(step) ((step) * HostFrameFields())
#else
#define HOST_PER_FIELD(step) (step)
#endif
