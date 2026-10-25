// Self-check of the TexReplace key: one texture written into GS memory at two addresses and buffer
// widths, among different leftover memory, hashes the same; a changed texel or a changed used CLUT
// entry changes the key; a changed unused CLUT entry does not. The raw GS bytes of the two copies
// differ, so the check tells an address-free key from one over GS memory.
// Build: cmake --build build-host --target texreplace_check
#include <cstdio>
#include <random>
#include <vector>
#include "TexReplace.h"
#include "gs/GsPixelFormats.h"
#include "xxhash.h"

namespace
{
	using TEX0 = CGSHandler::TEX0;
	int g_failures = 0;

	void Expect(bool ok, const char* what, unsigned psm)
	{
		printf("%s psm 0x%02x: %s\n", ok ? "ok  " : "FAIL", psm, what);
		g_failures += !ok;
	}

	TEX0 Tex(unsigned psm, unsigned tbp, unsigned tbw)
	{
		auto tex0 = make_convertible<TEX0>(0);
		tex0.nBufPtr = tbp;
		tex0.nBufWidth = tbw;
		tex0.nPsm = psm;
		tex0.nWidth = 6;  // 64
		tex0.nPad0 = 1;   // 32 high
		tex0.nCPSM = CGSHandler::PSMCT32;
		return tex0;
	}

	template <typename Indexor>
	void Write(uint8* ram, const TEX0& tex0, const std::vector<uint32>& texels)
	{
		Indexor indexor(ram, tex0.GetBufPtr(), tex0.nBufWidth);
		for(uint32 y = 0; y < tex0.GetHeight(); y++)
			for(uint32 x = 0; x < tex0.GetWidth(); x++)
				indexor.SetPixel(x, y, texels[y * tex0.GetWidth() + x]);
	}

	void Write(uint8* ram, const TEX0& tex0, const std::vector<uint32>& texels)
	{
		using PF = CGsPixelFormats;
		switch(tex0.nPsm)
		{
		case CGSHandler::PSMT8: Write<PF::CPixelIndexorPSMT8>(ram, tex0, texels); break;
		case CGSHandler::PSMT4: Write<PF::CPixelIndexorPSMT4>(ram, tex0, texels); break;
		case CGSHandler::PSMCT16: Write<PF::CPixelIndexorPSMCT16>(ram, tex0, texels); break;
		default: Write<PF::CPixelIndexorPSMCT32>(ram, tex0, texels); break;
		}
	}
}

int main()
{
	std::mt19937 random(1);
	std::vector<uint8> ram(CGSHandler::RAMSIZE);
	for(auto& b : ram) b = static_cast<uint8>(random());

	for(unsigned psm : {CGSHandler::PSMT8, CGSHandler::PSMT4, CGSHandler::PSMCT32, CGSHandler::PSMCT16})
	{
		unsigned mask = psm == CGSHandler::PSMT8 ? 0x7F : psm == CGSHandler::PSMT4 ? 0x7 : psm == CGSHandler::PSMCT16 ? 0xFFFF : 0xFFFFFFFF;
		std::vector<uint32> texels(64 * 32);
		for(auto& t : texels) t = random() & mask; // indexed: the upper half of the CLUT stays unused
		TEX0 a = Tex(psm, 0x0400, 1), b = Tex(psm, 0x2A00, 4);
		Write(ram.data(), a, texels);
		Write(ram.data(), b, texels);
		uint64 ha = TexReplace::HashTexels(ram.data(), a), hb = TexReplace::HashTexels(ram.data(), b);
		Expect(ha == hb, "same texels at TBP0 0x0400/TBW 1 and 0x2A00/TBW 4 hash the same", psm);
		Expect(XXH3_64bits(&ram[a.GetBufPtr()], 8192) != XXH3_64bits(&ram[b.GetBufPtr()], 8192), "their raw GS bytes differ", psm);

		std::vector<uint32> clut(256), clut2;
		for(auto& c : clut) c = static_cast<uint32>(random());
		clut2 = clut;
		clut2[CGsPixelFormats::IsPsmIDTEX4(psm) ? 12 : 200] ^= 1; // unused entry
		Expect(TexReplace::Key(ha, a, clut.data()) == TexReplace::Key(hb, b, clut2.data()), "an unused CLUT entry is not in the key", psm);
		if(CGsPixelFormats::IsPsmIDTEX(psm))
		{
			clut2[texels[0]] ^= 1; // used entry
			Expect(TexReplace::Key(ha, a, clut.data()) != TexReplace::Key(hb, b, clut2.data()), "a used CLUT entry is in the key", psm);
		}

		texels[100] ^= 1;
		Write(ram.data(), b, texels);
		Expect(TexReplace::HashTexels(ram.data(), b) != ha, "one changed texel changes the hash", psm);
	}
	printf(g_failures ? "TEXREPLACE_CHECK_FAIL %d\n" : "TEXREPLACE_CHECK_OK\n", g_failures);
	return g_failures != 0;
}
