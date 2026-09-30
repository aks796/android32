/* bionic_wchar.c -- multibyte conversion and syscall().
 *
 * mbstate_t: bionic's is 4 bytes on 32-bit, newlib's is 8. Handing a game-owned
 * mbstate_t to newlib's mbrtowc would write 4 bytes past it, so conversion is
 * done here. bionic's multibyte encoding is always UTF-8 (its "C" locale too),
 * so this is a restartable UTF-8 codec keeping at most 3 pending bytes in the
 * game's 4-byte state: seq[0..2] = bytes, seq[3] = how many.
 *
 * syscall(): engines use the raw syscall entry for exactly these (found at
 * the call sites, ARM EABI numbers):
 *   Unity     241 sched_setaffinity / 242 sched_getaffinity  (tid, 4, &mask)
 *   Mono      316 inotify_init / 317 inotify_add_watch / 318 inotify_rm_watch
 *   libc++    240 futex (std::atomic::wait / notify)
 * plus gettid, which libraries commonly reach this way. MIT.
 */
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "bionic.h"
#include "bionic_pthread.h"
#include "util.h"

int b_clock_gettime(int clk, struct b_timespec *ts); /* bionic_time.c */

typedef struct { uint8_t seq[4]; } b_mbstate_t;

static b_mbstate_t g_mbrtowc_state, g_wcrtomb_state;

static int utf8_len(uint8_t b) {
  if (b < 0x80) return 1;
  if (b < 0xc2) return -1;
  if (b < 0xe0) return 2;
  if (b < 0xf0) return 3;
  if (b < 0xf5) return 4;
  return -1;
}

size_t b_mbrtowc(uint32_t *pwc, const char *s, size_t n, b_mbstate_t *ps) {
  if (!ps)
    ps = &g_mbrtowc_state;
  if (!s) {
    memset(ps, 0, sizeof *ps);
    return 0;
  }
  if (n == 0)
    return (size_t)-2;

  uint8_t buf[4];
  int have = ps->seq[3];
  if (have > 3)
    have = 0;
  memcpy(buf, ps->seq, (size_t)have);
  int need = have ? utf8_len(buf[0]) : utf8_len((uint8_t)s[0]);
  if (need < 0) {
    memset(ps, 0, sizeof *ps);
    b_set_errno(L_EILSEQ);
    return (size_t)-1;
  }
  size_t used = 0;
  while (have < need && used < n)
    buf[have++] = (uint8_t)s[used++];
  if (have < need) {
    memcpy(ps->seq, buf, (size_t)have);
    ps->seq[3] = (uint8_t)have;
    return (size_t)-2;
  }
  memset(ps, 0, sizeof *ps);

  uint32_t c;
  switch (need) {
  case 1: c = buf[0]; break;
  case 2: c = (buf[0] & 0x1fu) << 6 | (buf[1] & 0x3fu); break;
  case 3: c = (buf[0] & 0x0fu) << 12 | (buf[1] & 0x3fu) << 6 | (buf[2] & 0x3fu); break;
  default: c = (buf[0] & 0x07u) << 18 | (buf[1] & 0x3fu) << 12 | (buf[2] & 0x3fu) << 6 | (buf[3] & 0x3fu); break;
  }
  for (int i = 1; i < need; i++)
    if ((buf[i] & 0xc0) != 0x80)
      goto bad;
  if ((need == 3 && (c < 0x800 || (c >= 0xd800 && c <= 0xdfff))) || (need == 4 && (c < 0x10000 || c > 0x10ffff)))
    goto bad;
  if (pwc)
    *pwc = c;
  return c ? used : 0;
bad:
  b_set_errno(L_EILSEQ);
  return (size_t)-1;
}

size_t b_wcrtomb(char *s, uint32_t wc, b_mbstate_t *ps) {
  if (!ps)
    ps = &g_wcrtomb_state;
  memset(ps, 0, sizeof *ps);
  if (!s)
    return 1; /* wcrtomb(NULL, ...) == wcrtomb(buf, L'\0', ...) */
  if (wc < 0x80) {
    s[0] = (char)wc;
    return 1;
  }
  if (wc < 0x800) {
    s[0] = (char)(0xc0 | wc >> 6);
    s[1] = (char)(0x80 | (wc & 0x3f));
    return 2;
  }
  if (wc >= 0xd800 && wc <= 0xdfff)
    goto bad;
  if (wc < 0x10000) {
    s[0] = (char)(0xe0 | wc >> 12);
    s[1] = (char)(0x80 | ((wc >> 6) & 0x3f));
    s[2] = (char)(0x80 | (wc & 0x3f));
    return 3;
  }
  if (wc <= 0x10ffff) {
    s[0] = (char)(0xf0 | wc >> 18);
    s[1] = (char)(0x80 | ((wc >> 12) & 0x3f));
    s[2] = (char)(0x80 | ((wc >> 6) & 0x3f));
    s[3] = (char)(0x80 | (wc & 0x3f));
    return 4;
  }
bad:
  b_set_errno(L_EILSEQ);
  return (size_t)-1;
}

/* ---- the rest of the multibyte family, on the two functions above (the
 * libc++ uses them for its streams and locale facets) ---- */
static b_mbstate_t g_mbtowc_state, g_mbrlen_state, g_mbsr_state, g_wcsr_state;

int b_mbtowc(uint32_t *pwc, const char *s, size_t n) {
  if (!s) {
    memset(&g_mbtowc_state, 0, sizeof g_mbtowc_state);
    return 0; /* UTF-8 has no shift states */
  }
  size_t r = b_mbrtowc(pwc, s, n, &g_mbtowc_state);
  if (r == (size_t)-1 || r == (size_t)-2) {
    memset(&g_mbtowc_state, 0, sizeof g_mbtowc_state);
    b_set_errno(L_EILSEQ);
    return -1;
  }
  return (int)r;
}

size_t b_mbrlen(const char *s, size_t n, b_mbstate_t *ps) {
  return b_mbrtowc(NULL, s, n, ps ? ps : &g_mbrlen_state);
}

size_t b___ctype_get_mb_cur_max(void) { return 4; } /* bionic's locales are UTF-8 */

/* nms: at most this many source bytes; len: at most this many wide chars */
size_t b_mbsnrtowcs(uint32_t *dst, const char **src, size_t nms, size_t len, b_mbstate_t *ps) {
  if (!ps)
    ps = &g_mbsr_state;
  const char *s = *src;
  size_t out = 0;
  while (nms && (!dst || out < len)) {
    uint32_t wc;
    size_t r = b_mbrtowc(&wc, s, nms, ps);
    if (r == (size_t)-1) {
      *src = s;
      return (size_t)-1;
    }
    if (r == (size_t)-2) { /* incomplete at the end of nms: consumed into the state */
      s += nms;
      break;
    }
    if (r == 0) { /* the terminating NUL */
      if (dst)
        dst[out] = 0;
      if (dst)
        *src = NULL;
      return out;
    }
    if (dst)
      dst[out] = wc;
    out++;
    s += r;
    nms -= r;
  }
  if (dst)
    *src = s;
  return out;
}

size_t b_mbsrtowcs(uint32_t *dst, const char **src, size_t len, b_mbstate_t *ps) {
  return b_mbsnrtowcs(dst, src, (size_t)-1, len, ps);
}

/* nwc: at most this many source wide chars; len: at most this many bytes */
size_t b_wcsnrtombs(char *dst, const uint32_t **src, size_t nwc, size_t len, b_mbstate_t *ps) {
  if (!ps)
    ps = &g_wcsr_state;
  const uint32_t *s = *src;
  size_t out = 0;
  char buf[8];
  while (nwc--) {
    size_t r = b_wcrtomb(buf, *s, ps);
    if (r == (size_t)-1) {
      *src = s;
      return (size_t)-1;
    }
    if (dst) {
      if (out + r > len)
        break;
      memcpy(dst + out, buf, r);
    }
    if (*s == 0) {
      if (dst)
        *src = NULL;
      return out; /* the NUL is written but not counted */
    }
    out += r;
    s++;
  }
  if (dst)
    *src = s;
  return out;
}

size_t b_wcsrtombs(char *dst, const uint32_t **src, size_t len, b_mbstate_t *ps) {
  return b_wcsnrtombs(dst, src, (size_t)-1, len, ps);
}

uint32_t b_btowc(int c) { return (c == -1 || c > 0x7f || c < 0) ? 0xffffffffu /* WEOF */ : (uint32_t)c; }
int b_wctob(uint32_t c) { return c < 0x80 ? (int)c : -1; }

/* ------------------------------------------------------------- syscall */
#define NR_gettid 224
#define NR_futex 240
#define NR_sched_setaffinity 241
#define NR_sched_getaffinity 242
#define NR_inotify_init 316
#define NR_inotify_add_watch 317
#define NR_inotify_rm_watch 318

long b_syscall(long nr, ...) {
  va_list ap;
  va_start(ap, nr);
  long a1 = va_arg(ap, long), a2 = va_arg(ap, long), a3 = va_arg(ap, long);
  long a4 = va_arg(ap, long);
  va_end(ap);
  switch (nr) {
  case NR_futex: {
    /* futex(uaddr, op, val, timeout): WAIT 0 / WAKE 1, +128 PRIVATE, +256
     * CLOCK_REALTIME. WAIT's timeout is relative; WAIT_BITSET's is an
     * absolute time on MONOTONIC (REALTIME with the flag). A wait lasts one
     * slice at most anyway (dcr_futex_wait): a spurious wakeup. */
    const int op = (int)a2 & 0x7f;
    volatile uint32_t *addr = (volatile uint32_t *)a1;
    if (op == 0 || op == 9 /* WAIT_BITSET */) {
      const struct b_timespec *ts = (const struct b_timespec *)a4;
      s64 ns = ts ? (s64)ts->tv_sec * 1000000000ll + ts->tv_nsec : -1;
      if (ts && op == 9) {
        struct b_timespec now;
        b_clock_gettime((a2 & 256) ? L_CLOCK_REALTIME : L_CLOCK_MONOTONIC, &now);
        ns -= (s64)now.tv_sec * 1000000000ll + now.tv_nsec;
        if (ns < 0)
          ns = 0;
      }
      int r = dcr_futex_wait(addr, (uint32_t)a3, ns);
      if (r < 0) {
        b_set_errno(-r);
        return -1;
      }
      return 0;
    }
    if (op == 1 || op == 10 /* WAKE_BITSET */)
      return dcr_futex_wake(addr, (int)a3);
    b_set_errno(L_ENOSYS);
    return -1;
  }
  case NR_gettid:
    return b_gettid();
  case NR_sched_setaffinity:
    return 0;
  case NR_sched_getaffinity:
    /* returns the number of bytes written to the mask, as the raw syscall does */
    if (a3 && a2 >= 4) {
      memset((void *)a3, 0, (size_t)a2);
      *(uint32_t *)a3 = 0x7; /* the three application cores */
      return 4;
    }
    b_set_errno(L_EINVAL);
    return -1;
  case NR_inotify_init:
  case NR_inotify_add_watch:
  case NR_inotify_rm_watch:
    b_set_errno(L_ENOSYS); /* Mono falls back to polling FileSystemWatcher */
    return -1;
  default: {
    static int logged;
    if (logged++ < 16)
      debugPrintf("[syscall] unhandled syscall(%ld) from %p\n", nr, __builtin_return_address(0));
    b_set_errno(L_ENOSYS);
    return -1;
  }
  }
}
