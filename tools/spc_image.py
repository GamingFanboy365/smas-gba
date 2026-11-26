"""The sound RAM a game's SPC700 holds after its uploads at reset.

The game uploads its sound driver, samples and music to the SPC700 once, at
reset (UploadSPCEngine, UploadMainSampleData, UploadSpuData), and never
again, so the 64 KB image is made here, from the ROM, instead of on the GBA.
"""
import struct

# SNES addresses of the upload streams, in the order the game sends them.
UPLOADS = {
    'SMB1': [0x09d000, 0x0a8000, 0x08a146],
    'SMBL': [0x09d000, 0x0a8000, 0x08a043],
}


def rom_offset(addr):
    return ((addr >> 16) << 15) | (addr & 0x7fff)


def build(rom, tag):
    ram = bytearray(0x10000)
    for addr in UPLOADS[tag]:
        p = rom_offset(addr)
        while True:
            n, target = struct.unpack_from('<HH', rom, p)
            p += 4
            if n == 0:
                break
            for i in range(n):
                ram[(target + i) & 0xffff] = rom[p + i]
            p += n
    return bytes(ram)
