// FREQ_FILTER sets a look over the presented frame, the same in the OpenGL and the Vulkan presents.
// off (default) presents the frame as drawn. bloom adds a light glow around bright neon from twelve
// taps in two rings around each pixel, each counting by how far its luma passes a threshold. crt
// darkens each GS line towards its edges, as a TV's scanlines, with a small gain for lost light.
// Both stay inside the one present pass (no extra images or passes), for 4K at 120 Hz on the PS5.
// ponytail: twelve full-resolution taps in that pass; a half-resolution blur chain if the PS5's
// frame line shows the present over budget.
#pragma once

#include <cstdlib>
#include <cstring>

namespace PresentFilter
{
	enum Mode
	{
		OFF,
		BLOOM,
		CRT,
	};

	// Read at each present, so that a change from the settings applies at once.
	inline Mode Setting()
	{
		const char* setting = getenv("FREQ_FILTER");
		if(setting != nullptr && strcmp(setting, "bloom") == 0) return BLOOM;
		if(setting != nullptr && strcmp(setting, "crt") == 0) return CRT;
		return OFF;
	}

	// A tap's offset in GS pixels and its weight.
	struct Tap
	{
		float x, y, weight;
	};

	// Six taps at 2 pixels, and six at 5 pixels turned half a step; the weights add up to 0.6.
	constexpr Tap bloomTaps[] = {
	    {2.0f, 0.0f, 0.06f}, {1.0f, 1.732f, 0.06f}, {-1.0f, 1.732f, 0.06f},
	    {-2.0f, 0.0f, 0.06f}, {-1.0f, -1.732f, 0.06f}, {1.0f, -1.732f, 0.06f},
	    {4.33f, 2.5f, 0.04f}, {0.0f, 5.0f, 0.04f}, {-4.33f, 2.5f, 0.04f},
	    {-4.33f, -2.5f, 0.04f}, {0.0f, -5.0f, 0.04f}, {4.33f, -2.5f, 0.04f},
	};
	// A tap counts by saturate((luma - threshold) * knee).
	constexpr float bloomThreshold = 0.45f;
	constexpr float bloomKnee = 2.5f;

	// A pixel at distance d from its line's centre (0 at the centre, 1 at the edge) takes
	// (1 - depth * d * d) * gain.
	constexpr float scanlineDepth = 0.45f;
	constexpr float scanlineGain = 1.12f;
}
