#pragma once

/** Run the enabled handlers of an interrupt controller cause (INTC_VBLANK_S and the like). */
void HostRaiseIntc(int nCause);

/** Run the enabled handlers of a DMA channel's completion interrupt. */
void HostRaiseDmac(int nChannel);
