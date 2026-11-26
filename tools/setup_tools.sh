#!/bin/bash
# Fetches and builds the test tools' upstream parts into third_party/:
# snesrev/smw (the SNES emulator and PPU behind tools/hostrun) and mGBA
# (libmgba, behind tools/mgba/smrun), at the commits this port was checked
# with, then builds hostrun and smrun.
#   tools/setup_tools.sh
# Needs git, cmake, ninja, a C compiler, libpng and zlib (dev packages).
set -e
cd "$(dirname "$0")/.."
SMW_COMMIT=eae20c65c58930c8b62c76188d259579ad4130f1
MGBA_COMMIT=c4fe856f2b33d116b8f54267359d4c81653759f6
mkdir -p third_party

if [ ! -d third_party/smw ]; then
  git clone https://github.com/snesrev/smw.git third_party/smw
fi
git -C third_party/smw checkout -q $SMW_COMMIT

if [ ! -d third_party/mgba ]; then
  git clone https://github.com/mgba-emu/mgba.git third_party/mgba
fi
git -C third_party/mgba checkout -q $MGBA_COMMIT
cmake -S third_party/mgba -B third_party/mgba/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_QT=OFF -DBUILD_SDL=OFF -DBUILD_LIBRETRO=OFF -DBUILD_TEST=OFF -DBUILD_SUITE=OFF \
  -DBUILD_CINEMA=OFF -DBUILD_EXAMPLE=OFF -DBUILD_PYTHON=OFF -DBUILD_PERF=OFF \
  -DBUILD_HEADLESS=OFF -DUSE_FFMPEG=OFF -DUSE_LIBZIP=OFF -DUSE_MINIZIP=OFF -DUSE_SQLITE3=OFF \
  -DUSE_ELF=OFF -DUSE_LUA=OFF -DUSE_EDITLINE=OFF -DUSE_DISCORD_RPC=OFF -DUSE_EPOXY=OFF \
  -DUSE_GDB_STUB=OFF -DUSE_DEBUGGERS=ON -DENABLE_SCRIPTING=OFF -DM_CORE_GB=OFF \
  -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_GL=OFF -DBUILD_GLES2=OFF -DBUILD_GLES3=OFF
ninja -C third_party/mgba/build

make -C tools/hostrun SMW=../../third_party/smw
make -C tools/mgba MGBA=../../third_party/mgba
echo "built tools/hostrun/hostrun and tools/mgba/smrun"
