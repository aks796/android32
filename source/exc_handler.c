/* exc_handler.c -- what happens when a thread faults (entered from exc32.S).
 *
 * Every fault is a crash here (the Crossy Road port also emulated JIT stores
 * and delivered SIGSEGV to Mono; this engine has neither): a report --
 * registers, module+offset of pc/lr, a return-address scan of the stack and
 * the log lines not yet written -- goes to svcOutputDebugString and to
 * <game folder>/crash.log, then a failure result hands the fault back to the
 * kernel, which kills the process and lets Atmosphere write its own report.
 * The report path takes no newlib or port locks -- the faulting thread may
 * hold any of them -- and writes the file through the FS service directly.
 *
 * The mod (libHomura) registers a SIGSEGV handler that only logs and aborts;
 * the report says more, so faults are not delivered to it. MIT.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "config.h"
#include "so_util.h"
#include "util.h"

/* ams::svc::aarch32::ExceptionInfo, with the 64-bit-kernel status block. */
typedef struct {
  uint32_t r[8];
  uint32_t sp, lr, pc, flags;
  uint32_t pstate, afsr0, afsr1, esr, far;
} ExcInfo32;

typedef struct {
  uint32_t fpscr, pad;
  uint64_t d16_31[16];
  uint64_t d0_15[16];
  uint32_t r8_12[5];
  uint32_t lr_copy;
} ExcFrame;
_Static_assert(sizeof(ExcFrame) == 288, "must match exc32.S");

#define EXC_INSTRUCTION_ABORT 0x100
#define EXC_DATA_ABORT 0x101
#define EXC_UNALIGNED_INSTRUCTION 0x102
#define EXC_UNALIGNED_DATA 0x103
#define EXC_UNDEFINED_INSTRUCTION 0x104
#define EXC_EXCEPTION_INSTRUCTION 0x105
#define EXC_MEMORY_SYSTEM_ERROR 0x106
#define EXC_FPU 0x200
#define EXC_INVALID_SYSCALL 0x301
#define EXC_SYSCALL_BREAK 0x302

/* ------------------------------------------------------------ registers */
static uint32_t *reg_slot(ExcInfo32 *i, ExcFrame *f, unsigned n) {
  if (n < 8) return &i->r[n];
  if (n < 13) return &f->r8_12[n - 8];
  if (n == 13) return &i->sp;
  if (n == 14) return &i->lr;
  return &i->pc;
}
static uint32_t get_r(ExcInfo32 *i, ExcFrame *f, unsigned n) { return *reg_slot(i, f, n); }

/* ---------------------------------------------------------- crash report */
int dcr_in_code_pool(const void *p); /* lab_loader.c */
extern char _start[];
extern char __rodata_start[] __attribute__((visibility("hidden"))); /* end of our .text (dcr32.ld) */
static char g_crash[4096];
static size_t g_crash_len;

static void out(const char *fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  if ((size_t)n >= sizeof line)
    n = sizeof line - 1;
  svcOutputDebugString(line, (u32)n);
  if (g_crash_len + (size_t)n < sizeof g_crash) {
    memcpy(g_crash + g_crash_len, line, (size_t)n);
    g_crash_len += (size_t)n;
  }
}

static const char *where(uint32_t a, char *buf, size_t cap) {
  so_module *m = so_find_module_by_addr((const void *)a);
  if (m) {
    const char *n = strrchr(m->name, '/') ? strrchr(m->name, '/') + 1 : m->name;
    snprintf(buf, cap, "%.40s+0x%lx", n, (unsigned long)(a - (uint32_t)(uintptr_t)m->load_virtbase));
  } else if (dcr_in_code_pool((const void *)a)) {
    snprintf(buf, cap, "hook trampoline");
  } else if (a >= (uint32_t)(uintptr_t)_start && a < (uint32_t)(uintptr_t)__rodata_start) {
    snprintf(buf, cap, "labyrinth2_nx+0x%lx", (unsigned long)(a - (uint32_t)(uintptr_t)_start));
  } else {
    snprintf(buf, cap, "?");
  }
  return buf;
}

static int is_code(uint32_t a) {
  so_module *m = so_find_module_by_addr((const void *)a);
  if (m)
    return 1;
  return dcr_in_code_pool((const void *)a) ||
         (a >= (uint32_t)(uintptr_t)_start && a < (uint32_t)(uintptr_t)__rodata_start);
}

/* For the watchdog (watchdog.c). */
const char *dcr_addr_name(uint32_t a, char *buf, size_t cap) { return where(a, buf, cap); }
int dcr_is_code_addr(uint32_t a) { return is_code(a); }

/* How many bytes from p on are mapped and readable (up to the end of p's
 * memory region), at most `want`. Another thread's stack is read only this
 * far: its recorded bounds can be wider than what is mapped (hardware
 * 2026-09-24: the profiler died reading past a stack's last page). */
size_t dcr_readable(uint32_t p, size_t want) {
  MemoryInfo mi;
  u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, p)) || !(mi.perm & Perm_R) || mi.type == MemType_Unmapped)
    return 0;
  uint64_t end = mi.addr + mi.size;
  if (end <= p)
    return 0;
  return end - p < want ? (size_t)(end - p) : want;
}

static void write_crash_file(void) {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs)
    return;
  static const char path[] = DCR_ROOT_PATH "/crash.log";
  fsFsCreateFile(fs, path, 0, 0);
  FsFile file;
  if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &file)))
    return;
  s64 size = 0;
  fsFileGetSize(&file, &size);
  fsFileWrite(&file, size, g_crash, g_crash_len, FsWriteOption_Flush);
  fsFileClose(&file);
}

static const char *type_name(uint32_t t) {
  switch (t) {
  case EXC_INSTRUCTION_ABORT: return "instruction abort";
  case EXC_DATA_ABORT: return "data abort";
  case EXC_UNALIGNED_INSTRUCTION: return "unaligned instruction";
  case EXC_UNALIGNED_DATA: return "unaligned data";
  case EXC_UNDEFINED_INSTRUCTION: return "undefined instruction";
  case EXC_EXCEPTION_INSTRUCTION: return "exception instruction";
  case EXC_MEMORY_SYSTEM_ERROR: return "memory system error";
  case EXC_FPU: return "FPU exception";
  case EXC_INVALID_SYSCALL: return "invalid system call";
  case EXC_SYSCALL_BREAK: return "svcBreak";
  default: return "exception";
  }
}

static void crash_report(uint32_t type, ExcInfo32 *i, ExcFrame *f) {
  char w1[64], w2[64];
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);
  g_crash_len = 0;
  out("\n[crash] ===== %s (0x%lx) in thread %llu =====\n", type_name(type), (unsigned long)type,
      (unsigned long long)tid);
  out("[crash] pc  %08lx  %s%s\n", (unsigned long)i->pc, where(i->pc, w1, sizeof w1),
      (i->pstate & 0x20) ? " (Thumb)" : "");
  out("[crash] lr  %08lx  %s\n", (unsigned long)i->lr, where(i->lr, w2, sizeof w2));
  out("[crash] far %08lx  esr %08lx  pstate %08lx\n", (unsigned long)i->far, (unsigned long)i->esr,
      (unsigned long)i->pstate);
  for (unsigned r = 0; r < 13; r += 4) {
    char line[128];
    int n = 0;
    for (unsigned k = r; k < r + 4 && k < 13; k++)
      n += snprintf(line + n, sizeof line - n, " r%-2u %08lx", k, (unsigned long)get_r(i, f, k));
    out("[crash]%s\n", line);
  }
  out("[crash] sp  %08lx\n", (unsigned long)i->sp);
  if (!(i->pstate & 0x20) && is_code(i->pc))
    out("[crash] insn %08lx\n", (unsigned long)*(const volatile uint32_t *)i->pc);

  /* Return addresses on the stack (heuristic: words that point into code). */
  MemoryInfo mi;
  u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, i->sp)) && mi.perm & Perm_R) {
    uint32_t end = (uint32_t)(mi.addr + mi.size), shown = 0;
    for (uint32_t a = i->sp & ~3u; a + 4 <= end && a < i->sp + 0x2000 && shown < 16; a += 4) {
      uint32_t v = *(const volatile uint32_t *)a;
      if (is_code(v & ~1u)) {
        out("[crash]   #%-2lu %08lx  %s\n", (unsigned long)shown, (unsigned long)v, where(v & ~1u, w1, sizeof w1));
        shown++;
      }
    }
  }
  /* Log lines still in the RAM ring (quiet mode, see dcr_boot.c). */
  static char tail[4096];
  if (log_ring_tail(tail, sizeof tail))
    out("[crash] last log lines not yet in debug.log:\n%s\n", tail);
  write_crash_file();
}

/* ---------------------------------------------------------------- entry */
Result dcr_exception_dispatch(uint32_t type, ExcInfo32 *info, ExcFrame *frame) {
  crash_report(type, info, frame);
  return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}
