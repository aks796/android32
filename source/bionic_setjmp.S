/* bionic_setjmp.S -- setjmp/longjmp/sigsetjmp/siglongjmp for the game's code.
 *
 * Why not newlib's: devkitARM's newlib is built soft-float and its setjmp does
 * not save d8-d15. The game is softfp (VFP instructions, core-register calling
 * convention) and AAPCS makes d8-d15 callee-saved, so a longjmp that dropped
 * them would silently corrupt floating-point state in the frame it returns to.
 *
 * Layout (only these routines read it; bionic's jmp_buf is 256 bytes):
 *   [0]      magic
 *   [4..44]  r4-r11, sp, lr        (10 words)
 *   [48..]   d8-d15                (8 doublewords)
 *   [112]    fpscr
 * MIT.
 */
    .syntax unified
    .arm
    .fpu neon-fp-armv8

    .macro FUNC name
    .section .text.\name, "ax", %progbits
    .global \name
    .type \name, %function
    .align 2
\name:
    .endm

    .set JB_MAGIC, 0x4a4d5042      @ 'JMPB'

FUNC b_setjmp
    ldr     r1, =JB_MAGIC
    str     r1, [r0]
    add     r1, r0, #4
    stmia   r1, {r4-r11, sp, lr}
    add     r1, r0, #48
    vstmia  r1, {d8-d15}
    vmrs    r2, fpscr
    str     r2, [r0, #112]
    mov     r0, #0
    bx      lr
    .ltorg

FUNC b_longjmp
    add     r2, r0, #48
    vldmia  r2, {d8-d15}
    ldr     r2, [r0, #112]
    vmsr    fpscr, r2
    add     r2, r0, #4
    ldmia   r2, {r4-r11, sp, lr}
    movs    r0, r1
    moveq   r0, #1                  @ longjmp(env, 0) returns 1
    bx      lr

@ sigsetjmp(env, savemask): no signal masks exist here, so it is setjmp.
FUNC b_sigsetjmp
    b       b_setjmp

FUNC b_siglongjmp
    b       b_longjmp
