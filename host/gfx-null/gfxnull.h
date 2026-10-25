#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The DMA controller's channel registers, at 0x10008000 and up. */
enum {
    kHostDmaChcr = 0x00,
    kHostDmaMadr = 0x10,
    kHostDmaQwc = 0x20,
    kHostDmaTadr = 0x30,
    kHostDmaStart = 0x100,
};

/** The GS status register (CSR) bits the host sets. */
#define HOST_GS_CSR ((volatile unsigned long long *)(uintptr_t)0x12001000U)
#define HOST_GS_CSR_VSYNC 0x8ULL
#define HOST_GS_CSR_FIELD 0x2000ULL

/**
 * Run one DMA transfer to the end, as the hardware would. Called with the channel registers
 * already written (STR set) and before the start bit is cleared.
 *
 * @param nChannel 0 for VIF0, 1 for VIF1, 2 for GIF.
 * @param nMadr The source address register. Bit 31 selects the scratchpad.
 * @param nQwc The quadword count register.
 * @param nTadr The tag address register of a chain transfer.
 * @param nChcr The channel control register, whose MOD bits (2-3) select normal or chain mode.
 */
void HostGfxDma(int nChannel, uint32_t nMadr, uint32_t nQwc, uint32_t nTadr, uint32_t nChcr);

/**
 * Read nBytes of the GS-to-host image transfer just started (TRXDIR = 1) into pDest, as the EE
 * reads them from the VIF1 FIFO. Without a renderer pDest keeps its contents.
 */
void HostGfxReadImage(void *pDest, unsigned nBytes);

/**
 * Present the frame. Called once each vertical blank, from the vertical blank thread, before the
 * game's blank handlers run and before VSync() returns.
 */
void HostGfxVsync(void);

/** Return once the frame swapped at frame-loop tick nTick has been presented (the renderer's own
 * pace), keeping the game's timers going meanwhile. */
void HostGfxWaitPresented(unsigned nTick);

/** The vertical blanks so far, at 59.94 Hz. The first call starts the count. */
unsigned HostVblankCount(void);

/** Wait for the next vertical blank, and return the field it began (the CSR FIELD bit). */
int HostWaitVblank(void);

/** The frame loop's blanks so far: the field blanks, or twice as many with FREQ_FPS=120. */
unsigned HostFrameTick(void);

/** Wait until HostFrameTick() reaches nTarget (the game's swap), and return the tick. */
unsigned HostWaitFrameTick(unsigned nTarget);

/** The frame loop's blanks in one field: 1, or 2 with FREQ_FPS=120. */
unsigned HostTicksPerField(void);

/**
 * The fields between the last two swaps: 1 at 60 Hz (always, as the game counts frames), the
 * ticks over 2 at 120 Hz. Frame code that steps by the frame scales its step by it.
 */
float HostFrameFields(void);

/** Whether the last swap began a field (always at 60 Hz). Frame code that draws random numbers or
 *  counts whole frames does so only then, so it runs as at 60 Hz. */
int HostFrameEndsField(void);

/** The fixed clock's time in nanoseconds (FREQ_CLOCK=fixed), or -1. */
long long HostFixedClockNs(void);

/** Print "EVENT <HostFrameTick()> <fixed clock ns> <text>" when FREQ_EVENTLOG is set. */
void HostEvent(const char *pszFormat, ...);
/**
 * Whether FREQ_WIDESCREEN=1 asks for a 16:9 picture. The game's cameras then see 4/3 wider, and
 * the 640-pixel frame is presented stretched to 16:9.
 */
int HostWidescreen(void);

/** FREQ_HUD_EDGES=1 (with FREQ_WIDESCREEN=1): the head-up display's side parts sit at the screen
 *  edges rather than in the centre 4:3 box. */
int HostHudEdges(void);

/** FREQ_TRAILS: strength of the motion trails (the text trails and the feedback blur), 0 to 1;
 *  1, the default, is the original. */
float HostTrails(void);

/** FREQ_GEM_PALETTE=cb: the gems of the five player colours (and their flash particles) take a
 *  palette that stays distinct under deuteranopia and protanopia (src/app/tnlutil.h). */
int HostGemPalette(void);

/** Test hook FREQ_TEST_GEMCOLOR=<0..4>: every plain gem takes that colour index (null, green, red,
 *  yellow, purple) instead of nColor. */
int HostTestGemColor(int nColor);

#ifdef __cplusplus
}
#endif
