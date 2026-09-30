/* bionic_stdio.c -- <stdio.h> for the bionic ABI.
 *
 * A FILE* the game holds is one of:
 *   - &__sF[0..2]   bionic's stdin/stdout/stderr (84-byte FILEs -- confirmed
 *                   in libmono: 85 uses of __sF+0xa8, 17 of __sF+0x54);
 *   - a newlib FILE* we returned from fopen()/fdopen().
 * The game never looks inside a FILE (no __srget/__swbuf imports), so a newlib
 * FILE* is safe to hand over opaque. Writes to stdout/stderr go to the log.
 *
 * Synthetic files (/proc/..., /dev/urandom, pipes) are fake fds in bionic_io.c;
 * fdopen()/fopen() on those build a newlib FILE over them with funopen(). That
 * matters because Unity's C++ ifstream goes open() -> fdopen(): the Drive Ahead
 * lineage lost a boot to a /proc/cpuinfo read that failed exactly there. MIT.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <wchar.h>

#include "bionic.h"
#include "bionic_io.h"
#include "dcr_path.h"
#include "util.h"

unsigned char b___sF[3 * B_FILE_SIZE];

typedef enum { S_NONE, S_IN, S_OUT, S_ERR } StdKind;

static StdKind std_kind(const void *fp) {
  const unsigned char *p = fp;
  if (p == &b___sF[0]) return S_IN;
  if (p == &b___sF[B_FILE_SIZE]) return S_OUT;
  if (p == &b___sF[2 * B_FILE_SIZE]) return S_ERR;
  return S_NONE;
}

static FILE *real_fp(void *fp) {
  switch (std_kind(fp)) {
  case S_IN: return stdin;
  case S_OUT: return stdout;
  case S_ERR: return stderr;
  default: return (FILE *)fp;
  }
}

/* A line repeated back to back is logged once, then counted: the mod prints
 * "SubstrateHookFunctionThumb" once per hook, some 550 times at start-up, and
 * a log line costs a card write plus, on the boot console, a display frame
 * (the first hardware run spent 9 s of its hook phase on them). */
static Mutex g_rep_lock;
static char g_rep_line[256];
static StdKind g_rep_kind;
static unsigned g_rep_count;

static const char *stream_name(StdKind k) { return k == S_OUT ? "stdout" : "stderr"; }

static void rep_flush_locked(void) {
  if (g_rep_count)
    debugPrintf("[%s] (the line above %u more time%s)\n", stream_name(g_rep_kind), g_rep_count,
                g_rep_count > 1 ? "s" : "");
  g_rep_count = 0;
}

void dcr_stdio_flush_repeats(void) {
  mutexLock(&g_rep_lock);
  rep_flush_locked();
  g_rep_line[0] = 0;
  mutexUnlock(&g_rep_lock);
}

static void log_stream(StdKind k, const char *s, size_t n) {
  char line[1024];
  if (n >= sizeof line)
    n = sizeof line - 1;
  memcpy(line, s, n);
  line[n] = 0;
  mutexLock(&g_rep_lock);
  if (k == g_rep_kind && g_rep_line[0] && !strcmp(line, g_rep_line)) {
    g_rep_count++;
    mutexUnlock(&g_rep_lock);
    return;
  }
  rep_flush_locked();
  if (n < sizeof g_rep_line)
    memcpy(g_rep_line, line, n + 1);
  else
    g_rep_line[0] = 0;
  g_rep_kind = k;
  debugPrintf("[%s] %s%s", stream_name(k), line, (n && line[n - 1] == '\n') ? "" : "\n");
  mutexUnlock(&g_rep_lock);
}

/* -------------------------------------------------- fake-fd backed FILEs */
static int ck_read(void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n) { return (int)b_read((int)(intptr_t)c, buf, (size_t)n); }
static int ck_write(void *c, const char *buf, _READ_WRITE_BUFSIZE_TYPE n) { return (int)n; }
static fpos_t ck_seek(void *c, fpos_t off, int whence) { return (fpos_t)b_lseek64((int)(intptr_t)c, off, whence); }
static int ck_close(void *c) { return b_close((int)(intptr_t)c); }

static FILE *fake_fdopen(int fd) {
  return funopen((void *)(intptr_t)fd, ck_read, ck_write, ck_seek, ck_close);
}

/* ------------------------------------------------- game.apk, read-only */
/* The engine reads sounds and music stored in the APK through
 * Sexy::AndroidFile::InitRead: an fopen of the whole APK per file, seeked to
 * the entry. On the Switch each of those was a file-system open of the 160 MB
 * game.apk, and the loading thread spent ~90% of the load blocked in
 * fsFsOpenFile (pvz_prof, hardware run 6: 227 opens in a 190 s load). A
 * read-only fopen of game.apk is therefore a FILE of our own over the RAM
 * block cache (dcr_apkcache.c, one handle of its own): no open at all. */
long long dcr_apkcache_size(void);
const char *dcr_addr_name(uint32_t a, char *buf, size_t cap); /* exc_handler.c */
int dcr_dircache_missing(const char *real);
void dcr_dircache_forget(void);
ssize_t dcr_apkcache_read(uint64_t off, void *buf, size_t n);
int dcr_apkcache_is_apk(const char *real);

typedef struct {
  uint64_t pos, size;
} ApkFile;

static int apkf_read(void *c, char *buf, _READ_WRITE_BUFSIZE_TYPE n) {
  ApkFile *k = c;
  ssize_t r = dcr_apkcache_read(k->pos, buf, (size_t)n);
  if (r < 0) {
    errno = EIO;
    return -1;
  }
  k->pos += (uint64_t)r;
  return (int)r;
}

static fpos_t apkf_seek(void *c, fpos_t off, int whence) {
  ApkFile *k = c;
  long long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long long)k->pos : (long long)k->size;
  long long np = base + (long long)off;
  if (np < 0) {
    errno = EINVAL;
    return -1;
  }
  k->pos = (uint64_t)np;
  return (fpos_t)np;
}

static int apkf_close(void *c) {
  free(c);
  return 0;
}

static FILE *apk_fopen(void) {
  long long size = dcr_apkcache_size();
  if (size < 0)
    return NULL;
  ApkFile *k = calloc(1, sizeof *k);
  if (!k)
    return NULL;
  k->size = (uint64_t)size;
  FILE *f = funopen(k, apkf_read, NULL, apkf_seek, apkf_close);
  if (!f)
    free(k);
  return f;
}

/* ----------------------------------------------------------- open/close */
void *b_fopen(const char *path, const char *mode) {
  if (!path || !mode) {
    b_set_errno(L_EINVAL);
    return NULL;
  }
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/dev/", 5)) {
    int fd = b_open(path, L_O_RDONLY);
    return fd < 0 ? NULL : fake_fdopen(fd);
  }
  char buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  if (mode[0] == 'r' && !strchr(mode, '+') && dcr_apkcache_is_apk(real)) {
    FILE *a = apk_fopen();
    if (a) {
      static unsigned n;
      if (!n++)
        debugPrintf("[io] read-only fopen of the APK: served from the RAM cache from now on\n");
      return a;
    }
  }
  int writes = strpbrk(mode, "wa+") != NULL;
  if (!writes && dcr_dircache_missing(real)) { /* dcr_dircache.c */
    b_set_errno(L_ENOENT);
    return NULL;
  }
  u64 t0 = armGetSystemTick();
  FILE *f = fopen(real, mode);
  if (f && writes)
    dcr_dircache_forget();
  u64 ms = armTicksToNs(armGetSystemTick() - t0) / 1000000ull;
  if (dcr_path_traced(path))
    debugPrintf("[io] fopen(%s, %s) -> %p\n", path, mode, (void *)f);
  /* The loading thread spent most of a 184 s load inside fsFsOpenFile here
   * (pvz_prof, hardware run 7); name the slow opens and who makes them. */
  static unsigned slow;
  if (ms >= 20 && slow < 200) {
    char who[64];
    slow++;
    debugPrintf("[io] slow fopen %llu ms: %s (%s) -> %s, from %s\n", (unsigned long long)ms, path,
                mode, f ? "ok" : "failed",
                dcr_addr_name((uint32_t)(uintptr_t)__builtin_return_address(0), who, sizeof who));
  }
  if (!f)
    b_fix_errno();
  else
    b_track_open(fileno(f), real, strpbrk(mode, "wa+") != NULL);
  return f;
}

void *b_fdopen(int fd, const char *mode) {
  if (b_is_fake_fd(fd))
    return fake_fdopen(fd);
  FILE *f = fdopen(fd, mode);
  if (!f)
    b_fix_errno();
  return f;
}

int b_fclose(void *fp) {
  if (std_kind(fp) != S_NONE)
    return 0;
  int fd = fileno((FILE *)fp);
  if (fd >= 0)
    b_untrack_open(fd);
  return fclose((FILE *)fp);
}

/* --------------------------------------------------------------- output */
int b_vfprintf(void *fp, const char *fmt, va_list ap) {
  StdKind k = std_kind(fp);
  if (k == S_OUT || k == S_ERR) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0)
      log_stream(k, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
    return n;
  }
  return vfprintf(real_fp(fp), fmt, ap);
}

int b_fprintf(void *fp, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = b_vfprintf(fp, fmt, ap);
  va_end(ap);
  return r;
}

int b_vprintf(const char *fmt, va_list ap) { return b_vfprintf(&b___sF[B_FILE_SIZE], fmt, ap); }

int b_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = b_vprintf(fmt, ap);
  va_end(ap);
  return r;
}

int b_fputs(const char *s, void *fp) {
  StdKind k = std_kind(fp);
  if (k == S_OUT || k == S_ERR) {
    log_stream(k, s, strlen(s));
    return 1;
  }
  return fputs(s, real_fp(fp));
}

int b_puts(const char *s) {
  log_stream(S_OUT, s, strlen(s));
  return 1;
}

int b_fputc(int c, void *fp) {
  StdKind k = std_kind(fp);
  if (k == S_OUT || k == S_ERR)
    return (unsigned char)c; /* single chars to the console: dropped, not worth a log line */
  return fputc(c, real_fp(fp));
}
int b_putc(int c, void *fp) { return b_fputc(c, fp); }
int b_putchar(int c) { return (unsigned char)c; }

size_t b_fwrite(const void *p, size_t sz, size_t n, void *fp) {
  StdKind k = std_kind(fp);
  if (k == S_OUT || k == S_ERR) {
    log_stream(k, p, sz * n);
    return n;
  }
  return fwrite(p, sz, n, real_fp(fp));
}

int b_fflush(void *fp) {
  if (!fp || std_kind(fp) != S_NONE)
    return 0;
  return fflush((FILE *)fp);
}

/* ---------------------------------------------------------------- input */
size_t b_fread(void *p, size_t sz, size_t n, void *fp) {
  if (std_kind(fp) != S_NONE)
    return 0;
  return fread(p, sz, n, (FILE *)fp);
}

char *b_fgets(char *s, int n, void *fp) {
  if (std_kind(fp) != S_NONE)
    return NULL;
  return fgets(s, n, (FILE *)fp);
}

int b_getc(void *fp) { return std_kind(fp) != S_NONE ? EOF : getc((FILE *)fp); }
int b_ungetc(int c, void *fp) { return std_kind(fp) != S_NONE ? EOF : ungetc(c, (FILE *)fp); }

int b_fscanf(void *fp, const char *fmt, ...) {
  if (std_kind(fp) != S_NONE)
    return EOF;
  va_list ap;
  va_start(ap, fmt);
  int r = vfscanf((FILE *)fp, fmt, ap);
  va_end(ap);
  return r;
}

/* ---------------------------------------------------------- positioning */
int b_fseek(void *fp, long off, int whence) {
  if (std_kind(fp) != S_NONE) {
    b_set_errno(L_ESPIPE);
    return -1;
  }
  int r = fseek((FILE *)fp, off, whence);
  if (r)
    b_fix_errno();
  return r;
}

long b_ftell(void *fp) { return std_kind(fp) != S_NONE ? -1 : ftell((FILE *)fp); }
void b_clearerr(void *fp) { if (std_kind(fp) == S_NONE) clearerr((FILE *)fp); }

/* ----------------------------------------------------------- wide chars */
wint_t b_getwc(void *fp) { return std_kind(fp) != S_NONE ? WEOF : getwc((FILE *)fp); }
wint_t b_putwc(wchar_t c, void *fp) { return std_kind(fp) != S_NONE ? (wint_t)c : putwc(c, (FILE *)fp); }
wint_t b_ungetwc(wint_t c, void *fp) { return std_kind(fp) != S_NONE ? WEOF : ungetwc(c, (FILE *)fp); }

int b_feof(void *fp) { return std_kind(fp) != S_NONE ? 0 : feof((FILE *)fp); }
int b_fgetc(void *fp) { return std_kind(fp) != S_NONE ? EOF : fgetc((FILE *)fp); }
wchar_t *b_fgetws(wchar_t *s, int n, void *fp) {
  return std_kind(fp) != S_NONE ? NULL : fgetws(s, n, (FILE *)fp);
}
void b_setbuf(void *fp, char *buf) {
  if (std_kind(fp) == S_NONE)
    setvbuf((FILE *)fp, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}
/* bionic's 32-bit off_t is 32 bits (fseeko/ftello; the o64 forms are separate) */
int b_fseeko(void *fp, b_off_t off, int whence) {
  if (std_kind(fp) != S_NONE)
    return -1;
  int r = fseeko((FILE *)fp, (off_t)off, whence);
  if (r)
    b_fix_errno();
  return r;
}
b_off_t b_ftello(void *fp) {
  if (std_kind(fp) != S_NONE)
    return -1;
  off_t r = ftello((FILE *)fp);
  if (r > 0x7fffffff) {
    b_set_errno(L_EOVERFLOW);
    return -1;
  }
  return (b_off_t)r;
}

int b_setvbuf(void *fp, char *buf, int mode, size_t size) {
  if (std_kind(fp) != S_NONE)
    return 0; /* the console streams go to the log, unbuffered either way */
  return setvbuf((FILE *)fp, buf, mode, size); /* _IOFBF/_IOLBF/_IONBF: 0/1/2 in both */
}

/* No processes to spawn (Mono probes for tools such as "uname" through popen). */
void *b_popen(const char *cmd, const char *mode) {
  debugPrintf("[stdio] popen(\"%s\") refused\n", cmd ? cmd : "");
  b_set_errno(L_ENOSYS);
  return NULL;
}
int b_pclose(void *fp) {
  b_set_errno(L_ECHILD);
  return -1;
}

/* ------------------------------------------- extras (from the Angry Birds Space port) */
int b_ferror(void *fp) { return std_kind(fp) != S_NONE ? 0 : ferror((FILE *)fp); }

/* freopen of a standard stream (stdout/stderr redirection) keeps ours, as
 * b_dup2 does; any other stream is reopened at the translated path. */
void *b_freopen(const char *path, const char *mode, void *fp) {
  if (std_kind(fp) != S_NONE)
    return fp;
  if (!path || !mode) {
    b_set_errno(L_EINVAL);
    return NULL;
  }
  char buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  int fd = fileno((FILE *)fp);
  if (fd >= 0)
    b_untrack_open(fd);
  FILE *f = freopen(real, mode, (FILE *)fp);
  if (!f)
    b_fix_errno();
  else
    b_track_open(fileno(f), real, strpbrk(mode, "wa+") != NULL);
  return f;
}

/* tmpfile(): a scratch file in the app's cache folder (newlib's has no
 * temporary directory to use here). */
void *b_tmpfile(void) {
  static volatile uint32_t seq;
  char path[96];
  snprintf(path, sizeof path, "/data/data/" DCR_PACKAGE "/cache/tmp%08lx.tmp",
           (unsigned long)__atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED));
  return b_fopen(path, "w+b");
}
