@ The mixer's inner loops: one voice, one chunk, added into an interleaved
@ L, R buffer of 32-bit sums. ARM code in IWRAM.
@
@ int MixNearest(MixJob *job, int32_t *mix, int count)
@ int MixLinear(MixJob *job, int32_t *mix, int count)
@   job: { const int16_t *pcm; uint32_t pos, step, end, loop_back; int32_t gl, gr; }
@   pos, step, end and loop_back are 16.16 sample positions. At the end the
@   position goes back by loop_back, or, for a sample that ends (loop_back
@   0), the loop stops and returns 0. Otherwise returns 1. job->pos is
@   written back.

  .syntax unified
  .section .iwram.mixer, "ax", %progbits
  .arm
  .align 2

  .global MixNearest
  .type MixNearest, %function
MixNearest:
  stmfd sp!, {r4-r11, lr}
  ldmia r0, {r3, r4, r5, r6, r7, r8, r12}  @ pcm pos step end loop_back gl gr
1:
  mov   r9, r4, lsr #16
  add   r9, r3, r9, lsl #1
  ldrsh r9, [r9]
  ldmia r1, {r10, r11}
  mla   r10, r9, r8, r10
  mla   r11, r9, r12, r11
  stmia r1!, {r10, r11}
  add   r4, r4, r5
  cmp   r4, r6
  bhs   3f
2:
  subs  r2, r2, #1
  bne   1b
  str   r4, [r0, #4]
  mov   r0, #1
  ldmfd sp!, {r4-r11, lr}
  bx    lr
3:
  cmp   r7, #0
  beq   4f
5:
  sub   r4, r4, r7
  cmp   r4, r6
  bhs   5b
  b     2b
4:
  str   r4, [r0, #4]
  mov   r0, #0
  ldmfd sp!, {r4-r11, lr}
  bx    lr

  .global MixLinear
  .type MixLinear, %function
MixLinear:
  stmfd sp!, {r4-r11, lr}
  ldmia r0, {r3, r4, r5, r6, r7, r8, r12}
  str   r0, [sp, #-4]!
1:
  mov   r9, r4, lsr #16
  add   r9, r3, r9, lsl #1
  ldrsh r10, [r9, #2]
  ldrsh r9, [r9]
  mov   r11, r4, lsl #16
  sub   r10, r10, r9
  mov   r11, r11, lsr #17            @ the fraction, 15 bits
  mul   r0, r10, r11
  add   r9, r9, r0, asr #15          @ s0 + (s1 - s0) * frac
  ldmia r1, {r10, r11}
  mla   r10, r9, r8, r10
  mla   r11, r9, r12, r11
  stmia r1!, {r10, r11}
  add   r4, r4, r5
  cmp   r4, r6
  bhs   3f
2:
  subs  r2, r2, #1
  bne   1b
  ldr   r0, [sp], #4
  str   r4, [r0, #4]
  mov   r0, #1
  ldmfd sp!, {r4-r11, lr}
  bx    lr
3:
  cmp   r7, #0
  beq   4f
5:
  sub   r4, r4, r7
  cmp   r4, r6
  bhs   5b
  b     2b
4:
  ldr   r0, [sp], #4
  str   r4, [r0, #4]
  mov   r0, #0
  ldmfd sp!, {r4-r11, lr}
  bx    lr
