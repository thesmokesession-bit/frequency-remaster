#include "TexReplace.h"

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <fcntl.h>
#include <png.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gs/GsPixelFormats.h"
#include "xxhash.h"

namespace
{
	using TEX0 = CGSHandler::TEX0;

	const char* g_dumpDir = getenv("FREQ_TEX_DUMP");
	const char* g_replaceDir = getenv("FREQ_TEX_REPLACE");

	struct Entry
	{
		const char* kind;
		uint32 psm, cpsm, width, height;
		uint32 first, count = 0, lastPresent = ~0U;
		uint64 texels;
		std::vector<uint32> tbps;
		std::string name;
		uint32 wrapS = 0, wrapT = 0; // bit per CLAMP mode seen
	};

	// GS thread only.
	std::unordered_map<uint64, Entry> g_entries;
	uint32 g_present = 0;
	bool g_indexDirty = false;
	// The indices each indexed texture uses: a CLUT entry no texel uses is not part of a key, as
	// the game leaves such entries holding whatever its memory held.
	std::unordered_map<uint64, std::bitset<256>> g_usedIndices;

	// Texel hashes the game drew, with their texture's name. Written on the game's thread.
	std::mutex g_generatedLock;
	std::unordered_map<uint64, std::string> g_generated;
	// Texel hashes of file textures' level 0 (reported as "file:<path>"), with the file's path.
	std::unordered_map<uint64, std::string> g_loaded;

	uint32 Width(const TEX0& tex0)
	{
		return std::min<uint32>(tex0.GetWidth(), CGSHandler::TEX0_MAX_TEXTURE_SIZE);
	}
	uint32 Height(const TEX0& tex0)
	{
		return std::min<uint32>(tex0.GetHeight(), CGSHandler::TEX0_MAX_TEXTURE_SIZE);
	}

	// Bytes per texel in the hashed form: an index, a 16-bit colour, or a 32-bit colour.
	uint32 UnitSize(uint32 psm)
	{
		if(CGsPixelFormats::IsPsmIDTEX(psm)) return 1;
		if(psm == CGSHandler::PSMCT16 || psm == CGSHandler::PSMCT16S) return 2;
		return 4;
	}

	uint64 HashUnits(uint32 psm, uint32 width, uint32 height, const std::vector<uint8>& units)
	{
		uint64 seed = psm | (static_cast<uint64>(width) << 8) | (static_cast<uint64>(height) << 24);
		return XXH3_64bits_withSeed(units.data(), units.size(), seed);
	}

	template <typename Indexor, typename T, typename F>
	void Fill(uint8* ram, const TEX0& tex0, std::vector<uint8>& units, F f)
	{
		Indexor indexor(ram, tex0.GetBufPtr(), tex0.nBufWidth);
		auto dst = reinterpret_cast<T*>(units.data());
		for(uint32 y = 0; y < Height(tex0); y++)
			for(uint32 x = 0; x < Width(tex0); x++)
				*dst++ = static_cast<T>(f(indexor.GetPixel(x, y)));
	}

	std::vector<uint8> ReadUnits(uint8* ram, const TEX0& tex0)
	{
		using PF = CGsPixelFormats;
		std::vector<uint8> units(Width(tex0) * Height(tex0) * UnitSize(tex0.nPsm));
		auto same = [](uint32 v) { return v; };
		switch(tex0.nPsm)
		{
		case CGSHandler::PSMT8: Fill<PF::CPixelIndexorPSMT8, uint8>(ram, tex0, units, same); break;
		case CGSHandler::PSMT4: Fill<PF::CPixelIndexorPSMT4, uint8>(ram, tex0, units, same); break;
		case CGSHandler::PSMT8H: Fill<PF::CPixelIndexorPSMCT32, uint8>(ram, tex0, units, [](uint32 v) { return v >> 24; }); break;
		case CGSHandler::PSMT4HL: Fill<PF::CPixelIndexorPSMCT32, uint8>(ram, tex0, units, [](uint32 v) { return (v >> 24) & 0xF; }); break;
		case CGSHandler::PSMT4HH: Fill<PF::CPixelIndexorPSMCT32, uint8>(ram, tex0, units, [](uint32 v) { return v >> 28; }); break;
		case CGSHandler::PSMCT16: Fill<PF::CPixelIndexorPSMCT16, uint16>(ram, tex0, units, same); break;
		case CGSHandler::PSMCT16S: Fill<PF::CPixelIndexorPSMCT16S, uint16>(ram, tex0, units, same); break;
		case CGSHandler::PSMCT24: Fill<PF::CPixelIndexorPSMCT32, uint32>(ram, tex0, units, [](uint32 v) { return v & 0xFFFFFF; }); break;
		default: Fill<PF::CPixelIndexorPSMCT32, uint32>(ram, tex0, units, same); break;
		}
		return units;
	}

	// Whether the PNG holds the GS's 0..0x80 alpha doubled.
	bool DoubledAlpha(const TEX0& tex0)
	{
		uint32 psm = CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) ? tex0.nCPSM : tex0.nPsm;
		return psm != CGSHandler::PSMCT16 && psm != CGSHandler::PSMCT16S && psm != CGSHandler::PSMCT24;
	}

	std::vector<uint32> DecodeRgba(const TEX0& tex0, const std::vector<uint8>& units, const uint32* clut)
	{
		std::vector<uint32> rgba(Width(tex0) * Height(tex0));
		for(size_t i = 0; i < rgba.size(); i++)
		{
			uint32 c;
			switch(UnitSize(tex0.nPsm))
			{
			case 1: c = clut[units[i]]; break;
			case 2:
			{
				uint32 v = reinterpret_cast<const uint16*>(units.data())[i];
				auto x5 = [](uint32 x) { return (x << 3) | (x >> 2); };
				c = x5(v & 0x1F) | (x5((v >> 5) & 0x1F) << 8) | (x5((v >> 10) & 0x1F) << 16) | ((v & 0x8000) ? 0xFF000000 : 0);
				break;
			}
			default: c = reinterpret_cast<const uint32*>(units.data())[i]; break;
			}
			if(tex0.nPsm == CGSHandler::PSMCT24 || (CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) && tex0.nCPSM == CGSHandler::PSMCT24))
				c |= 0xFF000000;
			else if(DoubledAlpha(tex0))
				c = (c & 0xFFFFFF) | (std::min<uint32>((c >> 24) * 2, 0xFF) << 24);
			rgba[i] = c;
		}
		return rgba;
	}

	std::string KeyPath(const char* dir, uint64 key)
	{
		char name[32];
		snprintf(name, sizeof(name), "/%016llx.png", static_cast<unsigned long long>(key));
		return dir + std::string(name);
	}

	TexReplace::Image Load(uint64 key, const TEX0& tex0)
	{
		TexReplace::Image hd;
		std::string path = KeyPath(g_replaceDir, key);
		if(access(path.c_str(), R_OK) != 0) return hd;
		png_image img = {};
		img.version = PNG_IMAGE_VERSION;
		if(!png_image_begin_read_from_file(&img, path.c_str()))
		{
			fprintf(stderr, "texreplace: %s: %s\n", path.c_str(), img.message);
			return hd;
		}
		img.format = PNG_FORMAT_RGBA;
		uint32 w = Width(tex0), h = Height(tex0);
		if(img.width < w || img.height < h || img.width % w || img.height % h)
		{
			fprintf(stderr, "texreplace: %s is %ux%u, not a multiple of %ux%u\n", path.c_str(), img.width, img.height, w, h);
			png_image_free(&img);
			return hd;
		}
		hd.rgba.resize(static_cast<size_t>(img.width) * img.height);
		if(!png_image_finish_read(&img, nullptr, hd.rgba.data(), 0, nullptr))
		{
			fprintf(stderr, "texreplace: %s: %s\n", path.c_str(), img.message);
			hd.rgba.clear();
			return hd;
		}
		hd.width = img.width;
		hd.height = img.height;
		bool doubled = DoubledAlpha(tex0), noAlpha = tex0.nPsm == CGSHandler::PSMCT24;
		for(auto& c : hd.rgba)
			if(noAlpha)
				c &= 0xFFFFFF;
			else if(doubled)
				c = (c & 0xFFFFFF) | ((((c >> 24) + 1) >> 1) << 24);
		fprintf(stderr, "texreplace: %016llx %ux%u drawn as %ux%u\n", static_cast<unsigned long long>(key), w, h, hd.width, hd.height);
		return hd;
	}

	void WriteIndex()
	{
		g_indexDirty = false;
		std::vector<std::pair<uint64, const Entry*>> rows;
		for(const auto& e : g_entries) rows.emplace_back(e.first, &e.second);
		std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second->first != b.second->first ? a.second->first < b.second->first : a.first < b.first; });
		std::string tmp = std::string(g_dumpDir) + "/index.tsv.tmp";
		// fopen() is the game's (host/io/stdio.cpp); host files go through open().
		int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		FILE* f = fd >= 0 ? fdopen(fd, "w") : nullptr;
		if(!f) return;
		fprintf(f, "# key\tkind\tpsm\tcpsm\twidth\theight\tfirst\tcount\ttbp0\tname\ttexels\twrap\n");
		for(const auto& [key, e] : rows)
		{
			fprintf(f, "%016llx\t%s\t0x%02x\t0x%02x\t%u\t%u\t%u\t%u\t", static_cast<unsigned long long>(key), e->kind, e->psm, e->cpsm,
			        e->width, e->height, e->first, e->count);
			for(size_t i = 0; i < e->tbps.size(); i++) fprintf(f, i ? ",%u" : "%u", e->tbps[i]);
			fprintf(f, "\t%s\t%016llx\t", e->name.empty() ? "-" : e->name.c_str(), static_cast<unsigned long long>(e->texels));
			for(uint32 axis = 0; axis < 2; axis++)
			{
				for(uint32 mode = 0; mode < 4; mode++)
					if(((axis ? e->wrapT : e->wrapS) >> mode) & 1) fputc("rcCR"[mode], f);
				fputc(axis ? '\n' : '/', f);
			}
		}
		fclose(f);
		rename(tmp.c_str(), (std::string(g_dumpDir) + "/index.tsv").c_str());
	}

	Entry& Note(uint64 key, uint64 texels, const char* kind, const TEX0& tex0, bool& isNew)
	{
		auto [it, inserted] = g_entries.try_emplace(key);
		auto& e = it->second;
		isNew = inserted;
		if(inserted)
		{
			e.kind = kind;
			e.psm = tex0.nPsm;
			e.cpsm = CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) ? tex0.nCPSM : 0;
			e.width = Width(tex0);
			e.height = Height(tex0);
			e.first = g_present;
			e.texels = texels;
			g_indexDirty = true;
		}
		if(e.lastPresent != g_present)
		{
			e.lastPresent = g_present;
			e.count++;
		}
		// ponytail: the first 8 addresses, enough to show a texture moved
		uint32 tbp = tex0.GetBufPtr();
		if(e.tbps.size() < 8 && std::find(e.tbps.begin(), e.tbps.end(), tbp) == e.tbps.end())
		{
			e.tbps.push_back(tbp);
			g_indexDirty = true;
		}
		return e;
	}
}

bool TexReplace::Active()
{
	static const bool active = [] {
		if(g_dumpDir) mkdir(g_dumpDir, 0777);
		return g_dumpDir || g_replaceDir;
	}();
	return active;
}

uint64 TexReplace::HashTexels(uint8* ram, const TEX0& tex0)
{
	auto units = ReadUnits(ram, tex0);
	uint64 hash = HashUnits(tex0.nPsm, Width(tex0), Height(tex0), units);
	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		auto& used = g_usedIndices[hash];
		for(uint8 index : units) used.set(index);
	}
	return hash;
}

uint64 TexReplace::Key(uint64 texelHash, const TEX0& tex0, const uint32* clut)
{
	if(!CGsPixelFormats::IsPsmIDTEX(tex0.nPsm)) return texelHash;
	unsigned entries = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) ? 16 : 256;
	uint32 usedClut[256] = {};
	auto used = g_usedIndices.find(texelHash);
	for(unsigned i = 0; i < entries; i++)
		if(used == g_usedIndices.end() || used->second.test(i)) usedClut[i] = clut[i];
	return XXH3_64bits_withSeed(usedClut, entries * sizeof(uint32), texelHash ^ (static_cast<uint64>(tex0.nCPSM) << 56));
}

std::vector<uint32> TexReplace::Decode(uint8* ram, const TEX0& tex0, const uint32* clut)
{
	auto units = ReadUnits(ram, tex0);
	std::vector<uint32> rgba(Width(tex0) * Height(tex0));
	bool clut16 = CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) && ((tex0.nCPSM == CGSHandler::PSMCT16) || (tex0.nCPSM == CGSHandler::PSMCT16S));
	for(size_t i = 0; i < rgba.size(); i++)
	{
		uint32 c;
		switch(UnitSize(tex0.nPsm))
		{
		case 1:
			c = clut[units[i]];
			if(clut16) c |= (c >> 5) & 0x070707; // MakeLinearCLUT shifts 5 bits up; the GS reads them replicated
			break;
		case 2:
		{
			uint32 v = reinterpret_cast<const uint16*>(units.data())[i];
			auto x5 = [](uint32 x) { return (x << 3) | (x >> 2); };
			c = x5(v & 0x1F) | (x5((v >> 5) & 0x1F) << 8) | (x5((v >> 10) & 0x1F) << 16) | ((v & 0x8000) ? 0xFF000000 : 0);
			break;
		}
		default: c = reinterpret_cast<const uint32*>(units.data())[i]; break;
		}
		if(tex0.nPsm == CGSHandler::PSMCT24) c &= 0xFFFFFF; // as GetTextureColor reads it (a CLUT keeps its alpha)
		rgba[i] = c;
	}
	return rgba;
}

bool TexReplace::Bind(uint64 key, uint64 texelHash, uint8* ram, const TEX0& tex0, const uint32* clut, const CGSHandler::CLAMP& clamp, Image& hd)
{
	bool isNew;
	const char* kind = "file";
	std::string name;
	if(g_entries.find(key) == g_entries.end())
	{
		std::lock_guard<std::mutex> lock(g_generatedLock);
		auto gen = g_generated.find(texelHash);
		auto loaded = g_loaded.find(texelHash);
		if(gen != g_generated.end())
		{
			kind = "gen";
			name = gen->second;
		}
		else if(loaded != g_loaded.end())
			name = loaded->second;
	}
	auto& e = Note(key, texelHash, kind, tex0, isNew);
	if(isNew) e.name = name;
	if(!((e.wrapS >> clamp.nWMS) & (e.wrapT >> clamp.nWMT) & 1)) g_indexDirty = true;
	e.wrapS |= 1 << clamp.nWMS;
	e.wrapT |= 1 << clamp.nWMT;
	if(e.kind[0] == 'g') return false; // the game's own drawing stays as drawn
	if(isNew)
	{
		if(g_dumpDir)
		{
			auto rgba = DecodeRgba(tex0, ReadUnits(ram, tex0), clut);
			png_image img = {};
			img.version = PNG_IMAGE_VERSION;
			img.width = Width(tex0);
			img.height = Height(tex0);
			img.format = PNG_FORMAT_RGBA;
			if(!png_image_write_to_file(&img, KeyPath(g_dumpDir, key).c_str(), 0, rgba.data(), 0, nullptr))
				fprintf(stderr, "texreplace: cannot write %s: %s\n", KeyPath(g_dumpDir, key).c_str(), img.message);
		}
		if(g_replaceDir)
		{
			hd = Load(key, tex0);
			e.kind = hd.rgba.empty() ? "file" : "hd";
		}
	}
	return e.kind[0] == 'h';
}

void TexReplace::BindRenderTarget(const TEX0& tex0)
{
	uint64 where[] = {tex0.GetBufPtr(), tex0.nBufWidth, tex0.nPsm, Width(tex0), Height(tex0)};
	bool isNew;
	Note(XXH3_64bits_withSeed(where, sizeof(where), 0x7274), 0, "rt", tex0, isNew);
}

void TexReplace::Present()
{
	g_present++;
	if(g_dumpDir && (g_indexDirty || g_present % 60 == 0)) WriteIndex();
}

void TexReplace::Generated(const char* name, uint32 psm, uint32 width, uint32 height, const void* pixels, uint32 stride)
{
	std::vector<uint8> units(width * height * UnitSize(psm));
	auto src = static_cast<const uint8*>(pixels);
	uint8* dst = units.data();
	for(uint32 y = 0; y < height; y++, src += stride)
	{
		switch(psm)
		{
		case CGSHandler::PSMT4:
			for(uint32 x = 0; x < width; x++) *dst++ = (x & 1) ? (src[x / 2] >> 4) : (src[x / 2] & 0xF);
			break;
		case CGSHandler::PSMCT24:
			for(uint32 x = 0; x < width; x++, dst += 4)
			{
				dst[0] = src[x * 3], dst[1] = src[x * 3 + 1], dst[2] = src[x * 3 + 2], dst[3] = 0;
			}
			break;
		default:
			std::copy(src, src + width * UnitSize(psm), dst);
			dst += width * UnitSize(psm);
			break;
		}
	}
	uint64 hash = HashUnits(psm, width, height, units);
	std::lock_guard<std::mutex> lock(g_generatedLock);
	if(name && strncmp(name, "file:", 5) == 0)
	{
		// <dump>/loaded.tsv lists every file texture uploaded, drawn or not (coverage of the disc).
		auto& known = g_loaded[hash];
		if(g_dumpDir && known != name + 5)
		{
			int fd = open((std::string(g_dumpDir) + "/loaded.tsv").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
			if(fd >= 0)
			{
				char line[512]; // no dprintf in the PS5's C library
				const int n = snprintf(line, sizeof(line), "%016llx\t%s\t0x%02x\t%u\t%u\n", static_cast<unsigned long long>(hash), name + 5, psm, width, height);
				if(n > 0) write(fd, line, std::min(static_cast<size_t>(n), sizeof(line) - 1));
				close(fd);
			}
		}
		known = name + 5;
	}
	else
		g_generated[hash] = name ? name : "";
}
