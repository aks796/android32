/* util.c -- debug log + small helpers.
 *
 * Every line goes to svcOutputDebugString (visible in an attached debugger and
 * in an emulator's log) and to <root>/debug.log. The file is opened once and
 * flushed per line while booting; once the frame loop runs, log_set_quiet(1)
 * keeps lines in a RAM ring instead (the Drive Ahead port traced its
 * whole-console freezes to continuous SD-card writes during play), and
 * log_flush_ring() writes them out at safe points.
 *
 * The file's length on the card is only committed by the FS service when the
 * file is flushed to it (fsync) or closed: a game stopped by the console
 * (HOME > close, a fatal error) left a debug.log cut a kilobyte in, whatever
 * had been written (hardware run 2026-09-26). So it is fsync()ed at every
 * flush of the ring and every 16 lines while booting (RT_LOG_FSYNC), and the
 * previous launch's log is kept as debug.prev.log.
 *
 * The printf family here is newlib's; runtime.mk wraps its core
 * (--wrap=_svfprintf_r, bionic_printf.c), so a NULL %s is as safe as on
 * bionic. MIT.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include <unistd.h>

#include "rt_settings.h"
#include "util.h"

/* RT_LOG_FSYNC: 1 commits debug.log to the card (fsync) at every flush of the
 * ring and every 16 lines while booting, so a log survives the game being
 * closed or killed; 0 only fflush()es. Every port: 1 (sonic's; the others
 * only fflush()ed before the merge). */
#ifndef RT_LOG_FSYNC
#define RT_LOG_FSYNC 1
#endif

static Mutex g_log_lock;
static FILE *g_log_fp;
static int g_quiet;
static int g_console; /* the boot console exists (log_console_open) */
static int g_console_text; /* log lines are shown on it (log_console_show_text) */
static int g_console_retired; /* the window has been given to EGL: never take it back */
static rt_progress_fn g_progress_fn; /* setup's progress once the console is gone */

/* The lock's word: 0 when free, else the owner's handle (| 0x40000000 when
 * others wait). The watchdog reads it without taking the lock. */
uint32_t log_lock_word(void) { return __atomic_load_n((uint32_t *)&g_log_lock, __ATOMIC_RELAXED); }

#define RING_SIZE (256 * 1024)
static char g_ring[RING_SIZE];
static size_t g_ring_head;     /* total bytes ever written */
static size_t g_ring_flushed;  /* total bytes already on the card */

static unsigned g_unsynced; /* lines written since the last fsync */

/* Under g_log_lock. */
static void sync_file(void) {
  if (g_log_fp) {
    fflush(g_log_fp);
    if (RT_LOG_FSYNC)
      fsync(fileno(g_log_fp));
  }
  g_unsynced = 0;
}

void log_init(const char *root) {
  char path[512], prev[512];
  snprintf(path, sizeof path, "%s/debug.log", root);
  snprintf(prev, sizeof prev, "%s/debug.prev.log", root);
  mutexLock(&g_log_lock);
  if (!g_log_fp) {
    remove(prev);
    rename(path, prev); /* the last launch's, kept */
    g_log_fp = fopen(path, "w");
  }
  mutexUnlock(&g_log_lock);
}

void log_set_quiet(int quiet) {
  /* Leaving the RAM ring: write out what it holds first, so the file stays in
   * order (the lines before a shutdown landed after it: hardware run 6). */
  if (!quiet)
    log_flush_ring();
  mutexLock(&g_log_lock);
  if (quiet)
    sync_file(); /* the boot's lines, committed */
  g_quiet = quiet;
  mutexUnlock(&g_log_lock);
}

static void ring_put(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++)
    g_ring[(g_ring_head + i) % RING_SIZE] = s[i];
  g_ring_head += n;
}

/* The newest (up to) cap-1 bytes still only in the RAM ring, NUL-terminated.
 * No lock: the crash handler calls this with the world stopped. */
size_t log_ring_tail(char *dst, size_t cap) {
  if (!cap)
    return 0;
  size_t have = g_ring_head - g_ring_flushed;
  if (have > RING_SIZE)
    have = RING_SIZE;
  if (have > cap - 1)
    have = cap - 1;
  size_t start = g_ring_head - have;
  for (size_t i = 0; i < have; i++)
    dst[i] = g_ring[(start + i) % RING_SIZE];
  dst[have] = 0;
  return have;
}

void log_flush_ring(void) {
  mutexLock(&g_log_lock);
  if (g_log_fp && g_ring_head > g_ring_flushed) {
    size_t lost = 0;
    if (g_ring_head - g_ring_flushed > RING_SIZE) {
      lost = g_ring_head - g_ring_flushed - RING_SIZE;
      g_ring_flushed = g_ring_head - RING_SIZE;
    }
    if (lost)
      fprintf(g_log_fp, "[log] %u bytes dropped from the RAM ring\n", (unsigned)lost);
    while (g_ring_flushed < g_ring_head) {
      size_t off = g_ring_flushed % RING_SIZE;
      size_t n = g_ring_head - g_ring_flushed;
      if (n > RING_SIZE - off)
        n = RING_SIZE - off;
      fwrite(g_ring + off, 1, n, g_log_fp);
      g_ring_flushed += n;
    }
    sync_file();
  }
  mutexUnlock(&g_log_lock);
}

void (*dcr_log_tap)(const char *line);

void debugPrintf(const char *fmt, ...) {
  char buf[1536]; /* Asphalt 8's profiler lines run to ~1400 */
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  if ((size_t)n >= sizeof buf)
    n = sizeof buf - 1;
  if (dcr_log_tap)
    dcr_log_tap(buf);

  mutexLock(&g_log_lock);
  /* svcOutputDebugString adds its own line break; strip ours to avoid blanks */
  size_t sn = (size_t)n;
  while (sn && (buf[sn - 1] == '\n' || buf[sn - 1] == '\r'))
    sn--;
  svcOutputDebugString(buf, sn);
  if (g_quiet) {
    ring_put(buf, (size_t)n);
  } else if (g_log_fp) {
    fwrite(buf, 1, (size_t)n, g_log_fp);
    fflush(g_log_fp);
    if (++g_unsynced >= 16)
      sync_file();
  }
  if (g_console && g_console_text) {
    fwrite(buf, 1, (size_t)n, stdout);
    if (!g_quiet)
      consoleUpdate(NULL);
  }
  mutexUnlock(&g_log_lock);
}

/* On-screen log. Until the game draws, the screen would otherwise stay black
 * on hardware; with this, a hardware run shows how far the boot got. It owns
 * the default NWindow until the renderer takes it (log_console_close). */
void log_console_open(void) {
  if (!nwindowIsValid(nwindowGetDefault()))
    return; /* no display window: stay on the SD-card log only */
  mutexLock(&g_log_lock);
  if (!g_console && !g_console_retired) {
    consoleInit(NULL);
    g_console = 1;
    consoleUpdate(NULL); /* one blank frame: the log text stays off (below) */
  }
  mutexUnlock(&g_log_lock);
}

/* Let log lines onto the open console: when config.ini asks for the boot log
 * on screen ([debug] boot_log_on_screen). Otherwise the console stays blank,
 * or shows setup's progress screen (log_console_progress) -- it is still
 * opened at every launch, since the renderer taking over a window the console
 * has used is the sequence every hardware run has had (a never-used window
 * read back black in the emulator's GL self-test). */
void log_console_show_text(void) {
  mutexLock(&g_log_lock);
  if (g_console && !g_console_text) {
    g_console_text = 1;
    printf("%s for Switch\n\n", PORT_TITLE);
    consoleUpdate(NULL);
  }
  mutexUnlock(&g_log_lock);
}

int log_console_active(void) { return g_console; }

void log_progress_set_renderer(rt_progress_fn fn) {
  mutexLock(&g_log_lock);
  g_progress_fn = fn;
  mutexUnlock(&g_log_lock);
}

/* The column that centres n characters on the 80-column console (1-based). */
static int centre(int cols, int n) { return n < cols ? (cols - n) / 2 + 1 : 1; }

/* First-run setup and updates as a progress screen (the PvZ Touch port's): the
 * game's name in green, why, a bar with the percentage, the current step below
 * it -- not the log, which goes to debug.log as always. Drawn on the boot
 * console while it is open, never while the log is on it ([debug]
 * boot_log_on_screen); once the console has handed the window over, by the
 * port's renderer (log_progress_set_renderer), called outside the log lock
 * (the Mutex is not recursive, and the renderer may log). Drawn again only
 * when the step, the whole percent or where it is drawn changes (a console
 * frame waits for the display). */
void log_console_progress(const char *what, int permille) {
  static char last[96];
  static int last_pct = -1, last_where;
  if (!what)
    what = "";
  if (permille < 0)
    permille = 0;
  if (permille > 1000)
    permille = 1000;
  const int pct = permille / 10;
  rt_progress_fn fn = NULL;
  mutexLock(&g_log_lock);
  const int where = g_console ? (g_console_text ? 0 : 1) : (g_progress_fn ? 2 : 0);
  if (where && (pct != last_pct || where != last_where || strncmp(what, last, sizeof last - 1))) {
    last_pct = pct;
    last_where = where;
    snprintf(last, sizeof last, "%s", what);
    if (where == 2) {
      fn = g_progress_fn;
    } else {
      enum { COLS = 80, BAR = 56 };
      const int fill = permille * BAR / 1000;
      char bar[BAR + 1];
      for (int i = 0; i < BAR; i++)
        bar[i] = i < fill ? '#' : '-';
      bar[BAR] = 0;
      static const char title[] = PORT_TITLE, note[] = PORT_SETUP_NOTE;
      printf("\x1b[2J");
      printf("\x1b[18;%dH\x1b[32;1m%s\x1b[0m", centre(COLS, (int)strlen(title)), title);
      printf("\x1b[20;%dH%s", centre(COLS, (int)strlen(note)), note);
      printf("\x1b[23;%dH[\x1b[32m%s\x1b[0m] %3d%%", (COLS - BAR - 7) / 2 + 1, bar, pct);
      printf("\x1b[25;%dH%s", centre(COLS, (int)strlen(last)), last);
      fflush(stdout);
      consoleUpdate(NULL);
    }
  }
  mutexUnlock(&g_log_lock);
  if (fn)
    fn(PORT_TITLE, PORT_SETUP_NOTE, what, permille);
}

/* Give the window to EGL -- permanently. The console must not take it back
 * once Mesa has used it: Mesa registers 3 buffer slots with the display's
 * buffer queue and the console only 2, and after the hand-back the queue
 * still offers the third slot, whose buffer Mesa has freed. The console's
 * third frame then fails to dequeue and libnx aborts (hardware run
 * 2026-09-23: result 0x2B59, BadGfxDequeueBuffer, after exactly two console
 * frames). From here on the log goes to the SD card only. */
void log_console_close(void) {
  mutexLock(&g_log_lock);
  if (g_console) {
    consoleExit(NULL);
    g_console = 0;
  }
  g_console_retired = 1;
  mutexUnlock(&g_log_lock);
}

/* Present the console from the main loop (lines logged during play are not
 * pushed to the screen one by one). */
void log_console_update(void) {
  mutexLock(&g_log_lock);
  if (g_console && g_console_text) /* a blank or progress screen needs no new frame */
    consoleUpdate(NULL);
  mutexUnlock(&g_log_lock);
}

/* Emulator detection. crt0_reloc.c records how text relocation went: Mesosphere
 * accepts the current-process pseudo-handle for SetProcessMemoryPermission,
 * emulators (Ryujinx) refuse it -- and they also do not enforce guest page
 * permissions. That refusal is the one signal available before anything else
 * runs, so it doubles as "which kernel am I on". */
extern volatile uint32_t __dcr_reloc_path __attribute__((visibility("hidden")));
int dcr_is_emulator(void) { return __dcr_reloc_path == 2; }
