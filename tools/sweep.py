#!/usr/bin/env python3
"""Runs every level on the GBA build and on the PC reference, and compares.

    tools/sweep.py --cart smas.gba --elf build/smas.elf --roms roms --out out-sweep [--game smb1|smbll]
                   [--only 1-1,2-2] [--jobs 4]

For each level, from its start and from its halfway page, the same scripted
run (holding right, jumping now and then, with star power poked in so the
run gets somewhere) goes through tools/hostrun and tools/mgba/smrun, with a
picture every 50 game frames. Each GBA picture is compared with the
reference's 240 middle columns at the vertical offset that fits best, below
the HUD (lines 16-159); squashed screens (the cards between levels) and
blank frames are left out. The summary lists, per run, the worst share of
differing pixels, and the speed: the 60-frame windows that took more than
61 VBlanks once the level is under way, and the heaviest window's cycles a
frame ("slow": long and heavy windows; "loads": long but light ones, a
level's load or a death in them). Side-by-side pictures of each run's worst frame go to OUT/worst/.
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
START = '-*200 start*2 -*150 start*2 -*150 start*2 -*100 '
PLAY = '(right+b*40 right+b+a*22 right*6)x40'
SHOTS = '700-1900/50'
NFRAMES = 1920
# Worlds whose second level starts with a walk into a pipe, an area of its own.
PIPE_WORLDS = {'smb1': (1, 2, 4, 7), 'smbll': ()}
# The games' halfway pages (kHalfwayPageNibbles), a nibble a level.
HALFWAY = {
    'smb1': bytes.fromhex('56406570664066406640666065700000'),
    'smbll': bytes.fromhex('6660886066707760d6007780 70b0'.replace(' ', '')) + bytes(2),
}


def halfway_page(game, world, level):
    b = HALFWAY[game][(world - 1) * 2 + ((level - 1) >> 1)]
    return (b & 15) if (level - 1) & 1 else (b >> 4)


def level_pokes(game, world, level, halfway):
    """world and level as displayed (1-based)."""
    area = level - 1
    if world in PIPE_WORLDS[game] and level >= 2:
        area = level
    pokes = ['190:56=%x' % (world - 1),
             '353:75c=%x,760=%x,75b=%x' % (level - 1, area, halfway)]
    # Star power, so that enemies don't end the run.
    pokes += ['%d:7af=ff' % f for f in range(660, NFRAMES, 200)]
    return pokes


def run_one(args, game, world, level, halfway):
    tag = '%s_%d-%d%s' % (game, world, level, 'h' if halfway else '')
    d = os.path.join(args.out, tag)
    os.makedirs(d, exist_ok=True)
    pokes = level_pokes(game, world, level, halfway)
    script = START + PLAY
    rom = os.path.join(args.roms, 'smb1.sfc' if game == 'smb1' else 'smbll.sfc')
    cmd = [os.path.join(HERE, 'hostrun', 'hostrun'), rom, '-n', str(NFRAMES), '-k', script,
           '-s', SHOTS, '-o', d]
    for p in pokes:
        cmd += ['-w', p]
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    except subprocess.TimeoutExpired:
        return tag, d, ''   # the game itself hangs there, in the original too
    menu = '-*30 a*2 -*10' if game == 'smb1' else '-*30 down*2 -*5 a*2 -*10'
    cmd = [os.path.join(HERE, 'mgba', 'smrun'), args.cart, '-n', str(NFRAMES * 2 + 400),
           '-f', args.frame_addr, '-F', args.shown_addr, '-r', args.ram_addr, '-K', menu, '-k', script, '-s', SHOTS,
           '-o', d, '-q']
    for p in pokes:
        cmd += ['-w', p]
    log = subprocess.run(cmd, capture_output=True, text=True).stdout
    with open(os.path.join(d, 'log.txt'), 'w') as f:
        f.write(log)
    return tag, d, log


def compare(ref_path, gba_path):
    """Returns (share of differing pixels, offset), or None for frames left out."""
    ref = np.asarray(Image.open(ref_path).convert('RGB'), dtype=np.int16)[:, 8:248]
    gba = np.asarray(Image.open(gba_path).convert('RGB'), dtype=np.int16)
    if gba.max() < 16 or ref.max() < 16:
        return None  # blank
    best = None
    part = gba[16:160]
    for off in range(0, 65):
        diff = np.abs(ref[off + 16:off + 160] - part).max(axis=2) > 12
        share = diff.mean()
        if best is None or share < best[0]:
            best = (share, off)
    return best


def analyze(tag, d, log, worst_dir):
    shares = []
    for name in sorted(os.listdir(d)):
        if not name.startswith('f') or not name.endswith('.png'):
            continue
        g = os.path.join(d, 'g' + name[1:])
        if not os.path.exists(g):
            continue
        r = compare(os.path.join(d, name), g)
        if r is None:
            continue
        shares.append((r[0], r[1], name))
    # Squashed screens don't fit at any offset: a card between levels has a
    # mostly black reference. Leave out frames whose reference is that.
    kept = []
    for share, off, name in shares:
        ref = np.asarray(Image.open(os.path.join(d, name)).convert('RGB'))
        if (ref.max(axis=2) < 48).mean() > 0.6:
            continue
        kept.append((share, off, name))
    worst = max(kept) if kept else (0.0, 0, None)
    if worst[2]:
        ref = Image.open(os.path.join(d, worst[2])).convert('RGB')
        gba = Image.open(os.path.join(d, 'g' + worst[2][1:])).convert('RGB')
        sheet = Image.new('RGB', (256 + 240 + 8, 224), (40, 40, 40))
        sheet.paste(ref, (0, 0))
        sheet.paste(gba, (264, 0))
        sheet.save(os.path.join(worst_dir, '%s_%s' % (tag, worst[2])))
    # Speed: the 60-frame windows once the level is under way (the first 14
    # are the title, the menus and the level's card). A window's cycles a
    # frame over the budget (280896, less what waiting costs) is the game
    # running slow; a long window that's light has a load in it.
    windows = [(int(m.group(1)), int(m.group(2)) + int(m.group(3)) + int(m.group(4)))
               for m in re.finditer(r'60 frames in (\d+) vblanks: game (\d+) build (\d+) present (\d+)', log)]
    play = windows[14:]
    slow = sum(1 for v, c in play if v > 61 and c > 262000)
    loads = sum(1 for v, c in play if v > 61 and c <= 262000)
    heavy = max((c for _, c in play), default=0)
    return tag, len(kept), worst[0], worst[2], len(play), slow, loads, heavy


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cart', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('--roms', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--game', default='smb1')
    ap.add_argument('--only', default='')
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    nm = subprocess.run(['llvm-nm', args.elf], capture_output=True, text=True).stdout
    sym = {l.split()[2]: l.split()[0] for l in nm.splitlines() if len(l.split()) == 3}
    args.frame_addr, args.ram_addr = sym['g_game_frame'], sym['g_ram']
    args.shown_addr = sym['g_shown_frame']
    worst_dir = os.path.join(args.out, 'worst')
    os.makedirs(worst_dir, exist_ok=True)
    only = set(filter(None, args.only.split(',')))
    runs = []
    for world in range(1, 9):
        for level in range(1, 5):
            if only and '%d-%d' % (world, level) not in only:
                continue
            runs.append((args.game, world, level, 0))
            if halfway_page(args.game, world, level):
                runs.append((args.game, world, level, halfway_page(args.game, world, level)))
    rows = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = [ex.submit(run_one, args, *r) for r in runs]
        for f in futs:
            tag, d, log = f.result()
            rows.append(analyze(tag, d, log, worst_dir))
    lines = ['%-14s %5s %7s %-12s %7s %5s %5s %8s' % ('run', 'shots', 'worst', 'frame', 'windows', 'slow', 'loads', 'heaviest')]
    for tag, n, share, name, nwin, slow, loads, heavy in rows:
        lines.append('%-14s %5d %6.2f%% %-12s %7d %5d %5d %8d' % (tag, n, share * 100, name or '-', nwin, slow, loads, heavy))
    text = '\n'.join(lines)
    with open(os.path.join(args.out, 'summary.txt'), 'w') as f:
        f.write(text + '\n')
    print(text)


if __name__ == '__main__':
    main()
