#!/usr/bin/env python3
"""Write the .vudata section of a dvp-as object (a DMA chain with its microprogram) to a file.

The native build has no DVP assembler, so it embeds the chains of the Emotion Engine build.
Regenerate both after a change to src/gfx/*.vsm:

    python3 -I host/core/extract-vudata.py build/src/gfx/vu0microcode.vsm.o \
        host/core/vu0microcode.bin
    python3 -I host/core/extract-vudata.py build/src/gfx/vu1microcode.vsm.o \
        host/core/vu1microcode.bin
"""
import struct
import sys


def main() -> None:
    data = open(sys.argv[1], 'rb').read()
    assert data[:6] == b'\x7fELF\x01\x01', 'expected a little-endian ELF32 object'
    shoff, = struct.unpack_from('<I', data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2e)
    headers = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx][4]
    for name, _, _, _, offset, size, *_ in headers:
        if data[names + name:data.index(b'\0', names + name)] == b'.vudata':
            open(sys.argv[2], 'wb').write(data[offset:offset + size])
            return
    sys.exit('no .vudata section')


if __name__ == '__main__':
    main()
