/* exc_handler.c -- what happens when a thread faults (entered from exc32.S).
 *
 * First the port's port_exception_hook gets the fault: a port whose engine
 * recovers from faults handles them there (the Crossy Road port emulates Mono
 * JIT stores and delivers SIGSEGV to Mono's handler). Everything else is a
 * crash: a report -- registers, module+offset (and the nearest exported
 * function) of pc/lr, a return-address scan of the stack and the log lines
 * not yet written -- goes to svcOutputDebugString and to
 * <game folder>/crash.log, then a failure result hands the fault back to the
 * kernel, which kills the process and lets Atmosphere write its own report.
 * The report path takes no newlib or port locks -- the faulting thread may
 * hold any of them -- and writes the file through the FS service directly.
 *
 * A game module may register a SIGSEGV handler that only logs and aborts
 * (PvZ's mod does); the report says more, so faults are not delivered to it
 * unless the port's hook does so. MIT.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "rt_settings.h"
#include "exc_handler.h"
#include "so_util.h"
#include "util.h"

_Static_assert(sizeof(ThreadExceptionInfo32) == 0x44, "the kernel's 32-bit ExceptionInfo");
_Static_assert(sizeof(ThreadExceptionFrame32) == 288, "must match exc32.S");

/* ------------------------------------------------------------ callbacks */
__attribute__((weak)) int port_exception_hook(uint32_t type, ThreadExceptionInfo32 *info,
                                              ThreadExceptionFrame32 *frame) {
  (void)type, (void)info, (void)frame;
  return 0;
}

__attribute__((weak)) const char *port_code_region(uint32_t addr) {
  (void)addr;
  return NULL;
}

/* ------------------------------------------------------------ registers */
uint32_t *rt_exc_reg(ThreadExceptionInfo32 *i, ThreadExceptionFrame32 *f, unsigned n) {
  if (n < 8) return &i->r[n];
  if (n < 13) return &f->r8_12[n - 8];
  if (n == 13) return &i->sp;
  if (n == 14) return &i->lr;
  return &i->pc;
}
static uint32_t get_r(ThreadExceptionInfo32 *i, ThreadExceptionFrame32 *f, unsigned n) {
  return *rt_exc_reg(i, f, n);
}

/* ---------------------------------------------------------- crash report */
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

static const char *where(uint32_t a, char *buf, size_t cap) {
  so_module *m = so_find_module_by_addr((const void *)a);
  const char *label;
  if (m) {
    const char *n = strrchr(m->name, '/') ? strrchr(m->name, '/') + 1 : m->name;
    const uint32_t off = a - (uint32_t)(uintptr_t)m->load_virtbase;
    /* Name the function too: the nearest exported STT_FUNC at or below off.
     * The symbol table is checked readable first -- inside so_finalize it
     * briefly points at staging pages already donated to the code mapping. */
    const char *fn = NULL;
    uint32_t best = 0;
    if (m->syms && dcr_readable((uint32_t)(uintptr_t)m->syms, sizeof(Elf32_Sym)))
      for (int i = 0; i < m->num_syms; i++) {
        const Elf32_Sym *s = &m->syms[i];
        uint32_t v = s->st_value & ~1u;
        if (s->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(s->st_info) != STT_FUNC || v > off || v < best)
          continue;
        best = v;
        fn = m->dynstrtab + s->st_name;
      }
    if (fn && off - best < 0x10000)
      snprintf(buf, cap, "%.24s+0x%lx (%.90s+0x%lx)", n, (unsigned long)off, fn,
               (unsigned long)(off - best));
    else
      snprintf(buf, cap, "%.40s+0x%lx", n, (unsigned long)off);
  } else if ((label = port_code_region(a)) != NULL) {
    snprintf(buf, cap, "%s", label);
  } else if (a >= (uint32_t)(uintptr_t)_start && a < (uint32_t)(uintptr_t)__rodata_start) {
    /* our own text: offsets into the payload's ELF (the Makefile's TARGET) */
    snprintf(buf, cap, PORT_PAYLOAD_NAME "+0x%lx", (unsigned long)(a - (uint32_t)(uintptr_t)_start));
  } else {
    snprintf(buf, cap, "?");
  }
  return buf;
}

static int is_code(uint32_t a) {
  so_module *m = so_find_module_by_addr((const void *)a);
  if (m)
    return 1;
  return port_code_region(a) != NULL ||
         (a >= (uint32_t)(uintptr_t)_start && a < (uint32_t)(uintptr_t)__rodata_start);
}

/* For the watchdog (watchdog.c), the profilers and the printf shims. */
const char *dcr_addr_name(uint32_t a, char *buf, size_t cap) { return where(a, buf, cap); }
int dcr_is_code_addr(uint32_t a) { return is_code(a); }

static void write_crash_file(void) {
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs)
    return;
  static const char path[] = PORT_ROOT_PATH "/crash.log";
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

static void crash_report(uint32_t type, ThreadExceptionInfo32 *i, ThreadExceptionFrame32 *f) {
  char w1[160], w2[160]; /* where() with a function name is up to ~140 */
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
  /* Log lines still in the RAM ring (quiet mode). */
  static char tail[4096];
  if (log_ring_tail(tail, sizeof tail))
    out("[crash] last log lines not yet in debug.log:\n%s\n", tail);
  write_crash_file();
}

/* ---------------------------------------------------------------- entry */
Result dcr_exception_dispatch(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame) {
  if (port_exception_hook(type, info, frame))
    return 0;
  crash_report(type, info, frame);
  return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

#if __has_include(<switch/arm/exception32.h>)
/* The libnx32 fork's entry (exception32.s) calls this. exc32.S's entry is used
 * instead unless the build assembles it with RT_OWN_EXC_ENTRY=0 (see there);
 * then this is the way in, to the same handler. */
Result __libnx_exception_handler32(u32 type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame) {
  return dcr_exception_dispatch(type, info, frame);
}
#endif
