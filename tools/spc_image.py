"""The sound RAM and samples a game's SPC700 holds, made from its ROM.

The game uploads its sound driver, samples and music to the SPC700 once, at
reset (UploadSPCEngine, UploadMainSampleData, UploadSpuData), and never
again, so the 64 KB image is made here instead of on the GBA, where the
decompiled driver (game/smb1/smb1_spc_player.c) reads it straight from the
cartridge. Its few writes to sound RAM are made here too (see build()).

The samples are BRR, decoded to 16-bit PCM here, so the GBA's mixer only
has to play them (decode_samples()).
"""
import struct

# SNES addresses of the upload streams, in the order the game sends them.
UPLOADS = {
    'SMB1': [0x09d000, 0x0a8000, 0x08a146],
    'SMBL': [0x09d000, 0x0a8000, 0x08a043],
}
DIR = 0x3c00            # the sample directory (DSP DIR register = 0x3c)
NUM_SAMPLES = 64        # entries 64+ of the directory overlap the instruments
NOISE_SRCN = 0x20       # the driver writes random BRR blocks there (SomeRandomStuff)


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
    # What the driver does to its RAM at reset (Spc_Reset):
    # memset(ram, 0, 0x500) and SomeRandomStuff_Init.
    ram[0:0x500] = bytes(0x500)
    for i in range(0x1b):
        ram[0xfe00 + i * 9] = 0x96
    ram[0xfe00 + 0x1b * 9] = 0x97
    ram[0x3c80:0x3c84] = bytes([0, 0xfe, 0, 0xfe])
    return bytes(ram)


def _decode_brr(ram, addr, old=0, older=0, max_blocks=4096):
    """Decodes BRR blocks from addr until the end flag, as the DSP does.

    Returns (samples, end_flags, (old, older), addresses of the blocks)."""
    out, blocks = [], []
    flags = 0
    for _ in range(max_blocks):
        header = ram[addr & 0xffff]
        blocks.append(addr & 0xffff)
        shift, filt, flags = header >> 4, (header >> 2) & 3, header & 3
        for i in range(16):
            byte = ram[(addr + 1 + (i >> 1)) & 0xffff]
            s = (byte >> 4) if not (i & 1) else (byte & 15)
            if s > 7:
                s -= 16
            if shift <= 12:
                s = (s << shift) >> 1
            else:
                s = (s >> 3) << 12
            if filt == 1:
                s += old + (-old >> 4)
            elif filt == 2:
                s += 2 * old + ((3 * -old) >> 5) - older + (older >> 4)
            elif filt == 3:
                s += 2 * old + ((13 * -old) >> 6) - older + ((3 * older) >> 4)
            s = max(-0x8000, min(0x7fff, s))
            s = s & 0x7fff
            s = (s ^ 0x4000) - 0x4000      # clip to 15 bits, signed
            older, old = old, s
            out.append(s)
        addr += 9
        if flags & 1:
            break
    return out, flags, (old, older), blocks


def decode_samples(ram):
    """The PCM asset: a table of NUM_SAMPLES entries (offset, length, loop),
    then the samples, 16-bit. offset is in bytes from the asset's start;
    loop is the index playback goes back to at the end, 0xffffffff for a
    sample that ends, 0xfffffffe for the noise source. The samples are the
    DSP's 15-bit outputs; two more after the end, for interpolation."""
    entries, data = [], bytearray()
    header_size = 4 + 12 * NUM_SAMPLES
    cache = {}
    for srcn in range(NUM_SAMPLES):
        start, loop = struct.unpack_from('<HH', ram, DIR + srcn * 4)
        if srcn == NOISE_SRCN:
            entries.append((header_size, 0, 0xfffffffe))
            continue
        if start < 0x500 or start >= 0xfe00:
            entries.append((header_size, 0, 0xffffffff))  # not a sample
            continue
        key = (start, loop)
        if key not in cache:
            pcm, flags, hist, blocks = _decode_brr(ram, start)
            loop_idx = 0xffffffff
            if flags == 3:
                # Play the loop once more with the history the end leaves,
                # and loop over that copy, so the loop joins up as on the DSP.
                again, _, _, _ = _decode_brr(ram, loop, *hist)
                loop_idx = len(pcm)
                pcm = pcm + again
            tail = pcm[loop_idx:loop_idx + 2] if loop_idx != 0xffffffff else [0, 0]
            if len(tail) < 2:
                tail = (tail + [0, 0])[:2]
            offset = header_size + len(data)
            data += struct.pack('<%dh' % (len(pcm) + 2), *(pcm + tail))
            data += b'\0' * (-len(data) % 4)
            cache[key] = (offset, len(pcm), loop_idx)
        entries.append(cache[key])
    out = struct.pack('<I', NUM_SAMPLES)
    out += b''.join(struct.pack('<III', *e) for e in entries)
    return out + bytes(data)
