/* kuser.S -- the Linux kernel's user helpers, for code built with an old
 * libgcc.
 *
 * Every 32-bit ARM Linux (and Android) process has the kernel's "kuser"
 * page at 0xffff0000. libgcc's linux-atomic.c builds the __sync_* functions
 * on two of its entry points, loaded from literal pools and called with
 * blx: __kuser_cmpxchg at 0xffff0fc0 and __kuser_memory_barrier at
 * 0xffff0fa0. Engines built with an old NDK carry that code (PvZ's two
 * libraries, Sonic's); Horizon has nothing at 0xffff0000, so the first
 * __sync_fetch_and_add was an instruction abort there (PvZ, hardware
 * 2026-09-24, in a constructor). so_fix_kuser_helpers (so_util.c) points the
 * literals here instead.
 *
 * Same contract as the kernel's (arch/arm/kernel/entry-armv.S): only r3, ip
 * and the flags may change; cmpxchg returns r0 = 0 with C set when *ptr held
 * oldval and now holds newval, non-zero with C clear otherwise. MIT.
 */
    .syntax unified
    .arm
    .text

/* int __kuser_cmpxchg(int oldval, int newval, volatile int *ptr) */
    .global dcr_kuser_cmpxchg
    .type dcr_kuser_cmpxchg, %function
    .align 2
dcr_kuser_cmpxchg:
    dmb     ish
1:  ldrex   r3, [r2]
    subs    r3, r3, r0
    strexeq r3, r1, [r2]
    teqeq   r3, #1
    beq     1b
    rsbs    r0, r3, #0
    dmb     ish
    bx      lr
    .size dcr_kuser_cmpxchg, . - dcr_kuser_cmpxchg

/* void __kuser_memory_barrier(void) */
    .global dcr_kuser_memory_barrier
    .type dcr_kuser_memory_barrier, %function
    .align 2
dcr_kuser_memory_barrier:
    dmb     ish
    bx      lr
    .size dcr_kuser_memory_barrier, . - dcr_kuser_memory_barrier
