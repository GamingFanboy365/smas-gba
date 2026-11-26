cmake_minimum_required(VERSION 3.20)
project(smas_gba C ASM)

# The engine: the GBA port with no game data. tools/make_cartridge.py
# appends the games from the player's own Super Mario All-Stars ROM.
set(SMAS_TRACE OFF CACHE BOOL "Log the joypad and a RAM hash every frame")
set(SMAS_AUDIO ON CACHE BOOL "Build the sound driver and mixer")

set(GAME_SRCS
  game/smb1/smb1_00.c game/smb1/smb1_01.c game/smb1/smb1_02.c game/smb1/smb1_08.c
  game/smbll/smbll_00.c game/smbll/smbll_01.c game/smbll/smbll_02.c
  game/smbll/smbll_08.c game/smbll/smbll_0b.c)
set(GBA_SRCS
  gba/crt0.S gba/aeabi.S
  gba/libc/string.iwram.c gba/libc/misc.c
  gba/main.c gba/irq.c gba/game.c gba/cart.c gba/rtl.c gba/snes_hw.iwram.c
  gba/render.iwram.c)
if(SMAS_AUDIO)
  list(APPEND GBA_SRCS gba/audio_stub.c)
else()
  list(APPEND GBA_SRCS gba/audio_stub.c)
endif()

add_executable(smas.elf ${GBA_SRCS} ${GAME_SRCS})
target_include_directories(smas.elf PRIVATE gba/libc/include)
target_compile_definitions(smas.elf PRIVATE SMAS_GBA=1)
if(SMAS_TRACE)
  target_compile_definitions(smas.elf PRIVATE SMAS_TRACE=1)
endif()
target_compile_options(smas.elf PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-O2 -std=gnu11 -fno-strict-aliasing -ffunction-sections -fdata-sections
  -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unknown-pragmas
  -Wno-parentheses -Wno-unused-function -Wno-missing-braces -Wno-pointer-sign
  -Wno-char-subscripts -Wno-unused-value -Wno-misleading-indentation>)

# Thumb for everything in ROM, ARM for the hot code in IWRAM (*.iwram.c).
foreach(src ${GBA_SRCS} ${GAME_SRCS})
  if(src MATCHES "\\.iwram\\.c$")
    set_source_files_properties(${src} PROPERTIES COMPILE_OPTIONS "-marm")
  elseif(src MATCHES "\\.c$")
    set_source_files_properties(${src} PROPERTIES COMPILE_OPTIONS "-mthumb")
  endif()
endforeach()

target_link_options(smas.elf PRIVATE -T ${CMAKE_SOURCE_DIR}/gba/gba.ld
  -Wl,--gc-sections -Wl,-Map=${CMAKE_BINARY_DIR}/smas.map)
set_target_properties(smas.elf PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/gba/gba.ld)

add_custom_command(TARGET smas.elf POST_BUILD
  COMMAND ${GBA_OBJCOPY} -O binary smas.elf smas-engine.gba
  COMMENT "Making smas-engine.gba")
