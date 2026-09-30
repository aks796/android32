/* bionic_core.c -- errno, environment, properties, logging, process identity,
 * ctype tables, aeabi memory helpers, atexit and the other small bionic
 * entry points. See bionic.h for the ABI facts these conversions rest on. MIT.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "config.h"
#include "error.h"
#include "bionic_pthread.h"
#include "util.h"

/* ============================== errno ===================================== */
int b_errno_to_linux(int e) {
  if (e <= 34)
    return e; /* EPERM..ERANGE share their numbers */
  switch (e) {
  case EDEADLK: return L_EDEADLK;
  case ENAMETOOLONG: return L_ENAMETOOLONG;
  case ENOLCK: return L_ENOLCK;
  case ENOSYS: return L_ENOSYS;
  case ENOTEMPTY: return L_ENOTEMPTY;
  case ELOOP: return L_ELOOP;
  case EOVERFLOW: return L_EOVERFLOW;
  case EILSEQ: return L_EILSEQ;
  case ENOTSOCK: return L_ENOTSOCK;
  case EOPNOTSUPP: return L_EOPNOTSUPP;
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
  case ENOTSUP: return L_EOPNOTSUPP;
#endif
  case EAFNOSUPPORT: return L_EAFNOSUPPORT;
  case ENETDOWN: return L_ENETDOWN;
  case ENETUNREACH: return L_ENETUNREACH;
  case ECONNRESET: return L_ECONNRESET;
  case ENOTCONN: return L_ENOTCONN;
  case ETIMEDOUT: return L_ETIMEDOUT;
  case ECONNREFUSED: return L_ECONNREFUSED;
  case EHOSTUNREACH: return L_EHOSTUNREACH;
  case EALREADY: return L_EALREADY;
  case EINPROGRESS: return L_EINPROGRESS;
  case EADDRINUSE: return 98;
  case EADDRNOTAVAIL: return 99;
  case ECONNABORTED: return 103;
  case EISCONN: return 106;
  case EMSGSIZE: return 90;
  case ENOBUFS: return 105;
  case EDESTADDRREQ: return 89;
  case ENOPROTOOPT: return 92;
  case EPROTONOSUPPORT: return 93;
  case EPROTOTYPE: return 91;
  case ENETRESET: return 102;
  case EHOSTDOWN: return 112;
  default: return L_EIO;
  }
}

static int linux_to_newlib_errno(int e) {
  if (e <= 34)
    return e;
  switch (e) {
  case L_EDEADLK: return EDEADLK;
  case L_ENAMETOOLONG: return ENAMETOOLONG;
  case L_ENOSYS: return ENOSYS;
  case L_ENOTEMPTY: return ENOTEMPTY;
  case L_ETIMEDOUT: return ETIMEDOUT;
  case L_EINPROGRESS: return EINPROGRESS;
  case L_ECONNREFUSED: return ECONNREFUSED;
  case L_ECONNRESET: return ECONNRESET;
  case L_ENOTCONN: return ENOTCONN;
  case L_ENETDOWN: return ENETDOWN;
  case L_ENETUNREACH: return ENETUNREACH;
  case L_EHOSTUNREACH: return EHOSTUNREACH;
  case L_EALREADY: return EALREADY;
  case 98: return EADDRINUSE;
  case 99: return EADDRNOTAVAIL;
  case 103: return ECONNABORTED;
  case 106: return EISCONN;
  default: return EIO;
  }
}

/* The game reads errno through __errno(), which is newlib's per-thread slot.
 * Shims store LINUX numbers there. */
void b_set_errno(int linux_errno) { errno = linux_errno; }
void b_fix_errno(void) { errno = b_errno_to_linux(errno); }

char *b_strerror(int linux_errno) { return strerror(linux_to_newlib_errno(linux_errno)); }

void b_perror(const char *s) {
  debugPrintf("[perror] %s%s%s\n", s ? s : "", s ? ": " : "", b_strerror(errno));
}

/* ======================= stack protector / abort ========================== */
uintptr_t b___stack_chk_guard = 0x5a17c0deu;

void NORETURN b___stack_chk_fail(void) {
  debugPrintf("[abort] __stack_chk_fail from %p\n", __builtin_return_address(0));
  fatal_error("Stack corruption detected (__stack_chk_fail) at %p.",
              __builtin_return_address(0));
}

void NORETURN b_abort(void) {
  debugPrintf("[abort] abort() from %p\n", __builtin_return_address(0));
  fatal_error("The game called abort() from %p.", __builtin_return_address(0));
}

void NORETURN b___assert2(const char *file, int line, const char *func, const char *expr) {
  fatal_error("Assertion failed in the game:\n%s:%d %s\n%s", file ? file : "?", line,
              func ? func : "?", expr ? expr : "?");
}

/* The engine ending the process (a missing resource, its own Quit): on Linux
 * every thread stops at once. libnx's exit() instead shuts its services down
 * while our threads still run -- hardware run 3 (2026-09-24): the engine's
 * _exit(0) on its app thread, then the UI thread's next touch-screen poll
 * found hid gone and libnx aborted (0x1159, an Atmosphere crash report). So:
 * log, flush, and end the process in one step. */
static void NORETURN end_process(const char *what, int code, void *from, int flush_files) {
  debugPrintf("[exit] %s(%d) from %p: ending the process\n", what, code, from);
  log_flush_ring();
  if (flush_files)
    fflush(NULL); /* exit() flushes stdio; _exit() does not */
  svcExitProcess();
  __builtin_unreachable();
}

void NORETURN b_exit(int code) { end_process("exit", code, __builtin_return_address(0), 1); }
void NORETURN b__exit(int code) { end_process("_exit", code, __builtin_return_address(0), 0); }

/* ================================ page size ================================ */
unsigned int b___page_size = 0x1000;

/* ============================== environment ================================ */
/* Values the game reads that Android would have in its environment.
 * libnative_code adds ANDROID_{SOURCE,DATA,FILES,EXTFILES}_DIR with putenv(),
 * and the engine finds its data through getenv("ANDROID_SOURCE_DIR"). */
static char *g_env_store[64] = {
    "ANDROID_ROOT=/system",
    "ANDROID_DATA=/data",
    "EXTERNAL_STORAGE=/sdcard",
    "HOME=/data/data/" DCR_PACKAGE "/files",
    "TMPDIR=/data/data/" DCR_PACKAGE "/cache",
    "LANG=en_US.UTF-8",
    NULL,
};
char **b_environ = g_env_store;
static Mutex g_env_lock;

static int env_find(const char *name, size_t nlen) {
  for (int i = 0; g_env_store[i]; i++)
    if (!strncmp(g_env_store[i], name, nlen) && g_env_store[i][nlen] == '=')
      return i;
  return -1;
}

char *b_getenv(const char *name) {
  if (!name)
    return NULL;
  mutexLock(&g_env_lock);
  int i = env_find(name, strlen(name));
  char *v = i >= 0 ? g_env_store[i] + strlen(name) + 1 : NULL;
  mutexUnlock(&g_env_lock);
  return v;
}

int b_setenv(const char *name, const char *value, int overwrite) {
  if (!name || !*name || strchr(name, '=')) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  size_t nlen = strlen(name);
  mutexLock(&g_env_lock);
  int i = env_find(name, nlen);
  if (i >= 0 && !overwrite) {
    mutexUnlock(&g_env_lock);
    return 0;
  }
  char *kv = malloc(nlen + strlen(value ? value : "") + 2);
  if (!kv) {
    mutexUnlock(&g_env_lock);
    b_set_errno(L_ENOMEM);
    return -1;
  }
  sprintf(kv, "%s=%s", name, value ? value : "");
  if (i < 0) {
    for (i = 0; g_env_store[i]; i++) {
    }
    if (i >= (int)ARRAY_SIZE(g_env_store) - 1) {
      mutexUnlock(&g_env_lock);
      free(kv);
      b_set_errno(L_ENOMEM);
      return -1;
    }
    g_env_store[i + 1] = NULL;
  }
  g_env_store[i] = kv; /* the old string may be static; never freed */
  mutexUnlock(&g_env_lock);
  return 0;
}

/* putenv("K=V"): the string becomes part of the environment (POSIX). */
int b_putenv(char *kv) {
  const char *eq = kv ? strchr(kv, '=') : NULL;
  if (!eq || eq == kv) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  char name[128];
  size_t n = (size_t)(eq - kv) < sizeof name - 1 ? (size_t)(eq - kv) : sizeof name - 1;
  memcpy(name, kv, n);
  name[n] = 0;
  debugPrintf("[env] %s\n", kv);
  return b_setenv(name, eq + 1, 1);
}

int b_unsetenv(const char *name) {
  if (!name)
    return 0;
  mutexLock(&g_env_lock);
  int i = env_find(name, strlen(name));
  if (i >= 0)
    for (; g_env_store[i]; i++)
      g_env_store[i] = g_env_store[i + 1];
  mutexUnlock(&g_env_lock);
  return 0;
}

/* ============================ system properties ============================ */
/* What the engine and the Mono runtime ask Android about the device. */
static const struct { const char *k, *v; } g_props[] = {
    {"ro.build.version.sdk", "28"},
    {"ro.build.version.release", "9"},
    {"ro.product.manufacturer", "Nintendo"},
    {"ro.product.model", "Switch"},
    {"ro.product.brand", "Nintendo"},
    {"ro.product.name", "switch"},
    {"ro.product.device", "switch"},
    {"ro.product.cpu.abi", "armeabi-v7a"},
    {"ro.product.cpu.abi2", "armeabi"},
    {"ro.hardware", "tegra"},
    {"ro.board.platform", "tegra"},
    {"ro.kernel.qemu", "0"},
    {"ro.debuggable", "0"},
    {"ro.build.fingerprint", "nintendo/switch/switch:9/labyrinth2_nx/1:user/release-keys"},
    {"ro.build.type", "user"},
};

int b___system_property_get(const char *name, char *value) {
  if (!value)
    return 0;
  value[0] = 0;
  if (!name)
    return 0;
  for (unsigned i = 0; i < ARRAY_SIZE(g_props); i++)
    if (!strcmp(name, g_props[i].k)) {
      strncpy(value, g_props[i].v, 91);
      value[91] = 0;
      return (int)strlen(value);
    }
  return 0;
}

/* ================================= logging ================================= */
static const char *prio_name(int p) {
  static const char *const n[] = {"?", "?", "V", "D", "I", "W", "E", "F", "S"};
  return (p >= 0 && p < 9) ? n[p] : "?";
}

int b___android_log_write(int prio, const char *tag, const char *text) {
  debugPrintf("[%s/%s] %s\n", prio_name(prio), tag ? tag : "", text ? text : "");
  return 1;
}

int b___android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
  char buf[1024];
  vsnprintf(buf, sizeof buf, fmt ? fmt : "", ap);
  return b___android_log_write(prio, tag, buf);
}

int b___android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = b___android_log_vprint(prio, tag, fmt, ap);
  va_end(ap);
  return r;
}

/* =========================== process identity ============================== */
#define DCR_FAKE_PID 4242
#define DCR_FAKE_UID 10123

b_pid_t b_getpid(void) { return DCR_FAKE_PID; }
b_uid_t b_getuid(void) { return DCR_FAKE_UID; }
b_uid_t b_geteuid(void) { return DCR_FAKE_UID; }
int b_getresuid(b_uid_t *r, b_uid_t *e, b_uid_t *s) {
  if (r) *r = DCR_FAKE_UID;
  if (e) *e = DCR_FAKE_UID;
  if (s) *s = DCR_FAKE_UID;
  return 0;
}
int b_setresuid(b_uid_t r, b_uid_t e, b_uid_t s) { return 0; }
int b_getpriority(int which, int who) { return 0; }
int b_setpriority(int which, int who, int prio) { return 0; }
int b_getdtablesize(void) { return 1024; }

int b_gethostname(char *name, size_t len) {
  if (name && len) {
    strncpy(name, "localhost", len);
    name[len - 1] = 0;
  }
  return 0;
}

int b_uname(struct b_utsname *u) {
  if (!u) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  memset(u, 0, sizeof *u);
  strcpy(u->sysname, "Linux");
  strcpy(u->nodename, "localhost");
  strcpy(u->release, "4.9.0");
  strcpy(u->version, "#1 SMP");
  strcpy(u->machine, "armv8l");
  return 0;
}

int b_getrusage(int who, struct b_rusage *ru) {
  if (!ru) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  memset(ru, 0, sizeof *ru);
  u64 ns = armTicksToNs(armGetSystemTick());
  ru->ru_utime.tv_sec = (int32_t)(ns / 1000000000ull);
  ru->ru_utime.tv_usec = (int32_t)((ns / 1000ull) % 1000000ull);
  ru->ru_maxrss = 256 * 1024;
  return 0;
}

static char g_pw_name[] = "u0_a123", g_pw_dir[] = "/data/data/" DCR_PACKAGE,
            g_pw_shell[] = "/system/bin/sh", g_empty[] = "";
static struct b_passwd g_pw = {g_pw_name, g_empty, DCR_FAKE_UID, DCR_FAKE_UID, g_pw_dir, g_pw_shell};
struct b_passwd *b_getpwuid(b_uid_t uid) { return &g_pw; }
struct b_passwd *b_getpwnam(const char *name) { return &g_pw; }
static char *g_gr_mem[] = {NULL};
static struct b_group g_gr = {g_pw_name, g_empty, DCR_FAKE_UID, g_gr_mem};
struct b_group *b_getgrgid(b_gid_t gid) { return &g_gr; }
struct b_group *b_getgrnam(const char *name) { return &g_gr; }

/* bionic sysconf numbering, confirmed at the call sites in libunity/libmono
 * (6, 40, 97, 98 are the values they pass). */
long b_sysconf(int name) {
  switch (name) {
  case 0x00: return 2097152;      /* _SC_ARG_MAX */
  case 0x06: return 100;          /* _SC_CLK_TCK */
  case 0x0b: return 1024;         /* _SC_OPEN_MAX */
  case 0x27:                      /* _SC_PAGESIZE */
  case 0x28: return 0x1000;       /* _SC_PAGE_SIZE */
  case 0x60:                      /* _SC_NPROCESSORS_CONF */
  case 0x61: return 3;            /* _SC_NPROCESSORS_ONLN: three cores for applications */
  case 0x62: {                    /* _SC_PHYS_PAGES */
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    return (long)(total >> 12);
  }
  case 0x63: {                    /* _SC_AVPHYS_PAGES */
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    return (long)((total > used ? total - used : 0) >> 12);
  }
  default:
    debugPrintf("[sysconf] unknown name 0x%x -> -1\n", name);
    b_set_errno(L_EINVAL);
    return -1;
  }
}

/* No processes here. */
int b_fork(void) { b_set_errno(L_ENOSYS); return -1; }
int b_execl(const char *p, ...) { b_set_errno(L_ENOSYS); return -1; }
int b_execv(const char *p, char *const a[]) { b_set_errno(L_ENOSYS); return -1; }
int b_execve(const char *p, char *const a[], char *const e[]) { b_set_errno(L_ENOSYS); return -1; }
int b_system(const char *cmd) { return -1; }
b_pid_t b_waitpid(b_pid_t pid, int *status, int options) { b_set_errno(L_ECHILD); return -1; }
long b_ptrace(int req, ...) { b_set_errno(L_EPERM); return -1; }

/* prctl: PR_SET_NAME (15) is the only one used -- thread naming. */
int b_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5) {
  if (option == 15 && a2) { /* PR_SET_NAME: thread names (watchdog, crash reports) */
    BThread *t = b_thread_self();
    if (t)
      snprintf(t->name, sizeof t->name, "%s", (const char *)(uintptr_t)a2);
  }
  return 0;
}

/* LC_ALL is 6 in Linux numbering (setlocale(6, ...) at libmono's call sites);
 * newlib's is 0. Everything runs in the C locale. */
char *b_setlocale(int category, const char *locale) {
  static char c_locale[] = "C";
  return c_locale;
}

/* ================================ atexit =================================== */
/* The game's static destructors never run: this process ends by leaving to the
 * HOME menu or by exit(), and running engine destructors against torn-down
 * services faults. Registration just succeeds. */
int b___cxa_atexit(void (*fn)(void *), void *arg, void *dso) { return 0; }
int b___aeabi_atexit(void *arg, void (*fn)(void *), void *dso) { return 0; }
void b___cxa_finalize(void *dso) {}

/* ================================ ctype ==================================== */
/* bionic declares `extern const char *_ctype_` (a POINTER, confirmed in
 * libunity: load GOT, dereference, +1) indexed as (_ctype_ + 1)[c], so entry 0
 * is EOF. BSD flag bits. */
#undef _U
#define _U 0x01
#undef _L
#define _L 0x02
#undef _N
#define _N 0x04
#undef _S
#define _S 0x08
#undef _P
#define _P 0x10
#undef _C
#define _C 0x20
#undef _X
#define _X 0x40
#undef _B
#define _B 0x80
static char g_ctype_tab[1 + 256];
static short g_tolower_tab[1 + 256];
static short g_toupper_tab[1 + 256];
const char *b__ctype_ = g_ctype_tab;
const short *b__tolower_tab_ = g_tolower_tab;
const short *b__toupper_tab_ = g_toupper_tab;

__attribute__((constructor)) static void ctype_init(void) {
  g_ctype_tab[0] = 0;
  g_tolower_tab[0] = -1;
  g_toupper_tab[0] = -1;
  for (int c = 0; c < 256; c++) {
    char f = 0;
    if (c < 128) {
      if (isupper(c)) f |= _U;
      if (islower(c)) f |= _L;
      if (isdigit(c)) f |= _N;
      if (isspace(c)) f |= _S;
      if (ispunct(c)) f |= _P;
      if (iscntrl(c)) f |= _C;
      if (isxdigit(c) && !isdigit(c)) f |= _X;
      if (c == ' ') f |= _B;
    }
    g_ctype_tab[1 + c] = f;
    g_tolower_tab[1 + c] = (short)((c < 128) ? tolower(c) : c);
    g_toupper_tab[1 + c] = (short)((c < 128) ? toupper(c) : c);
  }
}

/* ============================ misc small ones =============================== */
static int g_h_errno;
int *b___get_h_errno(void) { return &g_h_errno; }

const char *b_gai_strerror(int err) { return "getaddrinfo failed (offline)"; }

/* The Google "blocking region" markers are weak hooks for a tracing profiler. */
void b___google_potentially_blocking_region_begin(void) {}
void b___google_potentially_blocking_region_end(void) {}
