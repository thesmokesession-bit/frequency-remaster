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
 * Present the frame. Called once each vertical blank, from the vertical blank thread, before the
 * game's blank handlers run and before VSync() returns.
 */
void HostGfxVsync(void);

/** The vertical blanks so far, at 59.94 Hz. The first call starts the count. */
unsigned HostVblankCount(void);

/** Wait for the next vertical blank, and return the field it began (the CSR FIELD bit). */
int HostWaitVblank(void);

/**
 * Whether FREQ_WIDESCREEN=1 asks for a 16:9 picture. The game's cameras then see 4/3 wider, and
 * the 640-pixel frame is presented stretched to 16:9.
 */
int HostWidescreen(void);

#ifdef __cplusplus
}
#endif
