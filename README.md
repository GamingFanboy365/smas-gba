# Super Mario All-Stars for the Game Boy Advance

A Game Boy Advance port of the two games of *Super Mario All-Stars* (SNES,
1993) that exist as C code: **Super Mario Bros.** and **Super Mario Bros.:
The Lost Levels**, with the All-Stars graphics, music and save files, at
the GBA's native 240×160.

It isn't an emulator. snesrev's reverse-engineered C version of the two
games ([snesrev/smw](https://github.com/snesrev/smw), which checks itself
against the original game frame by frame) is compiled for the GBA's ARM7TDMI
unchanged but for a few lines behind `SMAS_GBA`. Everything the games did
through the SNES's hardware (video, sound, joypad, saves) is reimplemented
for the GBA's in `gba/`. The approach follows the porting guide that came
with the Super Metroid GBA port.

The cartridge contains no game data. You build the engine, then
`tools/make_cartridge.py` adds the two games from your own copy of the
All-Stars ROM.

*Super Mario Bros. 2* and *3* from All-Stars aren't included: there is no C
version of them to port, and the GBA is far too slow to emulate a SNES.

## Building

You need clang and lld (LLVM 16 or later; no GCC or devkitARM), CMake,
Ninja and Python 3 with the `zstandard` module (`pip install zstandard`).

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gba-toolchain.cmake
```

```bash
ninja -C build
```

That makes `build/smas-engine.gba`, the engine alone. Then, with your ROM,
*Super Mario All-Stars* (USA), the plain one rather than the one with *Super
Mario World*, unheadered or headered, sha1
`c05817c5b7df2fbfe631563e0b37237156a8f6b6` without its header:

```bash
python3 tools/make_cartridge.py --rom smas.sfc --engine build/smas-engine.gba -o smas.gba
```

`smas.gba` runs in any GBA emulator (mGBA is what it's tested in). For a
flash cartridge on real hardware, fix up the header with `gbafix smas.gba`
first: the build leaves the BIOS's logo area blank.

## Playing

A game select comes first: Up, Down or Select to choose, A or Start to
play. The last choice is remembered.

| GBA | In the games |
|---|---|
| D-pad | move, menus |
| A | jump (the SNES's B) |
| B | run, fireballs (the SNES's Y) |
| Start | pause menu: continue, save and continue, save and quit |
| Select | menus |
| A + B + Select + Start | save and go back to the game select |

The SNES games' controller setting ("B- JUMP") is gone from the player
select: the GBA's buttons are already its default.

Saves go in the cartridge's SRAM, 8 KB per game, as on the SNES.

## How it fits on a GBA

**The picture.** The SNES draws 256×224, the GBA 240×160. In the levels the
art is shown 1:1, as the *Super Mario Advance* games do: 240 of the 256
columns, the HUD drawn over the top of the level instead of taking a strip
of the screen, and a camera that rests at the bottom (the ground always in
view) and rises only when Mario goes high, easing as it moves. The title
screen and the screens between levels are squashed to fit instead. The
title drops the HUD and keeps every row of its text, taking the 32 rows it
must lose evenly from everything else; the intro card keeps the HUD and
drops each text row's top and bottom (outline) rows.

**Video.** `gba/snes_hw.iwram.c` models the SNES registers, VRAM, CGRAM and
OAM the game writes, DMA and HDMA, and runs the frame's lines with the
game's own mid-frame IRQ handler, as the reference emulator times them.
`gba/render.iwram.c` converts the tiles and maps that changed into the
GBA's formats, plans the GBA's four layers and their priorities from the
SNES's Mode 1 order each frame (a layer whose tiles of both priorities
matter is shown as two GBA layers), and feeds per-line scrolling and colors
by HBlank DMA. The sky gradients are the backdrop color per line; the water
levels' tint is a solid layer blended additively, which is exact; HDMA
that only scrolls (the water's waves) is read once per frame instead of
line by line.

**Sound.** snesrev's decompiled SPC700 sound driver runs on the GBA's CPU
after each game frame, as on the PC. Its DSP writes go to a mixer in ARM
code in fast RAM (`gba/mixer.iwram.c`, `gba/mixer_asm.S`): eight voices at
13379 Hz, the DSP's envelopes, volumes, pitch and noise, played from
samples decoded at cartridge-build time (`tools/spc_image.py`), mixed from
an interrupt so that a slow game frame never starves the sound. There is no
echo.

## Testing

The port was checked against snesrev's PC version throughout, picture by
picture and frame by frame, with two headless runners:

`tools/hostrun` runs the PC version (the same game code, with its SNES PPU,
and optionally the original 65816 code compared every frame), scripted, and
writes pictures, state dumps and sound. `tools/mgba/smrun` runs the
cartridge on libmgba with the same scripts, timed by game frame so that its
pictures are the same frames, and profiles by function. Both can poke the
game's RAM to start in any level. `tools/setup_tools.sh` fetches the
upstream parts they need and builds them.

```bash
tools/setup_tools.sh
```

```bash
python3 tools/make_cartridge.py --rom smas.sfc --engine build/smas-engine.gba -o smas.gba --extract roms
```

```bash
tools/hostrun/hostrun roms/smb1.sfc -n 900 -k "-*200 start*2 -*150 start*2 -*150 start*2 -*100 right*300" -s 600,800 -o out
```

```bash
tools/mgba/smrun smas.gba -n 1200 -f $(llvm-nm build/smas.elf | awk '/ g_game_frame$/{print $1}') -K "-*30 a*2 -*10" -k "-*200 start*2 -*150 start*2 -*150 start*2 -*100 right*300" -s 600,800 -o out
```

The scenes checked: the titles and menus of both games, the intro cards,
1-1, 1-2 (underground), 2-2 (underwater), 1-4 (castle), the pause menu,
saving and loading, The Lost Levels' 1-1.

## Status and limits

Both games play from the title through their levels with sound and saves.
The title, 1-1, the underground and the castles run at 60 frames a second
(with a dip while a level loads); the water levels at about 47, the GBA's
CPU being full there. No echo in the sound. Levels and screens beyond those
listed above haven't been checked against the PC version picture by
picture; the renderer is general, but a scene using a SNES effect not seen
in those may need work.

## Credits and licenses

The game code in `game/` is snesrev's
([snesrev/smw](https://github.com/snesrev/smw), MIT; see `game/LICENSE.txt`),
as are the small deltas in `tools/data/` that derive the two games' ROM
images from the All-Stars ROM. *Super Mario All-Stars* is Nintendo's; this
repository contains none of its data.
