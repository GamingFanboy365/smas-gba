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

You need clang and lld (LLVM 17 or later; it's built and checked with 17,
18, 19 and 20; no GCC or devkitARM), CMake, Ninja and Python 3 with the
`zstandard` module (`pip install zstandard`). To pick a clang that isn't
the one called `clang`, add `-DGBA_CLANG=clang-20` (for example) to the
first command.

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

`smas.gba` runs in GBA emulators (it's tested in mGBA and in VBA-M) and is
made for real hardware too: `make_cartridge.py` completes the header (the
logo the BIOS checks, the checksum), so no `gbafix` is needed, the save type
is marked for flash cartridges (`SRAM_V113`, 32 KB), and the startup code
resets whatever a cartridge's menu left in the video and sound hardware. A
player has run it on an EZ-Flash Omega in a GBA SP.

## Playing

A game select comes first: Up, Down or Select to choose (with the cursor
sound of the All-Stars menu), A or Start to play (with the sound of
choosing a file in the games' own file select). The last choice is
remembered.

| GBA | In the games |
|---|---|
| D-pad | move, menus |
| A | jump (the SNES's B); in menus, choose (as Start) |
| B or R | run, fireballs (the SNES's Y) |
| Start | pause menu: continue, save and continue, save and quit |
| Select | menus |
| A + B + Select + Start | save and go back to the game select |

Save and quit, in the pause or Game Over menu, goes back to the game select,
as in All-Stars. A pressed while a menu is up chooses; held down from play
into a menu (dying with it held, say) it stays a jump and chooses nothing.

The SNES games' controller setting ("B- JUMP") is gone from the player
select: the GBA's buttons are already its default.

Saves go in the cartridge's SRAM, 8 KB per game, as on the SNES.

## How it fits on a GBA

**The picture.** The SNES draws 256×224, the GBA 240×160. In the levels the
art is shown 1:1, as the *Super Mario Advance* games do: 240 of the 256
columns, the HUD drawn over the top of the level instead of taking a strip
of the screen, and a camera that rests at the bottom (the ground always in
view) and rises only when Mario goes high, easing as it moves. The clouds
behind the HUD, on the same SNES layer as the HUD, are shown there by
another GBA layer for those lines. Pausing leaves the level where it is, as
on the SNES, and shows the pause menu in the middle of the screen.
Lakitu, who flies above the rows the view shows, is drawn just under the
HUD, so that he and his Spinies can be seen coming. The
ending's text is shown without the HUD, in the middle of the screen, and its
portrait in its circle (the SNES's window, drawn with a GBA window whose
width is set line by line). The title
screen and the screens between levels (the "WORLD 1-1" card, Game Over, Time
Up) drop the HUD instead and show the 192 rows below it on the 160 lines,
choosing the 32 rows that go by what's in them: rows of text are never
touched; the title takes them evenly from everything else; the cards take
empty rows first, their margins before the gaps, so that their text and
pictures aren't squashed at all.

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
an interrupt so that a slow game frame never starves the sound. Envelopes
are stepped every 32 samples, each voice's volume gliding sample by sample
from one step to the next so that notes fade out without clicks. The
output is the GBA's 8 bits a channel, as loud as the loudest sound effects
allow. There is no echo.

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

`tools/sweep.py` does that for every level of a game, from its start and
from its halfway point: a scripted run on both, a picture every 50 frames
compared (below the HUD, and behind it), and the speed of each second of
play. Its summary lists each run's worst picture and the windows that ran
slow.

```bash
python3 tools/sweep.py --cart smas.gba --elf build/smas.elf --roms roms --out out-sweep --game smb1
```

Besides the sweeps of both games, the scenes checked by eye: the titles and
menus, the cards between levels, Game Over and Time Up, the pause menu,
saving and loading.

## Status and limits

Both games play from the title through their levels with sound and saves,
at 60 frames a second: in the sweeps no second of play in any level, the
water levels included, runs slow. A level's load and the moments after a
death take a few frames longer than on the SNES (the new level's tiles are
converted while the screen is dark).

The sweeps' pictures match the PC version's but for the mosaic effect of
The Lost Levels' pipe transitions, whose blocks line up differently on the
GBA. There is no echo in the sound. Super Mario Bros.' ending and the
castles' Toad scenes were checked against the PC version; worlds reached
only by finishing the games (The Lost Levels' 9 and A to D) and The Lost
Levels' ending haven't been checked picture by picture.

## Credits and licenses

The game code in `game/` is snesrev's
([snesrev/smw](https://github.com/snesrev/smw), MIT; see `game/LICENSE.txt`),
as are the small deltas in `tools/data/` that derive the two games' ROM
images from the All-Stars ROM. *Super Mario All-Stars* is Nintendo's; this
repository contains none of its data.
