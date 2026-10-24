#pragma once

#include <stdint.h>

/**
 * The native build's copy of the Emotion Engine address map.
 *
 * The game keeps 28 bits of an address for DMA (ToDmaAddress() in src/gfx/gfxdevice.cpp), reaches
 * main memory uncached through the alias 0x20000000 above it, and draws into the scratchpad at
 * 0x70000000. Its static data, its heap, and its stacks therefore sit below kHostDmaLimit, and each
 * is mapped a second time at kHostUncachedAlias above itself, so that a host pointer is its own
 * Emotion Engine address.
 */

/** Every address the game can hand to DMA is below this limit. */
constexpr uintptr_t kHostDmaLimit = 0x10000000;

/** The distance of the uncached view of main memory from the memory. */
constexpr uintptr_t kHostUncachedAlias = 0x20000000;

/** The heap behind malloc() and operator new, from its base to kHostDmaLimit. */
constexpr uintptr_t kHostHeapBase = 0x08000000;

/** The scratchpad, its size, and the bit a DMA address selects it with. */
constexpr uintptr_t kHostScratchpad = 0x70000000;
constexpr uintptr_t kHostScratchpadSize = 0x4000;
constexpr uintptr_t kHostScratchpadDmaBit = 0x80000000;

/** The stack of the thread that runs the game's main(), _stack in the static data. */
#define HOST_GAME_STACK_SIZE 0x400000
