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


def build_assets(roms):
    import spc_image
    assets = []
    for tag, _, _, spc_tag, pcm_tag in GAMES:
        ram = spc_image.build(roms[tag], tag)
        assets.append((tag, roms[tag]))
        assets.append((spc_tag, ram))
        assets.append((pcm_tag, spc_image.decode_samples(ram)))
    return assets


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
    # The header's complement check.
    chk = (-(sum(out[0xa0:0xbd]) + 0x19)) & 0xff
    out[0xbd] = chk
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--rom', required=True, help='Super Mario All-Stars (USA) .sfc')
    ap.add_argument('--engine', required=True, help='smas-engine.gba from the build')
    ap.add_argument('-o', '--out', required=True, help='the cartridge to write')
    ap.add_argument('--extract', metavar='DIR',
                    help='also write smb1.sfc and smbll.sfc there (for tools/hostrun)')
    args = ap.parse_args()

    with open(args.rom, 'rb') as f:
        smas = f.read()
    if len(smas) % 0x8000 == 512:
        smas = smas[512:]  # a copier header
    if hashlib.sha1(smas).hexdigest() != SMAS_SHA1:
        sys.exit('%s is not Super Mario All-Stars (USA): sha1 %s, expected %s'
                 % (args.rom, hashlib.sha1(smas).hexdigest(), SMAS_SHA1))
    roms = derive_game_roms(smas)
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
