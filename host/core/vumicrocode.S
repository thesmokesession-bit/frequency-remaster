// The VU microprogram chains of src/gfx/*.vsm as the Emotion Engine build assembles them, in the
// static data where ToDmaAddress() can address them. host/core/extract-vudata.py writes the files.

    .data
    .balign 16
    .globl g_vu0MicrocodeChain
g_vu0MicrocodeChain:
    .incbin "vu0microcode.bin"

    .balign 16
    .globl g_vu1MicrocodeChain
g_vu1MicrocodeChain:
    .incbin "vu1microcode.bin"

    .section .note.GNU-stack, "", @progbits
