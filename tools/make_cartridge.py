#!/usr/bin/env python3
"""Builds the playable cartridge: the engine plus the games from your ROM.

    python3 tools/make_cartridge.py --rom smas.sfc --engine build/smas-engine.gba -o smas.gba

The ROM is Super Mario All-Stars (USA), the plain version (not the one with
Super Mario World), unheadered, sha1 c05817c5b7df2fbfe631563e0b37237156a8f6b6.
snesrev's code runs SMB1 and The Lost Levels as two 512 KB ROM images it
derives from that ROM with small zstd deltas (tools/data/*.zst, from
snesrev/smw, MIT); this script makes the same images and appends them, with
the assets prepared for the GBA, after the engine:

    engine, padded to 256 bytes
    "SMASDATA", u32 count, count x (u32 tag, u32 offset, u32 size)
    the assets, each 256-byte aligned (offsets are from "SMASDATA")

Needs the zstandard module (pip install zstandard).
"""
import argparse
import hashlib
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SMAS_SHA1 = 'c05817c5b7df2fbfe631563e0b37237156a8f6b6'
GAMES = [
    # tag, delta file, sha1 of the derived image, sound RAM tag, samples tag
    ('SMB1', 'smb1.zst', '4a5278150f3395419d68cb02a42f7c3c62cdf8b4', 'SPC1', 'PCM1'),
    ('SMBL', 'smbll.zst', '493e14812af7a92d0eacf00ba8bb6d3a266302ca', 'SPCL', 'PCML'),
]


def derive_game_roms(smas):
    try:
        import zstandard
    except ImportError:
        sys.exit('make_cartridge.py needs the zstandard module: pip install zstandard')
    dict_data = zstandard.ZstdCompressionDict(smas)
    roms = {}
    for tag, delta, sha1, _, _ in GAMES:
        with open(os.path.join(HERE, 'data', delta), 'rb') as f:
            out = zstandard.ZstdDecompressor(dict_data=dict_data).decompress(f.read())
        if hashlib.sha1(out).hexdigest() != sha1:
            sys.exit('%s: derived image has the wrong sha1' % tag)
        roms[tag] = out
    return roms


# The SNES's controller setting ("B- JUMP" / "A- JUMP") goes: the GBA's A
# is the SNES's B, and B its Y, as the setting's default has them. Its line
# in the player select is blanked and the cursor's items cut from 3 to 2.
# (SNES addresses: the item counts, then the line's two stripe images.)
MENU_PATCHES = {
    'SMB1': (0x089145, (0x088ea6, 0x088ecf)),
    'SMBL': (0x089042, (0x088da6, 0x088dcf)),
}


def rom_offset(addr):
    return ((addr >> 16) << 15) | (addr & 0x7fff)


def patch_menu(tag, rom):
    rom = bytearray(rom)
    count_addr, stripes = MENU_PATCHES[tag]
    p = rom_offset(count_addr)
    if rom[p] != 3:
        sys.exit('%s: unexpected menu item count at %06x' % (tag, count_addr))
    rom[p] = 2
    for addr in stripes:
        p = rom_offset(addr) + 4         # after the stripe's 4-byte header
        while rom[p] != 0xff:
            rom[p], rom[p + 1] = 0x24, 0x00   # the blank the line pads with
            p += 2
    return bytes(rom)


def build_assets(roms):
    import spc_image
    assets = []
    for tag, _, _, spc_tag, pcm_tag in GAMES:
        ram = spc_image.build(roms[tag], tag)
        assets.append((tag, roms[tag]))
        assets.append((spc_tag, ram))
        assets.append((pcm_tag, spc_image.decode_samples(ram)))
    return assets


# The logo a GBA's BIOS checks in the header before it starts a cartridge
# (bytes 4-159): without it real hardware shows its own logo and stops. The
# engine leaves the space blank; this is what gbafix writes there.
HEADER_LOGO = bytes.fromhex(
    '24ffae51699aa2213d84820a84e409ad11248b98c0817f21a352be199309ce20'
    '10464a4af82731ec58c7e83382e3cebf85f4df94ce4b09c194568ac01372a7fc'
    '9f844d73a3ca9a615897a327fc039876231dc7610304ae56bf38840040a70efd'
    'ff52fe036f9530f197fbc08560d68025a963be03014e38e2f9a234ffbb3e0344'
    '780090cb88113a9465c07c6387f03cafd625e48b380aac7221d4f807')


def pack(engine, assets):
    out = bytearray(engine)
    out += b'\0' * (-len(out) % 256)
    dir_start = len(out)
    header_size = 12 + 12 * len(assets)
    pos = (header_size + 255) & ~255
    entries, blobs = [], []
    for tag, data in assets:
        entries.append(struct.pack('<4sII', tag.encode(), pos, len(data)))
        blobs.append((pos, data))
        pos = (pos + len(data) + 255) & ~255
    block = bytearray(pos)
    block[0:12] = b'SMASDATA' + struct.pack('<I', len(assets))
    block[12:12 + 12 * len(assets)] = b''.join(entries)
    for p, data in blobs:
        block[p:p + len(data)] = data
    out += block
    if len(out) > 32 << 20:
        sys.exit('cartridge is over 32 MB')
    # The header: the logo and the complement check.
    assert len(HEADER_LOGO) == 156
    out[4:160] = HEADER_LOGO
    chk = (-(sum(out[0xa0:0xbd]) + 0x19)) & 0xff
    out[0xbd] = chk
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--rom', required=True, help='Super Mario All-Stars (USA) .sfc')
    ap.add_argument('--engine', required=True, help='smas-engine.gba from the build')
    ap.add_argument('-o', '--out', required=True, help='the cartridge to write')
    ap.add_argument('--extract', metavar='DIR',
                    help='also write smb1.sfc and smbll.sfc there, as the cartridge has '
                         'them (for tools/hostrun)')
    args = ap.parse_args()

    with open(args.rom, 'rb') as f:
        smas = f.read()
    if len(smas) % 0x8000 == 512:
        smas = smas[512:]  # a copier header
    if hashlib.sha1(smas).hexdigest() != SMAS_SHA1:
        sys.exit('%s is not Super Mario All-Stars (USA): sha1 %s, expected %s'
                 % (args.rom, hashlib.sha1(smas).hexdigest(), SMAS_SHA1))
    roms = derive_game_roms(smas)
    roms = {tag: patch_menu(tag, rom) for tag, rom in roms.items()}
    if args.extract:
        os.makedirs(args.extract, exist_ok=True)
        for tag, name in (('SMB1', 'smb1.sfc'), ('SMBL', 'smbll.sfc')):
            with open(os.path.join(args.extract, name), 'wb') as f:
                f.write(roms[tag])
    with open(args.engine, 'rb') as f:
        engine = f.read()
    cart = pack(engine, build_assets(roms))
    with open(args.out, 'wb') as f:
        f.write(cart)
    print('wrote %s (%d KB)' % (args.out, len(cart) >> 10))


if __name__ == '__main__':
    main()
