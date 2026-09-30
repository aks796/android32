/* bionic_extra.c -- the rarer bionic imports.
 *
 * Mostly those of libraries built with a recent NDK against libc++, whose
 * locale layer calls the POSIX-2008 locale functions (newlocale / uselocale /
 * the *_l variants / localeconv), whose std::filesystem calls the *at()
 * family, and whose _FORTIFY_SOURCE build calls the __*_chk checks. bionic has
 * one locale, "C.UTF-8", and so do these: every locale is the same, the *_l
 * forms ignore the locale argument, and localeconv() is the C locale's. The
 * rest are engines' odds and ends (rlimits, syslog-style logging, basename,
 * fnmatch, memrchr, getauxval). MIT.
 */
#include <ctype.h>
#include <errno.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <wchar.h>
#include <wctype.h>

#include "bionic.h"
#include "error.h"
#include "util.h"

int b_open(const char *path, int lflags, ...);
int b_unlink(const char *path);
int b_rmdir(const char *path);
int b_statfs(const char *path, struct b_statfs *out);
size_t b_strftime(char *s, size_t max, const char *fmt, const void *btm);

/* ================================================================ locale */
static int g_c_locale; /* every locale_t handed out points here */
#define B_LC_GLOBAL_LOCALE ((void *)-1L)
static __thread void *t_locale = B_LC_GLOBAL_LOCALE;

void *b_newlocale(int mask, const char *name, void *base) { return &g_c_locale; }
void b_freelocale(void *l) {}
void *b_uselocale(void *l) {
  void *old = t_locale;
  if (l)
    t_locale = l;
  return old;
}

/* bionic's struct lconv (10 strings, then 14 chars) for the C locale */
struct b_lconv {
  char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol,
      *mon_decimal_point, *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
  char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes,
      n_sep_by_space, p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space,
      int_n_cs_precedes, int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};
static char s_dot[] = ".", s_empty[] = "";
static struct b_lconv g_lconv = {s_dot,   s_empty, s_empty, s_empty, s_empty,
                                 s_empty, s_empty, s_empty, s_empty, s_empty,
                                 127,     127,     127,     127,     127,
                                 127,     127,     127,     127,     127,
                                 127,     127,     127,     127};
struct b_lconv *b_localeconv(void) { return &g_lconv; }

int b_iswalpha_l(wint_t c, void *l) { return iswalpha(c); }
int b_iswblank_l(wint_t c, void *l) { return iswblank(c); }
int b_iswcntrl_l(wint_t c, void *l) { return iswcntrl(c); }
int b_iswdigit_l(wint_t c, void *l) { return iswdigit(c); }
int b_iswlower_l(wint_t c, void *l) { return iswlower(c); }
int b_iswprint_l(wint_t c, void *l) { return iswprint(c); }
int b_iswpunct_l(wint_t c, void *l) { return iswpunct(c); }
int b_iswspace_l(wint_t c, void *l) { return iswspace(c); }
int b_iswupper_l(wint_t c, void *l) { return iswupper(c); }
int b_iswxdigit_l(wint_t c, void *l) { return iswxdigit(c); }
wint_t b_towlower_l(wint_t c, void *l) { return towlower(c); }
wint_t b_towupper_l(wint_t c, void *l) { return towupper(c); }
int b_strcoll_l(const char *a, const char *b, void *l) { return strcoll(a, b); }
size_t b_strxfrm_l(char *d, const char *s, size_t n, void *l) { return strxfrm(d, s, n); }
int b_wcscoll_l(const wchar_t *a, const wchar_t *b, void *l) { return wcscoll(a, b); }
size_t b_wcsxfrm_l(wchar_t *d, const wchar_t *s, size_t n, void *l) { return wcsxfrm(d, s, n); }
long long b_strtoll_l(const char *s, char **e, int base, void *l) {
  long long r = strtoll(s, e, base);
  b_fix_errno();
  return r;
}
unsigned long long b_strtoull_l(const char *s, char **e, int base, void *l) {
  unsigned long long r = strtoull(s, e, base);
  b_fix_errno();
  return r;
}
/* long double is double on ARM EABI, for both C libraries */
long double b_strtold_l(const char *s, char **e, void *l) { return strtold(s, e); }

/* =============================================================== strings */
int b_strerror_r(int e, char *buf, size_t n) { /* the POSIX (int) form */
  if (!buf || !n)
    return L_ERANGE;
  snprintf(buf, n, "%s", strerror(e)); /* the numbers below 35 agree; above, a close text */
  return 0;
}

char *b_basename(const char *path) {
  static __thread char buf[256];
  if (!path || !*path)
    return strcpy(buf, ".");
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] == '/')
    n--;
  size_t s = n;
  while (s > 0 && path[s - 1] != '/')
    s--;
  size_t len = n - s < sizeof buf - 1 ? n - s : sizeof buf - 1;
  memcpy(buf, path + s, len);
  buf[len] = 0;
  if (!len)
    strcpy(buf, "/");
  return buf;
}

/* fnmatch: *, ?, [...] (with ! or ^ and ranges), \ escapes. Bionic's flag
 * values are BSD's, not glibc's. */
#define B_FNM_NOESCAPE 1
#define B_FNM_PATHNAME 2 /* '/' only matched literally */
static int fnm(const char *p, const char *s, int flags) {
  for (;; p++, s++) {
    switch (*p) {
    case 0:
      return *s ? 1 : 0;
    case '?':
      if (!*s || ((flags & B_FNM_PATHNAME) && *s == '/'))
        return 1;
      break;
    case '*':
      while (*p == '*')
        p++;
      if (!*p)
        return (flags & B_FNM_PATHNAME) && strchr(s, '/') ? 1 : 0;
      for (; *s; s++) {
        if (!fnm(p, s, flags))
          return 0;
        if ((flags & B_FNM_PATHNAME) && *s == '/')
          return 1;
      }
      return fnm(p, s, flags);
    case '[': {
      if (!*s)
        return 1;
      const char *q = p + 1;
      int neg = *q == '!' || *q == '^', hit = 0;
      if (neg)
        q++;
      for (int first = 1; *q && (first || *q != ']'); q++, first = 0) {
        if (q[1] == '-' && q[2] && q[2] != ']') {
          if ((unsigned char)*s >= (unsigned char)q[0] && (unsigned char)*s <= (unsigned char)q[2])
            hit = 1;
          q += 2;
        } else if (*q == *s) {
          hit = 1;
        }
      }
      if (*q != ']' || hit == neg)
        return 1;
      p = q;
      break;
    }
    case '\\':
      if (!(flags & B_FNM_NOESCAPE) && p[1])
        p++;
      /* fall through */
    default:
      if (*p != *s)
        return 1;
    }
  }
}
int b_fnmatch(const char *pattern, const char *s, int flags) {
  return (!pattern || !s) ? 1 : fnm(pattern, s, flags);
}

void b_sincos(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}
void b_sincosf(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

/* ================================================================ memory */
int b_posix_memalign(void **out, size_t align, size_t size) {
  if (!out || align < sizeof(void *) || (align & (align - 1)))
    return L_EINVAL;
  void *p = memalign(align, size ? size : 1);
  if (!p)
    return L_ENOMEM;
  *out = p;
  return 0;
}
int b_getpagesize(void) { return 0x1000; }

/* ============================================================= FORTIFY */
typedef struct { uint32_t bits[32]; } b_fd_set;
void b___FD_SET_chk(int fd, b_fd_set *set, size_t size) {
  if (fd >= 0 && (size_t)fd < size * 8)
    set->bits[fd / 32] |= 1u << (fd % 32);
}
int b___FD_ISSET_chk(int fd, const b_fd_set *set, size_t size) {
  return fd >= 0 && (size_t)fd < size * 8 && (set->bits[fd / 32] >> (fd % 32)) & 1;
}
char *b___strncpy_chk2(char *dst, const char *src, size_t n, size_t dst_len, size_t src_len) {
  if (n > dst_len)
    fatal_error("__strncpy_chk2: %u bytes into %u (from %p)", (unsigned)n, (unsigned)dst_len,
                __builtin_return_address(0));
  return strncpy(dst, src, n);
}
int b___vsnprintf_chk(char *dst, size_t n, int flags, size_t dst_len, const char *fmt, va_list ap) {
  if (n > dst_len)
    fatal_error("__vsnprintf_chk: %u bytes into %u (from %p)", (unsigned)n, (unsigned)dst_len,
                __builtin_return_address(0));
  return vsnprintf(dst, n, fmt, ap);
}

/* ============================================================ filesystem */
#define B_AT_FDCWD (-100)
#define B_AT_REMOVEDIR 0x200

int b_openat(int dirfd, const char *path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  int mode = va_arg(ap, int);
  va_end(ap);
  if (dirfd != B_AT_FDCWD && path && path[0] != '/') {
    b_set_errno(L_ENOSYS);
    return -1;
  }
  return b_open(path, flags, mode);
}
int b_unlinkat(int dirfd, const char *path, int flags) {
  if (dirfd != B_AT_FDCWD && path && path[0] != '/') {
    b_set_errno(L_ENOSYS);
    return -1;
  }
  return (flags & B_AT_REMOVEDIR) ? b_rmdir(path) : b_unlink(path);
}
void *b_fdopendir(int fd) {
  b_set_errno(L_ENOSYS);
  return NULL;
}
/* FAT on the SD card: no permissions, times as the filesystem keeps them, no links */
int b_fchmod(int fd, unsigned mode) { return 0; }
int b_fchmodat(int dirfd, const char *path, unsigned mode, int flags) { return 0; }
int b_utimensat(int dirfd, const char *path, const void *times, int flags) { return 0; }
int b_link(const char *a, const char *b) {
  b_set_errno(L_EPERM);
  return -1;
}
int b_symlink(const char *a, const char *b) {
  b_set_errno(L_EPERM);
  return -1;
}
int b_mknod(const char *path, unsigned mode, unsigned dev) {
  b_set_errno(L_EPERM);
  return -1;
}
long b_pathconf(const char *path, int name) {
  return name == 4 /* _PC_PATH_MAX */ ? 4096 : 255;
}

/* bionic's 32-bit struct statvfs: 11 longs, 44 bytes (fsblkcnt_t and
 * fsfilcnt_t are unsigned long there, and the reserved tail is LP64 only).
 * libc++'s std::filesystem::__space zeroes 44 bytes and reads f_frsize at 4
 * and f_blocks/f_bfree/f_bavail as words at 8, 12, 16. */
struct b_statvfs {
  unsigned long f_bsize, f_frsize;
  unsigned long f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
  unsigned long f_fsid, f_flag, f_namemax;
};
_Static_assert(sizeof(struct b_statvfs) == 44, "bionic arm32 struct statvfs");
int b_statvfs(const char *path, struct b_statvfs *out) {
  struct b_statfs fs;
  if (b_statfs(path, &fs) != 0 || !out)
    return -1;
  memset(out, 0, sizeof *out);
  out->f_bsize = fs.f_bsize;
  out->f_frsize = fs.f_frsize;
  out->f_blocks = fs.f_blocks;
  out->f_bfree = fs.f_bfree;
  out->f_bavail = fs.f_bavail;
  out->f_files = fs.f_files;
  out->f_ffree = out->f_favail = fs.f_ffree;
  out->f_namemax = fs.f_namelen;
  return 0;
}

/* ================================================================== misc */
struct b_rlimit { unsigned long cur, max; };
int b_getrlimit(int res, struct b_rlimit *r) {
  if (r)
    r->cur = r->max = ~0ul; /* RLIM_INFINITY */
  return 0;
}
int b_setrlimit(int res, const struct b_rlimit *r) { return 0; }

void *b_getservbyname(const char *name, const char *proto) { return NULL; } /* offline */

void b_android_set_abort_message(const char *msg) {
  debugPrintf("[abort] %s\n", msg ? msg : "");
  log_flush_ring();
}

void b_openlog(const char *ident, int opt, int facility) {}
void b_closelog(void) {}
void b_syslog(int prio, const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  debugPrintf("[syslog] %s\n", line);
}

void *b_memrchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  while (n--)
    if (*--p == (unsigned char)c)
      return (void *)p;
  return NULL;
}

/* A CPU capability probe (BoringSSL's, libraries' cpu-features): a
 * Cortex-A57 in AArch32 state -- NEON, VFPv4, idiv; AES, PMULL, SHA1, SHA2,
 * CRC32. The same values the synthetic /proc/self/auxv carries (bionic_io.c). */
unsigned long b_getauxval(unsigned long type) {
  switch (type) {
  case 16: return 0x003FB0D6ul; /* AT_HWCAP */
  case 26: return 0x1Ful;       /* AT_HWCAP2 */
  case 6: return 0x1000ul;      /* AT_PAGESZ */
  default: return 0;
  }
}
