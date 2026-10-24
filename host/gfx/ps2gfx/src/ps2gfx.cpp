// libps2gfx: wires Play!'s VIF1/VU1/GIF/GS (BSD-2) behind a C API. The DMA chain walker below
// follows Play!'s Dmac_Channel.cpp source-chain logic, but reads PS2 memory through a callback and
// feeds the devices from a staging buffer (their "RAM"), so device code stays unmodified.
#include "ps2gfx.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <png.h>

#include "AlignedAlloc.h"
#include "AppConfig.h"
#include "PtrStream.h"
#include "placeholder_def.h"
#include "zip/ZipArchiveReader.h"
#include "Ps2Const.h"
#include "MIPS.h"
#include "ee/DMAC.h"
#include "ee/GIF.h"
#include "ee/INTC.h"
#include "ee/MA_VU.h"
#include "ee/Vif.h"
#include "ee/Vpu.h"
#include "ee/VuExecutor.h"
#include "gs/GsDebuggerInterface.h"
#include "gs/GSH_Null.h"
#include "gs/GSH_OpenGL/GSH_OpenGL.h"
#ifdef PS2GFX_VULKAN
#include "gs/GSH_Vulkan/GSH_VulkanOffscreen.h"
#endif

namespace
{
	enum : uint32
	{
		STAGE_SIZE = 0x10000 * 0x10 + 0x100, // one DMA block is at most 0xFFFF qwords
		VIF_STAT_VIS = 1 << 10,
		VIF_FBRST_STC = 0x08,
		GIF_STAT_M3P = 1 << 1,
	};

	ps2gfx_config g_cfg;

	class CGSH_OpenGLLib : public CGSH_OpenGL
	{
	public:
		CGSH_OpenGLLib()
		    : CGSH_OpenGL(true)
		{
		}

	protected:
		void InitializeImpl() override // on the GS thread
		{
			if(g_cfg.make_current) g_cfg.make_current(g_cfg.user);
			glewExperimental = GL_TRUE;
			glewInit();
			CGSH_OpenGL::InitializeImpl();
		}
		void PresentBackbuffer() override
		{
			if(capture)
			{
				// A capture re-presents a frame already shown, so it is read back instead of swapped.
				captured = Framework::CBitmap(m_presentationParams.windowWidth, m_presentationParams.windowHeight, 32);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glReadPixels(0, 0, captured.GetWidth(), captured.GetHeight(), GL_BGRA, GL_UNSIGNED_BYTE, captured.GetPixels());
				capture = false;
				return;
			}
			if(g_cfg.present) g_cfg.present(g_cfg.user);
		}

	public:
		bool capture = false;
		Framework::CBitmap captured;
	};

	struct State
	{
		State()
		    : stage(static_cast<uint8*>(framework_aligned_alloc(STAGE_SIZE, 0x10)))
		    , spr(static_cast<uint8*>(framework_aligned_alloc(PS2::EE_SPR_SIZE, 0x10)))
		    , vuMem0(static_cast<uint8*>(framework_aligned_alloc(PS2::VUMEM0SIZE, 0x10)))
		    , vuMem1(static_cast<uint8*>(framework_aligned_alloc(PS2::VUMEM1SIZE, 0x10)))
		    , microMem1(new uint8[PS2::MICROMEM1SIZE])
		    , ee(MEMORYMAP_ENDIAN_LSBF)
		    , vu1(MEMORYMAP_ENDIAN_LSBF)
		    , maVu1(PS2::VUMEM1SIZE - 1)
		    , dmac(stage, spr, vuMem0, vuMem1, ee)
		    , gif(gs, dmac, stage, spr)
		{
			memset(stage, 0, STAGE_SIZE);
			memset(spr, 0, PS2::EE_SPR_SIZE);
			memset(vuMem1, 0, PS2::VUMEM1SIZE);
			memset(microMem1, 0, PS2::MICROMEM1SIZE);
			vpu1 = std::make_shared<CVpu>(1, CVpu::VPUINIT(microMem1, vuMem1, &vu1), gif, intc, stage, spr);

			// VU1 context, as in Play!'s Ee_SubSystem.cpp
			vu1.m_executor = std::make_unique<CVuExecutor>(vu1, PS2::MICROMEM1SIZE);
			vu1.m_pMemoryMap->InsertReadMap(0x00000000, 0x00003FFF, vuMem1, 0x00);
			vu1.m_pMemoryMap->InsertReadMap(0x00008000, 0x00008FFF, [this](uint32 a, uint32) { return Vu1IoRead(a); }, 0x01);
			vu1.m_pMemoryMap->InsertWriteMap(0x00000000, 0x00003FFF, vuMem1, 0x00);
			vu1.m_pMemoryMap->InsertWriteMap(0x00008000, 0x00008FFF, [this](uint32 a, uint32 v) { return Vu1IoWrite(a, v); }, 0x01);
			vu1.m_pMemoryMap->InsertInstructionMap(0x00000000, 0x00003FFF, microMem1, 0x01);
			vu1.m_pArch = &maVu1;
			vu1.m_pAddrTranslator = CMIPS::TranslateAddress64;
			vu1.m_vuMem = vuMem1;
			vu1.Reset();
			gif.Reset();
			vpu1->Reset();
			dmac.Reset();
			intc.Reset();
		}

		~State()
		{
			if(gs)
			{
				gs->Release();
				delete gs;
			}
			vpu1.reset();
			framework_aligned_free(stage);
			framework_aligned_free(spr);
			framework_aligned_free(vuMem0);
			framework_aligned_free(vuMem1);
			delete[] microMem1;
		}

		uint32 Vu1IoRead(uint32 address)
		{
			switch(address)
			{
			case CVpu::VU_ADDR_ITOP: return vpu1->GetVif().GetITOP();
			case CVpu::VU_ADDR_TOP: return vpu1->GetVif().GetTOP();
			default: return 0;
			}
		}

		uint32 Vu1IoWrite(uint32 address, uint32 value)
		{
			if(address != CVpu::VU_ADDR_XGKICK) return 0;
			xgkicks++;
			if(vu1Enabled) vpu1->ProcessXgKick(value);
			return 0;
		}

		void RunVu1()
		{
			// ponytail: 10M VU instructions per kick (about two frames of VU1 time) before giving up on a runaway program
			for(int i = 0; vpu1->IsVuRunning() && i < 2000; i++)
				vpu1->Execute(5000);
			if(vpu1->IsVuRunning()) fprintf(stderr, "ps2gfx: VU1 program did not finish\n");
		}

		void Read(uint32 addr, void* dst, size_t n)
		{
			if(g_cfg.mem_read)
				g_cfg.mem_read(g_cfg.user, addr, dst, n);
			else if(addr & 0x80000000)
				memcpy(dst, static_cast<uint8*>(g_cfg.spr_base) + (addr & (PS2::EE_SPR_SIZE - 1)), n);
			else
				memcpy(dst, reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), n);
		}

		// Copies qwc quadwords at addr to the staging buffer and feeds them to the device until consumed.
		// dir: CHCR.DIR, 1 = from memory. 0 on VIF1 is a GS->memory readback, which is not supported.
		bool Feed(int ch, uint32 addr, uint32 qwc, bool tagIncluded, uint32 dir = 1)
		{
			if(dir == 0)
			{
				fprintf(stderr, "ps2gfx: GS readback (CHCR.DIR = 0) on ch%d ignored\n", ch);
				return true;
			}
			if(qwc == 0) return true;
			Read(addr, stage, qwc * 0x10);
			uint32 done = 0;
			for(int stalls = 0; done < qwc;)
			{
				uint32 n = (ch == 1) ? vpu1->GetVif().ReceiveDMA(done * 0x10, qwc - done, dir, tagIncluded && done == 0)
				                     : gif.ReceiveDMA(done * 0x10, qwc - done, dir, tagIncluded && done == 0);
				done += n;
				if(done == qwc) break;
				if(n == 0 && ++stalls > 4)
				{
					fprintf(stderr, "ps2gfx: DMA ch%d stalled at 0x%08X (+%u/%u qw), VIF1 STAT 0x%08X GIF STAT 0x%08X\n", ch, addr,
					        done, qwc, vpu1->GetVif().GetRegister(CVif::VIF1_STAT), gif.GetRegister(CGIF::GIF_STAT));
					return false;
				}
				if(n) stalls = 0;
				Unstall();
			}
			return true;
		}

		// What the rest of the PS2 would do while a channel waits: finish VU1, acknowledge VIF1 interrupts.
		void Unstall()
		{
			if(vpu1->IsVuRunning()) RunVu1();
			auto& vif = vpu1->GetVif();
			if(vif.GetRegister(CVif::VIF1_STAT) & VIF_STAT_VIS) vif.SetRegister(CVif::VIF1_FBRST, VIF_FBRST_STC);
		}

		int Dma(int ch, uint32 madr, uint32 qwc, uint32 tadr, uint32 chcr)
		{
			uint32 mod = (chcr >> 2) & 3;
			if(mod == 0) return Feed(ch, madr, qwc ? qwc : 1, false, chcr & 1) ? 0 : -1; // normal: QWC 0 still sends one qw
			bool tte = (chcr >> 6) & 1, tie = (chcr >> 7) & 1, retTop = false, init = (qwc == 0);
			uint32 asp = (chcr >> 4) & 3, asr[2] = {}, tag = chcr >> 16;
			if(qwc && !Feed(ch, madr, qwc, false)) return -1;
			for(int guard = 0; guard < 1000000; guard++)
			{
				if(!init)
				{
					uint32 id = (tag >> 12) & 7;
					if(id == 0 || id == 7) return 0;               // REFE, END
					if(tie && (tag & 0x8000)) return 0;            // IRQ with TIE
					if(retTop) return 0;                           // RET with ASP == 0
				}
				init = false;
				if(tte && !Feed(ch, tadr, 1, true)) return -1;
				if(tadr == 0) return 0;
				uint64 t;
				Read(tadr, &t, 8);
				tag = static_cast<uint32>(t >> 16) & 0xFFFF;
				uint32 id = (t >> 28) & 7, q = t & 0xFFFF, addr = static_cast<uint32>(t >> 32) & 0xFFFFFFF0;
				switch(id)
				{
				case 0: madr = addr; qwc = q; tadr += 0x10; break;                    // REFE
				case 1: madr = tadr + 0x10; qwc = q; tadr = madr + q * 0x10; break;   // CNT
				case 2: madr = tadr + 0x10; qwc = q; tadr = addr; break;              // NEXT
				case 3:
				case 4: madr = addr; qwc = q; tadr += 0x10; break;                    // REF, REFS
				case 5:                                                               // CALL
					madr = tadr + 0x10;
					qwc = q;
					if(asp >= 2)
					{
						fprintf(stderr, "ps2gfx: DMA CALL nesting overflow\n");
						return -1;
					}
					asr[asp++] = madr + q * 0x10;
					tadr = addr;
					break;
				case 6:                                                               // RET
					madr = tadr + 0x10;
					qwc = q;
					if(asp) tadr = asr[--asp];
					else retTop = true;
					break;
				case 7: madr = tadr + 0x10; qwc = q; break;                           // END
				}
				if(!Feed(ch, madr, qwc, false)) return -1;
			}
			fprintf(stderr, "ps2gfx: DMA chain did not terminate\n");
			return -1;
		}

		uint8* stage;
		uint8* spr;
		uint8* vuMem0;
		uint8* vuMem1;
		uint8* microMem1;
		CMIPS ee;
		CMIPS vu1;
		CMA_VU maVu1;
		CGSHandler* gs = nullptr;
		CINTC intc;
		CDMAC dmac;
		CGIF gif;
		std::shared_ptr<CVpu> vpu1;
		bool vu1Enabled = true;
		uint64 xgkicks = 0;
	};

	std::unique_ptr<State> g;

	bool WritePng(const char* path, const Framework::CBitmap& bmp, uint32 w, uint32 h, bool bottomUp = false)
	{
		w = std::min(w, bmp.GetWidth());
		h = std::min(h, bmp.GetHeight());
		png_image img = {};
		img.version = PNG_IMAGE_VERSION;
		img.width = w;
		img.height = h;
		img.format = PNG_FORMAT_BGRA;
		int stride = static_cast<int>(bmp.GetWidth() * 4);
		std::vector<uint8> px(bmp.GetPixels(), bmp.GetPixels() + bmp.GetPixelsSize());
		for(size_t i = 3; i < px.size(); i += 4) px[i] = 0xFF; // the reference ignores alpha too
		return png_image_write_to_file(&img, path, 0, px.data(), bottomUp ? -stride : stride, nullptr) != 0;
	}
}

extern "C" int ps2gfx_init(const ps2gfx_config* config)
{
	g_cfg = *config;
	g = std::make_unique<State>();
	CGSHandler* gs = nullptr;
	if(config->renderer == PS2GFX_RENDERER_OPENGL && config->make_current)
	{
		gs = new CGSH_OpenGLLib();
		auto& appConfig = CAppConfig::GetInstance();
		if(config->res_factor > 1) appConfig.SetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, config->res_factor);
		appConfig.RegisterPreferenceBoolean("ps2gfx.aspect43", false);
		appConfig.SetPreferenceBoolean("ps2gfx.aspect43", config->aspect == PS2GFX_ASPECT_4_3);
		appConfig.SetPreferenceBoolean(PREF_CGSHANDLER_WIDESCREEN, config->aspect == PS2GFX_ASPECT_16_9);
	}
	else if(config->renderer == PS2GFX_RENDERER_NULL)
		gs = new CGSH_Null();
#ifdef PS2GFX_VULKAN
	else if(config->renderer == PS2GFX_RENDERER_VULKAN)
		gs = new CGSH_VulkanOffscreen();
#endif
	if(!gs)
	{
		g.reset();
		return -1;
	}
	g->gs = gs;
	gs->SetIntc(&g->intc);
	gs->Initialize();
	CGSHandler::PRESENTATION_PARAMS pp;
	pp.windowWidth = config->window_width ? config->window_width : 640;
	pp.windowHeight = config->window_height ? config->window_height : 448;
	pp.mode = CGSHandler::PRESENTATION_MODE_FIT;
	if(config->renderer == PS2GFX_RENDERER_OPENGL) gs->SetPresentationParams(pp); // Vulkan offscreen has no swapchain
	gs->Reset();
	return 0;
}

extern "C" void ps2gfx_shutdown(void)
{
	if(g && getenv("PS2GFX_STATS")) fprintf(stderr, "ps2gfx: %llu XGKICKs\n", static_cast<unsigned long long>(g->xgkicks));
	g.reset();
}

extern "C" int ps2gfx_dma(int channel, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t chcr)
{
	if(channel != 1 && channel != 2) return -1;
	int r = g->Dma(channel, madr, qwc & 0xFFFF, tadr, chcr);
	if(channel == 1) g->RunVu1();
	return r;
}

extern "C" void ps2gfx_priv_write(uint32_t addr, uint64_t value)
{
	g->gs->WritePrivRegister(addr, static_cast<uint32>(value));
	g->gs->WritePrivRegister(addr + 4, static_cast<uint32>(value >> 32));
}

extern "C" void ps2gfx_priv_write32(uint32_t addr, uint32_t value)
{
	g->gs->WritePrivRegister(addr, value);
}

extern "C" uint64_t ps2gfx_priv_read(uint32_t addr)
{
	return g->gs->ReadPrivRegister(addr) | (static_cast<uint64>(g->gs->ReadPrivRegister(addr + 4)) << 32);
}

extern "C" void ps2gfx_set_csr(uint64_t value)
{
	ps2gfx_priv_write(CGSHandler::GS_CSR, value);
}

extern "C" void ps2gfx_set_imr(uint64_t value)
{
	ps2gfx_priv_write(CGSHandler::GS_IMR, value);
}

extern "C" void ps2gfx_set_crt(int interlaced, int mode, int frame_mode)
{
	g->gs->SetCrt(interlaced != 0, mode, frame_mode != 0);
}

extern "C" void ps2gfx_io_write(uint32_t addr, uint32_t value)
{
	if(addr >= CGIF::REGS_START && addr < CGIF::REGS_END)
		g->gif.SetRegister(addr, value);
	else if((addr >= CVif::REGS1_START && addr < CVif::REGS1_END) || (addr >= CVif::VIF1_FIFO_START && addr < CVif::VIF1_FIFO_END))
		g->vpu1->GetVif().SetRegister(addr, value);
	else if(addr >= CGIF::GIF_FIFO_START && addr < CGIF::GIF_FIFO_END)
		g->gif.SetRegister(addr, value);
	else if(addr == CVpu::EE_ADDR_VU_FBRST)
		g->vpu1->SetFbrst((value >> 8) & 0xF);
	else if(addr == CVpu::EE_ADDR_VU_CMSAR1 && (value & 7) == 0)
	{
		g->vpu1->ExecuteMicroProgram(value);
		g->RunVu1();
	}
	else
		fprintf(stderr, "ps2gfx: unhandled io write 0x%08X = 0x%08X\n", addr, value);
}

extern "C" uint32_t ps2gfx_io_read(uint32_t addr)
{
	if(addr >= CGIF::REGS_START && addr < CGIF::REGS_END) return g->gif.GetRegister(addr);
	if(addr >= CVif::REGS1_START && addr < CVif::REGS1_END) return g->vpu1->GetVif().GetRegister(addr);
	return 0;
}

extern "C" void ps2gfx_read_image(void* dst, size_t n)
{
	g->gs->ReadImageData(dst, static_cast<uint32>(n));
}

extern "C" void ps2gfx_vsync(void)
{
	g->gs->SetVBlank();
	g->gs->ResetVBlank();
}

extern "C" void ps2gfx_wait_frames(void)
{
	// Only the mailbox is touched, which takes its own lock. Unthreaded, there is nothing to wait for.
	g->gs->SendGSCall([]() {}, true, false);
}

extern "C" int ps2gfx_screenshot(const char* path)
{
	auto info = g->gs->GetCurrentDisplayInfo();
	const auto& l = info.layers[0];
	auto dbg = dynamic_cast<CGsDebuggerInterface*>(g->gs);
	if(!l.enabled || !dbg) return -1;
	g->gs->Finish(true);
	uint64 frame = (l.bufPtr / 8192) | (static_cast<uint64>(l.bufWidth / 64) << 16) | (static_cast<uint64>(l.psm) << 24);
	auto bmp = dbg->GetFramebuffer(frame);
	if(bmp.IsEmpty() || bmp.GetBitsPerPixel() != 32) return -1;
	return WritePng(path, bmp, info.width, info.height) ? 0 : -1;
}

extern "C" int ps2gfx_screenshot_output(const char* path)
{
	auto gs = dynamic_cast<CGSH_OpenGLLib*>(g->gs);
	if(!gs) return -1;
	gs->capture = true;
	gs->Flip(CGSHandler::FLIP_FLAG_FORCE | CGSHandler::FLIP_FLAG_WAIT);
	if(gs->capture || gs->captured.IsEmpty())
	{
		gs->capture = false;
		return -1;
	}
	const auto& bmp = gs->captured;
	return WritePng(path, bmp, bmp.GetWidth(), bmp.GetHeight(), true) ? 0 : -1;
}

extern "C" int ps2gfx_load_state(const void* zip, size_t size)
{
	Framework::CPtrStream stream(zip, size);
	Framework::CZipArchiveReader archive(stream);
	g->vu1.m_executor->ClearActiveBlocksInRange(0, PS2::MICROMEM1SIZE, false);
	archive.BeginReadFile("vu1")->Read(&g->vu1.m_State, sizeof(MIPSSTATE));
	archive.BeginReadFile("vumem1")->Read(g->vuMem1, PS2::VUMEM1SIZE);
	archive.BeginReadFile("micromem1")->Read(g->microMem1, PS2::MICROMEM1SIZE);
	g->vpu1->LoadState(archive);
	g->gif.LoadState(archive);
	g->gs->LoadState(archive);
	return 0;
}

extern "C" void ps2gfx_set_vu1_enabled(int enabled)
{
	g->vu1Enabled = enabled != 0;
}
