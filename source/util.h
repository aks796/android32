/* util.h -- small shared helpers + the debug log. MIT. */
#ifndef DCR_UTIL_H
#define DCR_UTIL_H

#include <stdint.h>
#include <stddef.h>

#define ALIGN_MEM(x, a) (((x) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#define ARRAY_SIZE(x)   ((int)(sizeof(x) / sizeof((x)[0])))

#ifndef NORETURN
#define NORETURN __attribute__((noreturn))
#endif

/* debug.log under the game root (the previous launch's is kept as
 * debug.prev.log); also mirrored to svcOutputDebugString. */
void debugPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_init(const char *root);
void log_set_quiet(int quiet);   /* 1: keep lines in the RAM ring (during play) */
void log_flush_ring(void);        /* write the ring to debug.log (safe points) */
size_t log_ring_tail(char *dst, size_t cap); /* unflushed log tail, for crash reports */
uint32_t log_lock_word(void);    /* the log lock's owner word (watchdog emergency) */

/* Every line, before it is logged (a port's test script waits for lines:
 * lab2, sonic). Called outside the log lock. */
extern void (*dcr_log_tap)(const char *line);

/* The boot console. */
void log_console_open(void);      /* the boot console, blank until log_console_show_text */
void log_console_show_text(void); /* mirror the log on the console */
int log_console_active(void);
void log_console_close(void);     /* the renderer takes the window, for good */
void log_console_update(void);

/* Setup's progress screen (0..1000): the game's name (PORT_TITLE), why
 * (PORT_SETUP_NOTE), a bar and the step. Drawn on the boot console while it
 * is open and the log is not on it; once the console is closed, by the
 * renderer a port sets (a8r: the start screen's, with GLES), if any. Drawn
 * again only when the step or the whole percent changes. */
void log_console_progress(const char *what, int permille);
typedef void (*rt_progress_fn)(const char *title, const char *note, const char *what, int permille);
void log_progress_set_renderer(rt_progress_fn fn); /* called outside the log lock */

/* 1 when the kernel refused the pseudo-handle at start-up (emulator). */
int dcr_is_emulator(void);

#endif /* DCR_UTIL_H */
