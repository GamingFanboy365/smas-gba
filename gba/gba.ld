/* GBA linker script.
 *
 * ROM (0x08000000): startup code, Thumb code, read-only data, and the load
 *   images of the IWRAM and EWRAM sections.
 * IWRAM (0x03000000, 32 KB, 32-bit, no wait states): everything from files
 *   named *.iwram.c (built as ARM), and variables tagged IWRAM_DATA/IWRAM_BSS.
 *   The top of IWRAM holds the stacks and the BIOS's variables.
 * EWRAM (0x02000000, 256 KB, 16-bit, 2 wait states): all other .data and .bss
 *   (the game's 128 KB of RAM, the SNES VRAM copy, ...).
 */
OUTPUT_ARCH(arm)
ENTRY(_start)

MEMORY {
  rom   (rx)  : ORIGIN = 0x08000000, LENGTH = 32M
  iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 32K
  ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
}

/* The system-mode stack grows down from here; IRQ and SVC stacks are above. */
__sp_usr = 0x03007F00;
__sp_irq = 0x03007FA0;
__sp_svc = 0x03007FE0;
__stack_size = 0xA00;

SECTIONS {
  .text : ALIGN(4) {
    KEEP(*(.crt0))
    *(.text.rom .text.rom.*)
    *(EXCLUDE_FILE(*.iwram.c.obj) .text EXCLUDE_FILE(*.iwram.c.obj) .text.* .gnu.linkonce.t.*)
    *(.glue_7 .glue_7t)
    . = ALIGN(4);
  } > rom

  .rodata : ALIGN(4) {
    *(EXCLUDE_FILE(*.iwram.c.obj) .rodata EXCLUDE_FILE(*.iwram.c.obj) .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(4);
  } > rom

  .ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } > rom

  .iwram : ALIGN(4) {
    __iwram_start = .;
    *.iwram.c.obj(.text .text.* .rodata .rodata.* .data .data.*)
    *(.iwram .iwram.*)
    . = ALIGN(4);
    __iwram_end = .;
  } > iwram AT > rom
  __iwram_lma = LOADADDR(.iwram);

  .iwram_bss (NOLOAD) : ALIGN(4) {
    __iwram_bss_start = .;
    *.iwram.c.obj(.bss .bss.* COMMON)
    *(.iwram_bss .iwram_bss.*)
    . = ALIGN(4);
    __iwram_bss_end = .;
  } > iwram

  .ewram : ALIGN(4) {
    __ewram_start = .;
    *(.ewram .ewram.*)
    *(.data .data.* .gnu.linkonce.d.*)
    . = ALIGN(4);
    __ewram_end = .;
  } > ewram AT > rom
  __ewram_lma = LOADADDR(.ewram);

  .bss (NOLOAD) : ALIGN(4) {
    __bss_start = .;
    *(.bss .bss.* .gnu.linkonce.b.* COMMON)
    *(.ewram_bss .ewram_bss.*)
    . = ALIGN(4);
    __bss_end = .;
  } > ewram

  __heap_start = .;
  __heap_end = 0x02040000;
  __rom_end = __ewram_lma + SIZEOF(.ewram);

  /DISCARD/ : { *(.ARM.attributes) *(.comment) *(.note*) }
}

ASSERT(__iwram_bss_end <= __sp_usr - __stack_size, "IWRAM overflows into the stack")
