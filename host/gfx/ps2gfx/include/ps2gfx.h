/* libps2gfx: PS2 VIF1 (DMA ch1) + VU1 + GIF (DMA ch2) + GS, built from Play! (BSD-2, see LICENSE.Play.txt).
 *
 * Call every function from one thread. The GS runs on an internal thread (Play!'s GS thread); with the
 * OpenGL renderer that thread owns the GL context: config.make_current runs there once at init and
 * config.present runs there after each frame. Do not make the same context current elsewhere.
 * All PS2 addresses are 32-bit. Bit 31 set on a DMA source address means scratchpad (SPR) on the PS2.
 */
#ifndef PS2GFX_H
#define PS2GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PS2GFX_RENDERER_OPENGL = 0,
	PS2GFX_RENDERER_VULKAN = 1, /* offscreen (no present); needs a Vulkan ICD */
	PS2GFX_RENDERER_NULL = 2,   /* draws nothing: tests and headless runs */
};

typedef struct ps2gfx_config
{
	/* Reads n bytes of PS2 memory at addr (a DMA tag or data address, bit 31 = SPR).
	   NULL: addr is a host pointer below 4 GiB, and SPR addresses resolve to spr_base + (addr & 0x3FFF). */
	void (*mem_read)(void* user, uint32_t addr, void* dst, size_t n);
	void* spr_base;
	void* user;
	int renderer;
	/* OpenGL: makes an OpenGL 3.2+ core context current on the calling (GS) thread. Required for OpenGL. */
	void (*make_current)(void* user);
	/* Called on the GS thread after a vsync drew the frame to GL framebuffer 0 (e.g. SDL_GL_SwapWindow).
	   May be NULL. */
	void (*present)(void* user);
	int window_width, window_height; /* presentation size for the default framebuffer, 0 = 640x448 */
	/* The GS draws at res_factor times its resolution (0 or 1 = native; Vulkan: at most 8). */
	int res_factor;
	/* Shape of the presented picture inside the window: 0 = the CRT's 640x448, 1 = 4:3, 2 = 16:9. */
	int aspect;
} ps2gfx_config;

enum
{
	PS2GFX_ASPECT_CRT = 0,
	PS2GFX_ASPECT_4_3 = 1,
	PS2GFX_ASPECT_16_9 = 2,
};

/* Returns 0 on success. */
int ps2gfx_init(const ps2gfx_config* config);
void ps2gfx_shutdown(void);

/* Runs a DMA transfer to completion, VU1 microprograms included.
   channel 1 = VIF1, 2 = GIF. madr/qwc/tadr are the D1/D2 registers, chcr the CHCR value the game would
   write (MOD bits 2-3: 0 normal, 1 source chain; ASP 4-5; TTE bit 6; TIE bit 7; STR is implied).
   Returns 0, or -1 if the transfer stalled (logged to stderr). */
int ps2gfx_dma(int channel, uint32_t madr, uint32_t qwc, uint32_t tadr, uint32_t chcr);

/* GS privileged registers (0x12000000 PMODE ... 0x12001080 SIGLBLID), 64-bit. */
void ps2gfx_priv_write(uint32_t addr, uint64_t value);
void ps2gfx_priv_write32(uint32_t addr, uint32_t value); /* one half, as the EE's sw does (addr & 4 = high) */
uint64_t ps2gfx_priv_read(uint32_t addr);
void ps2gfx_set_csr(uint64_t value);
void ps2gfx_set_imr(uint64_t value);
/* SetGsCrt: interlaced 0/1, mode (2 = NTSC, 3 = PAL), frame_mode 0 = field, 1 = frame. */
void ps2gfx_set_crt(int interlaced, int mode, int frame_mode);

/* EE-side 32-bit register writes/reads: GIF regs 0x10003000.., VIF1 regs 0x10003C00.., VIF1 FIFO 0x10005000,
   GIF FIFO 0x10006000, VU1 FBRST 0x1000FFC0, CMSAR1 0x1000FFC4 (starts a VU1 microprogram). */
void ps2gfx_io_write(uint32_t addr, uint32_t value);
uint32_t ps2gfx_io_read(uint32_t addr);

/* Reads n bytes of the local-to-host transfer (TRXDIR = 1) a GIF transfer just started into dst, as the
   EE reads them from the VIF1 FIFO. */
void ps2gfx_read_image(void* dst, size_t n);

/* Vertical blank: flushes the GS, presents (config.present), sets CSR VSYNC and toggles CSR FIELD. */
void ps2gfx_vsync(void);

/* Waits until the GS thread has drawn every frame ps2gfx_vsync() queued. ps2gfx_vsync() keeps one
   frame in flight and otherwise waits for the GS itself; called first, without the lock that
   serialises the other calls, it lets ps2gfx_dma() go on while the GS draws. The one call that may
   overlap any other. */
void ps2gfx_wait_frames(void);

/* Writes the displayed framebuffer (DISPFB of the active read circuit, cropped to DISPLAY) as PNG,
   at res_factor times its size with Vulkan (OpenGL: native size). Returns 0 on success, -1 if
   nothing is displayed. */
int ps2gfx_screenshot(const char* path);
/* OpenGL: presents the displayed framebuffer again and writes the whole window, at the window's
   size, as PNG. Call after ps2gfx_vsync. Returns 0 on success. */
int ps2gfx_screenshot_output(const char* path);

/* Debug/replay: loads a Play! state zip (VU1 + VIF1 + GIF + GS) as written by the trace capture. */
int ps2gfx_load_state(const void* zip, size_t size);
/* Debug: 0 disables VU1 microprogram execution (negative control). */
void ps2gfx_set_vu1_enabled(int enabled);
/* HD textures (FREQ_TEX_DUMP / FREQ_TEX_REPLACE, see src/TexReplace.h): the game drew these texels at
   run time, so a texture with them is never dumped or replaced. psm is the GS format of the upload,
   pixels its rows, stride bytes apart. Any thread. */
void ps2gfx_tex_generated(const char* name, int psm, int width, int height, const void* pixels, int stride);

#ifdef __cplusplus
}
#endif

#endif
