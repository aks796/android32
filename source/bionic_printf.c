/* bionic_printf.c -- the engine's sprintf family, NULL-safe as bionic's.
 *
 * Bionic prints "(null)" for a NULL %s; devkitARM's newlib is built without
 * that check and runs strlen(NULL) instead. Unity formats NULL strings in some
 * error messages -- Ryujinx 2026-09-29: three of its loading threads died at
 * once in newlib's _svfprintf_r -> strlen(0). So the engine's snprintf,
 * sprintf, vsnprintf, vsprintf and vasprintf come here: the arguments are
 * walked (by the format's own conversions, on a copy of the va_list) and a
 * conversion whose string is NULL is printed as "(null)" by rewriting it to
 * take a precision-bounded read of a real "(null)"... which the argument
 * list cannot carry, so the spec becomes "%.0s" (newlib reads no byte for a
 * zero precision) preceded by the literal text "(null)". Formats without a
 * NULL string -- nearly all -- go straight to newlib. MIT.
 */
#define _GNU_SOURCE 1
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

size_t dcr_readable(uint32_t p, size_t want);                  /* exc_handler.c */
int dcr_is_code_addr(uint32_t a);
const char *dcr_addr_name(uint32_t a, char *buf, size_t cap);

#define FMT_MAX 1024

/* Walk `fmt` over a copy of `ap`. Returns the number of %s conversions whose
 * argument is NULL, and writes a safe rewrite of the format to out (only
 * valid when the result is > 0 and the format fits). */
static int null_strings(const char *fmt, va_list ap0, char *out, size_t cap) {
  va_list ap;
  va_copy(ap, ap0);
  int nulls = 0, bad = 0;
  size_t o = 0;
  int fits = 1;
  for (const char *p = fmt; *p;) {
    if (*p != '%') {
      if (o + 1 < cap) out[o++] = *p; else fits = 0;
      p++;
      continue;
    }
    const char *spec = p++;
    if (*p == '%') {
      if (o + 2 < cap) { out[o++] = '%'; out[o++] = '%'; } else fits = 0;
      p++;
      continue;
    }
    /* %n$ positional forms: not walked (Unity does not use them); leave all */
    const char *q = p;
    while (*q >= '0' && *q <= '9') q++;
    if (*q == '$') {
      va_end(ap);
      return 0;
    }
    while (*p && strchr("-+ #0'", *p)) p++;
    if (*p == '*') { (void)va_arg(ap, int); p++; }
    else while (*p >= '0' && *p <= '9') p++;
    if (*p == '.') {
      p++;
      if (*p == '*') { (void)va_arg(ap, int); p++; }
      else while (*p >= '0' && *p <= '9') p++;
    }
    int lng = 0; /* 0 int, 1 long, 2 long long */
    for (;; p++) {
      if (*p == 'h') continue;
      if (*p == 'l') { lng++; continue; }
      if (*p == 'q' || *p == 'j' || *p == 'L') { lng = 2; continue; }
      if (*p == 'z' || *p == 't') { lng = 1; continue; }
      break;
    }
    const char c = *p ? *p++ : 0;
    int is_null_s = 0;
    switch (c) {
    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
      if (lng >= 2) (void)va_arg(ap, long long); else (void)va_arg(ap, int);
      break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A':
      (void)va_arg(ap, double);
      break;
    case 's': {
      const char *sp = va_arg(ap, const char *);
      /* NULL, or not a pointer at all (a small number): "(null)" */
      if ((uintptr_t)sp < 0x10000 || dcr_readable((uint32_t)(uintptr_t)sp, 1) == 0) {
        is_null_s = 1;
        nulls++;
        if (sp)
          bad = 1;
      }
      break;
    }
    case 'p': case 'n':
      (void)va_arg(ap, void *);
      break;
    default:
      break; /* unknown: copied as is */
    }
    if (is_null_s) {
      static const char k[] = "(null)%.0s";
      if (o + sizeof k - 1 < cap) { memcpy(out + o, k, sizeof k - 1); o += sizeof k - 1; } else fits = 0;
    } else {
      size_t n = (size_t)(p - spec);
      if (o + n < cap) { memcpy(out + o, spec, n); o += n; } else fits = 0;
    }
  }
  va_end(ap);
  if (cap)
    out[o < cap ? o : cap - 1] = 0;
  if (nulls && !fits)
    return -1;
  if (nulls) {
    static int logged;
    if (logged++ < 8) {
      /* who: return addresses found on this stack (module + offset) */
      char who[200] = "";
      size_t w = 0;
      const uint32_t *sp = (const uint32_t *)__builtin_frame_address(0);
      size_t n = dcr_readable((uint32_t)(uintptr_t)sp, 0x1000) / 4;
      for (size_t i = 0, found = 0; i < n && found < 6 && w + 40 < sizeof who; i++) {
        uint32_t v = sp[i];
        if (!dcr_is_code_addr(v & ~1u) || (v & 3) == 2)
          continue;
        char nm[64];
        dcr_addr_name(v & ~1u, nm, sizeof nm);
        if (!strncmp(nm, "dcrsea_nx", 9))
          continue; /* this program's own frames (printf) */
        w += (size_t)snprintf(who + w, sizeof who - w, " %s", nm);
        found++;
      }
      debugPrintf("[printf] %s string printed as (null) (format \"%.120s\"); stack:%s\n", bad ? "an invalid" : "a NULL", fmt, who);
    }
  }
  return nulls;
}

/* fmt, or a NULL-safe rewrite of it in buf (bionic_stdio.c's fprintf family) */
const char *b_safe_format(const char *fmt, va_list ap, char *buf, size_t cap) {
  return fmt && null_strings(fmt, ap, buf, cap) > 0 ? buf : fmt;
}

int b_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
  char safe[FMT_MAX];
  if (fmt && null_strings(fmt, ap, safe, sizeof safe) > 0)
    return vsnprintf(buf, n, safe, ap);
  return vsnprintf(buf, n, fmt, ap);
}

int b_vsprintf(char *buf, const char *fmt, va_list ap) {
  char safe[FMT_MAX];
  if (fmt && null_strings(fmt, ap, safe, sizeof safe) > 0)
    return vsprintf(buf, safe, ap);
  return vsprintf(buf, fmt, ap);
}

int b_vasprintf(char **out, const char *fmt, va_list ap) {
  char safe[FMT_MAX];
  if (fmt && null_strings(fmt, ap, safe, sizeof safe) > 0)
    return vasprintf(out, safe, ap);
  return vasprintf(out, fmt, ap);
}

int b_snprintf(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = b_vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

int b_sprintf(char *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = b_vsprintf(buf, fmt, ap);
  va_end(ap);
  return r;
}

/* Every string printf of newlib -- sprintf, snprintf, vsnprintf, vasprintf,
 * whoever calls them (the engine, Mono, FMOD, this program) -- ends in
 * _svfprintf_r, and every FILE printf in _vfprintf_r: the link wraps both
 * (Makefile: --wrap), so a NULL %s is safe on any path. */
struct _reent;
int __real__svfprintf_r(struct _reent *r, FILE *fp, const char *fmt, va_list ap);
int __real__vfprintf_r(struct _reent *r, FILE *fp, const char *fmt, va_list ap);
int __wrap__svfprintf_r(struct _reent *r, FILE *fp, const char *fmt, va_list ap) {
  char safe[FMT_MAX];
  if (fmt && null_strings(fmt, ap, safe, sizeof safe) > 0)
    fmt = safe;
  return __real__svfprintf_r(r, fp, fmt, ap);
}
int __wrap__vfprintf_r(struct _reent *r, FILE *fp, const char *fmt, va_list ap) {
  char safe[FMT_MAX];
  if (fmt && null_strings(fmt, ap, safe, sizeof safe) > 0)
    fmt = safe;
  return __real__vfprintf_r(r, fp, fmt, ap);
}
