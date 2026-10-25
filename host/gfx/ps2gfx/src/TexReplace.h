// HD texture dump and replacement, independent of the GS renderer.
//
// FREQ_TEX_DUMP=<dir> writes each distinct texture the GS samples once, as <dir>/<key>.png (RGBA,
// decoded through its CLUT), and keeps <dir>/index.tsv: key, kind, PSM, CPSM, size, the present it
// was first drawn in, the presents it was drawn in, the TBP0s it was seen at, a name, the
// texel hash (equal for the CLUT variants of one texture), and the CLAMP wrap modes it was drawn
// with, S/T, each the set of r (REPEAT), c (CLAMP), C (REGION_CLAMP), R (REGION_REPEAT) seen.
// FREQ_TEX_REPLACE=<dir> draws <dir>/<key>.png, of any integer multiple of the texture's size, in
// place of the texture.
//
// The key is a hash of the texture's content and never of where it sits in GS memory: the TW x TH
// texels in raster order (indices for indexed formats), and for indexed formats the CLUT entries
// the draw looks up (entries no texel uses are left out, as they hold leftover memory). Kinds: file (dumped, replaceable), gen (texels the game drew at run time,
// reported by the game, never dumped or replaced) and rt (a GS framebuffer sampled as a texture).
//
// PNG alpha: for PSMCT32 and for CLUTs in PSMCT32 the GS's 0..0x80 is stored doubled (0..0xFF) and
// halved on load; for PSMCT16 and CLUTs in PSMCT16 the A bit is 0 or 0xFF and loads as is (the
// renderer still expands it with TEXA); PSMCT24 has no alpha.
#pragma once

#include <vector>
#include "gs/GSHandler.h"

namespace TexReplace
{
	struct Image
	{
		uint32 width = 0;
		uint32 height = 0;
		std::vector<uint32> rgba; // R in the low byte, alpha in the GS's range for the format
	};

	// Whether FREQ_TEX_DUMP or FREQ_TEX_REPLACE is set. Nothing else here runs otherwise.
	bool Active();

	// Hash of the texels of tex0 in GS memory.
	uint64 HashTexels(uint8* ram, const CGSHandler::TEX0& tex0);
	// The key: the texel hash, and for an indexed format the used entries of the converted CLUT and CPSM.
	uint64 Key(uint64 texelHash, const CGSHandler::TEX0& tex0, const uint32* clut);

	// Every textured state the GS sets up, on the GS thread. True when the key has a replacement;
	// the first time, hd holds it and the renderer makes its texture.
	bool Bind(uint64 key, uint64 texelHash, uint8* ram, const CGSHandler::TEX0& tex0, const uint32* clut, const CGSHandler::CLAMP& clamp, Image& hd);
	// A GS framebuffer sampled as a texture.
	void BindRenderTarget(const CGSHandler::TEX0& tex0);
	// Each presented frame, on the GS thread.
	void Present();

	// The game drew these texels at run time (any thread). pixels are the upload's rows, stride bytes apart.
	void Generated(const char* name, uint32 psm, uint32 width, uint32 height, const void* pixels, uint32 stride);
}
