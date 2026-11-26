@ The ARM EABI runtime helpers the compiler calls: division and 64-bit
@ shifts and multiplies. They're ARM code in IWRAM and return with bx, so
@ both ARM and Thumb code can call them (lld adds the interworking thunks).
@
@ The ARM7TDMI has no divide instruction: division is a shift-and-subtract
@ loop here, tens of cycles even when fast. Keep it out of hot loops.

  .syntax unified
  .section .iwram.aeabi, "ax", %progbits
  .arm
  .align 2

@ unsigned r0 / r1 -> r0 = quotient, r1 = remainder
  .global __aeabi_uidiv
  .global __aeabi_uidivmod
  .type __aeabi_uidiv, %function
  .type __aeabi_uidivmod, %function
__aeabi_uidiv:
__aeabi_uidivmod:
  cmp   r1, #0
  beq   .Ldiv0
  mov   r2, r1                 @ divisor, shifted up
  mov   r3, #1                 @ the quotient bit it stands for
.Lshift:
  cmp   r2, r0, lsr #1
  movls r2, r2, lsl #1
  movls r3, r3, lsl #1
  bls   .Lshift
  mov   r12, #0
.Lsub:
  cmp   r0, r2
  subhs r0, r0, r2
  orrhs r12, r12, r3
  movs  r3, r3, lsr #1
  movne r2, r2, lsr #1
  bne   .Lsub
  mov   r1, r0
  mov   r0, r12
  bx    lr
.Ldiv0:
  mov   r1, r0
  mvn   r0, #0
  bx    lr

@ signed r0 / r1 -> r0 = quotient, r1 = remainder (sign of the dividend)
  .global __aeabi_idiv
  .global __aeabi_idivmod
  .type __aeabi_idiv, %function
  .type __aeabi_idivmod, %function
__aeabi_idiv:
__aeabi_idivmod:
  stmfd sp!, {r4, lr}
  eor   r4, r0, r1             @ bit 31: sign of the quotient
  and   r4, r4, #0x80000000
  orr   r4, r4, r0, lsr #31    @ bit 0: sign of the remainder
  cmp   r0, #0
  rsblt r0, r0, #0
  cmp   r1, #0
  rsblt r1, r1, #0
  bl    __aeabi_uidivmod
  tst   r4, #0x80000000
  rsbne r0, r0, #0
  tst   r4, #1
  rsbne r1, r1, #0
  ldmfd sp!, {r4, lr}
  bx    lr

@ 64-bit multiply: (r1:r0) * (r3:r2) -> r1:r0
  .global __aeabi_lmul
  .type __aeabi_lmul, %function
__aeabi_lmul:
  stmfd sp!, {r4, lr}
  mul   r4, r0, r3
  mla   r4, r1, r2, r4
  umull r0, r1, r2, r0
  add   r1, r1, r4
  ldmfd sp!, {r4, lr}
  bx    lr

@ 64-bit shifts of r1:r0 by r2
  .global __aeabi_llsl
  .type __aeabi_llsl, %function
__aeabi_llsl:
  subs  r3, r2, #32
  movpl r1, r0, lsl r3
  movpl r0, #0
  bxpl  lr
  rsb   r3, r2, #32
  mov   r1, r1, lsl r2
  orr   r1, r1, r0, lsr r3
  mov   r0, r0, lsl r2
  bx    lr

  .global __aeabi_llsr
  .type __aeabi_llsr, %function
__aeabi_llsr:
  subs  r3, r2, #32
  movpl r0, r1, lsr r3
  movpl r1, #0
  bxpl  lr
  rsb   r3, r2, #32
  mov   r0, r0, lsr r2
  orr   r0, r0, r1, lsl r3
  mov   r1, r1, lsr r2
  bx    lr

  .global __aeabi_lasr
  .type __aeabi_lasr, %function
__aeabi_lasr:
  subs  r3, r2, #32
  movpl r0, r1, asr r3
  movpl r1, r1, asr #31
  bxpl  lr
  rsb   r3, r2, #32
  mov   r0, r0, lsr r2
  orr   r0, r0, r1, lsl r3
  mov   r1, r1, asr r2
  bx    lr
