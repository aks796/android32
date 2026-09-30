/* exc_handler.h -- the fault handler's interface (exc_handler.c).
 *
 * Every user-mode fault ends up in dcr_exception_dispatch (from exc32.S, or
 * from the libnx32 fork's entry through __libnx_exception_handler32). A port
 * that recovers from some faults (the Crossy Road port: Mono JIT stores and
 * Linux signal delivery) does so in port_exception_hook; everything else gets
 * the crash report. The types are the libnx32 fork's (arm/exception32.h),
 * defined here with the same layout for an older libnx32. MIT.
 */
#ifndef DCR_EXC_HANDLER_H
#define DCR_EXC_HANDLER_H
#include <stddef.h>
#include <stdint.h>
#include <switch.h>

#if !__has_include(<switch/arm/exception32.h>)
/* libnx32 before 4.12.0: the fork's layouts (static-asserted in exc_handler.c). */
typedef struct {
  u32 r[8];           /* r0-r7 */
  u32 sp, lr, pc, flags;
  u32 pstate;         /* CPSR (bit 5: Thumb) */
  u32 afsr0, afsr1, esr;
  u32 far;            /* fault address */
} ThreadExceptionInfo32;

typedef struct {
  u32 fpscr, pad;
  u64 d16_31[16];
  u64 d0_15[16];
  u32 r8_12[5];       /* r8-r12 */
  u32 lr_entry;
} ThreadExceptionFrame32;
#endif

/* Exception types (the kernel's r0 at entry). */
#define EXC_INSTRUCTION_ABORT     0x100
#define EXC_DATA_ABORT            0x101
#define EXC_UNALIGNED_INSTRUCTION 0x102
#define EXC_UNALIGNED_DATA        0x103
#define EXC_UNDEFINED_INSTRUCTION 0x104
#define EXC_EXCEPTION_INSTRUCTION 0x105
#define EXC_MEMORY_SYSTEM_ERROR   0x106
#define EXC_FPU                   0x200
#define EXC_INVALID_SYSCALL       0x301
#define EXC_SYSCALL_BREAK         0x302

/* The entry's C half: 0 resumes the thread with *info / *frame (a port hook
 * handled it), anything else lets the kernel end the process after the crash
 * report (Atmosphere then writes its own). */
Result dcr_exception_dispatch(uint32_t type, ThreadExceptionInfo32 *info,
                              ThreadExceptionFrame32 *frame);

/* Register n (0-15) of the faulting thread, where the kernel or the entry
 * saved it: writable, and what the thread resumes with. */
uint32_t *rt_exc_reg(ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame, unsigned n);

/* For the watchdog, the profilers and the printf shims (lock-free). */
const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* "libfoo.so+0x1234 (fn+0x10)" */
int dcr_is_code_addr(uint32_t a);
size_t dcr_readable(uint32_t p, size_t want); /* mapped + readable bytes from p, at most want */

/* CALLBACKS (weak; a port defines the strong one). Both run in exception
 * context: no locks the faulting thread may hold (newlib stdio and malloc
 * included), no allocation. */

/* Before the crash report, for every fault. Return 1 when the port handled it
 * (the thread resumes with the possibly changed *info / *frame: pc, sp,
 * registers), 0 for the crash report. Default: 0. */
int port_exception_hook(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame);

/* A label for code at addr that no loaded module holds ("hook trampoline",
 * "JIT code"), or NULL. Non-NULL also makes addr count as code for the
 * stack scans (dcr_is_code_addr). Default: NULL. */
const char *port_code_region(uint32_t addr);

#endif /* DCR_EXC_HANDLER_H */
