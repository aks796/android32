/* exc32.S -- AArch32 user-mode exception entry (libnx32 before 4.12.0 ships a TODO stub).
 *
 * On a fault the kernel (Mesosphere HandleUserException, 32-bit branch):
 *   - saves r0-r7, sp, lr, pc, pstate, esr, far into the process-local
 *     region's ExceptionInfo;
 *   - re-enters the program at its entry point with r0 = exception type,
 *     r1 = &ExceptionInfo, sp = the tail of the process-local region (<448
 *     bytes -- far too small for C), ARM state;
 *   - leaves r8-r12 and the VFP/NEON registers exactly as the faulting code
 *     had them, and on svcReturnFromException restores only what it saved.
 * libnx32's crt0 dispatches here when r0 != 0 && r1 != -1.
 *
 * So this entry moves to its own stack, saves r8-r12, d0-d31 and FPSCR (the
 * faulting instruction can be anywhere, with any register live), calls
 * dcr_exception_dispatch(type, info, frame) and restores everything before
 * svcReturnFromException(result). The kernel serialises user exceptions per
 * process (one thread inside at a time), so one static stack is enough.
 *
 * Frame layout at the call (ThreadExceptionFrame32, exc_handler.h):
 *   [sp+0]    fpscr, pad
 *   [sp+8]    d16-d31
 *   [sp+136]  d0-d15
 *   [sp+264]  r8, r9, r10, r11, r12, lr
 *
 * The libnx32 fork (4.12.0) ships this same entry as a weak symbol with a
 * 32 KiB stack; this strong one (64 KiB) replaces it. RT_OWN_EXC_ENTRY=0 on
 * the assembler's command line (it cannot see port_config.h) leaves this file
 * empty, and the fork's entry reaches the same handler through
 * __libnx_exception_handler32 (exc_handler.c). MIT.
 */
#ifndef RT_OWN_EXC_ENTRY
#define RT_OWN_EXC_ENTRY 1
#endif
#if RT_OWN_EXC_ENTRY
    .syntax unified
    .arm
    .fpu neon-fp-armv8

    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type __libnx_exception_entry, %function
    .align 2
__libnx_exception_entry:
    ldr     r2, 1f
0:  add     r2, pc, r2              @ r2 = dcr_exc_stack_end (pc-relative, no relocation)
    mov     sp, r2
    push    {r8-r12, lr}
    vpush   {d0-d15}
    vpush   {d16-d31}
    vmrs    r2, fpscr
    push    {r2, r3}
    mov     r2, sp                  @ r0 = type, r1 = info, r2 = frame
    bl      dcr_exception_dispatch
    mov     r4, r0                  @ result; r4 is restored by the kernel from info
    pop     {r2, r3}
    vmsr    fpscr, r2
    vpop    {d16-d31}
    vpop    {d0-d15}
    pop     {r8-r12, lr}
    mov     r0, r4
    svc     0x28                    @ svcReturnFromException(result): does not return
    b       .
1:  .word   dcr_exc_stack_end - (0b + 8)
    .size __libnx_exception_entry, . - __libnx_exception_entry

    .section .bss.dcr_exc_stack, "aw", %nobits
    .align 4
    .global dcr_exc_stack
dcr_exc_stack:
    .space  0x10000
    .global dcr_exc_stack_end
dcr_exc_stack_end:
#endif /* RT_OWN_EXC_ENTRY */
