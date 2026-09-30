/* util.c -- debug log + small helpers. MIT.
 *
 * Every line goes to svcOutputDebugString (visible in an attached debugger and
 * in an emulator's log) and to <root>/debug.log. The file is opened once and
 * flushed per line while booting; once the frame loop runs, log_set_quiet(1)
 * keeps lines in a RAM ring instead (the Drive Ahead port traced its
 * whole-console freezes to continuous SD-card writes during play), and
 * log_flush_ring() writes them out at safe points.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "util.h"

static Mutex g_log_lock;
static FILE *g_log_fp;
static int g_quiet;
static int g_console; /* the boot console exists (log_console_open) */
static int g_console_text; /* log lines are shown on it (log_console_show_text) */
static int g_console_retired; /* the window has been given to EGL: never take it back */

#define RING_SIZE (256 * 1024)
static char g_ring[RING_SIZE];
static size_t g_ring_head;     /* total bytes ever written */
static size_t g_ring_flushed;  /* total bytes already on the card */

void log_init(const char *root) {
  char path[512];
  snprintf(path, sizeof path, "%s/debug.log", root);
  mutexLock(&g_log_lock);
  if (!g_log_fp)
    g_log_fp = fopen(path, "w");
  mutexUnlock(&g_log_lock);
}

void log_flush_ring(void);

void log_set_quiet(int quiet) {
  /* Leaving the RAM ring: write out what it holds first, so the file stays in
   * order (the lines before a shutdown landed after it: hardware run 6). */
  if (!quiet)
    log_flush_ring();
  mutexLock(&g_log_lock);
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
    fflush(g_log_fp);
  }
  mutexUnlock(&g_log_lock);
}

void (*dcr_log_tap)(const char *line); /* lab_test.c: a scripted run's waits */

void debugPrintf(const char *fmt, ...) {
  char buf[1024];
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
 * on screen, or while first-launch setup has work to show (dcr_setup.c).
 * Otherwise the console stays blank -- it is still opened at every launch,
 * since the renderer taking over a window the console has used is the
 * sequence every hardware run has had (a never-used window read back black
 * in the emulator's GL self-test). */
void log_console_show_text(void) {
  mutexLock(&g_log_lock);
  if (g_console && !g_console_text) {
    g_console_text = 1;
    printf("Labyrinth 2 for Switch\n\n");
    consoleUpdate(NULL);
  }
  mutexUnlock(&g_log_lock);
}

int log_console_active(void) { return g_console; }

/* Setup work as the PvZ Touch port's green progress bar: the game's name,
 * what it is for, the bar and the step, centred on the 80-column console.
 * Drawn again only when the step or the whole percent changes; not while
 * the log is on screen ([debug] boot_log_on_screen). */
void log_console_progress(const char *what, int permille) {
  static char last[96];
  static int last_pct = -1;
  if (permille < 0)
    permille = 0;
  if (permille > 1000)
    permille = 1000;
  const int pct = permille / 10;
  mutexLock(&g_log_lock);
  if (g_console && !g_console_text && (pct != last_pct || strncmp(what, last, sizeof last - 1))) {
    last_pct = pct;
    snprintf(last, sizeof last, "%s", what);
    enum { COLS = 80, BAR = 56 };
    const int fill = permille * BAR / 1000;
    char bar[BAR + 1];
    for (int i = 0; i < BAR; i++)
      bar[i] = i < fill ? '#' : '-';
    bar[BAR] = 0;
    static const char title[] = "Labyrinth 2";
    static const char note[] = "Getting the game ready (after an install or an update)";
    printf("\x1b[2J");
    printf("\x1b[18;%dH\x1b[32;1m%s\x1b[0m", (COLS - (int)sizeof title + 1) / 2 + 1, title);
    printf("\x1b[20;%dH%s", (COLS - (int)sizeof note + 1) / 2 + 1, note);
    printf("\x1b[23;%dH[\x1b[32m%s\x1b[0m] %3d%%", (COLS - BAR - 7) / 2 + 1, bar, pct);
    const int wl = (int)strlen(last);
    printf("\x1b[25;%dH%s", wl < COLS ? (COLS - wl) / 2 + 1 : 1, last);
    fflush(stdout);
    consoleUpdate(NULL);
  }
  mutexUnlock(&g_log_lock);
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
  if (g_console)
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

