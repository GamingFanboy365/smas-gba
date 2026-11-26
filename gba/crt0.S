@ Startup code, cartridge header and the interrupt entry point.
@
@ The header's logo and complement byte are left blank here and filled in by
@ tools/make_cartridge.py, which builds the playable cartridge.

  .syntax unified
  .section .crt0, "ax", %progbits
  .arm
  .global _start
_start:
  b     start_vector
  .fill 156, 1, 0              @ logo, filled in by make_cartridge.py
  .ascii "SMAS GBA    "        @ title, 12 bytes
  .ascii "ZSME"                @ game code
  .ascii "00"                  @ maker code
  .byte 0x96                   @ fixed value
  .byte 0x00                   @ main unit code
  .byte 0x00                   @ device type
  .fill 7, 1, 0                @ reserved
  .byte 0x00                   @ software version
  .byte 0x00                   @ complement check, filled in later
  .byte 0x00, 0x00             @ reserved

  @ The save type, for flash cartridges and emulators that look for it:
  @ 32 KB of battery-backed SRAM.
  .align 2
  .ascii "SRAM_V113"
  .align 2

start_vector:
  @ Interrupts off, whatever ran before (a flash cartridge's menu) left on;
  @ main() resets the video memory and the registers it left.
  mov   r0, #0x04000000
  strb  r0, [r0, #0x208]       @ IME = 0

  @ Stacks for IRQ, supervisor and system mode; we run in system mode.
  mov   r0, #0x12
  msr   cpsr_c, r0
  ldr   sp, =__sp_irq
  mov   r0, #0x13
  msr   cpsr_c, r0
  ldr   sp, =__sp_svc
  mov   r0, #0x1f
  msr   cpsr_c, r0
  ldr   sp, =__sp_usr

  @ Copy the IWRAM and EWRAM load images out of ROM, clear the BSS.
  ldr   r0, =__iwram_start
  ldr   r1, =__iwram_lma
  ldr   r2, =__iwram_end
  bl    copy_words
  ldr   r0, =__ewram_start
  ldr   r1, =__ewram_lma
  ldr   r2, =__ewram_end
  bl    copy_words
  ldr   r0, =__iwram_bss_start
  ldr   r2, =__iwram_bss_end
  bl    zero_words
  ldr   r0, =__bss_start
  ldr   r2, =__bss_end
  bl    zero_words

  @ Install the IRQ entry point the BIOS jumps to.
  ldr   r0, =0x03007ffc
  ldr   r1, =irq_entry
  str   r1, [r0]

  ldr   r0, =main
  mov   lr, pc
  bx    r0
1:
  b     1b

copy_words:                    @ r0 = dst, r1 = src, r2 = dst end
  cmp   r0, r2
  ldrlo r3, [r1], #4
  strlo r3, [r0], #4
  blo   copy_words
  bx    lr

zero_words:                    @ r0 = dst, r2 = dst end
  mov   r3, #0
1:
  cmp   r0, r2
  strlo r3, [r0], #4
  blo   1b
  bx    lr

  .ltorg

@ The BIOS calls this in IRQ mode, ARM state, with r0-r3, r12 and lr saved.
@ It acknowledges the interrupts, tells the BIOS about them (for
@ VBlankIntrWait), then calls IrqDispatch(flags) in system mode, on the
@ system stack, with interrupts still disabled. IrqDispatch may re-enable
@ them itself (see gba/irq.c) for long handlers.
  .section .iwram.irq, "ax", %progbits
  .arm
  .align 2
  .global irq_entry
irq_entry:
  mov   r3, #0x04000000
  ldr   r2, [r3, #0x200]       @ IE in the low half, IF in the high half
  and   r0, r2, r2, lsr #16    @ the enabled ones that are pending
  add   r3, r3, #0x200
  strh  r0, [r3, #2]           @ acknowledge them in IF
  mov   r3, #0x04000000
  ldrh  r2, [r3, #-8]          @ the BIOS's copy at 0x03fffff8
  orr   r2, r2, r0
  strh  r2, [r3, #-8]

  mrs   r2, spsr
  stmfd sp!, {r2, lr}          @ on the IRQ stack
  mov   r2, #0x9f              @ system mode, IRQs off
  msr   cpsr_c, r2
  stmfd sp!, {r4, lr}          @ on the system stack (r4 pads to 8 bytes)
  ldr   r2, =IrqDispatch
  mov   lr, pc
  bx    r2
  ldmfd sp!, {r4, lr}
  mov   r2, #0x92              @ back to IRQ mode, IRQs off
  msr   cpsr_c, r2
  ldmfd sp!, {r2, lr}
  msr   spsr_cxsf, r2
  bx    lr
  .ltorg
