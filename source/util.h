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

/* debug.log under the game root; also mirrored to svcOutputDebugString. */
void debugPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_init(const char *root);
void log_set_quiet(int quiet);   /* 1: keep lines in the RAM ring (during play) */
void log_flush_ring(void);        /* write the ring to debug.log (safe points) */
size_t log_ring_tail(char *dst, size_t cap); /* unflushed log tail, for crash reports */
void log_console_open(void);      /* the boot console, blank until log_console_show_text */
void log_console_show_text(void); /* mirror the log on the console */
int log_console_active(void);
void log_console_close(void);     /* the renderer takes the window, for good */
void log_console_progress(const char *what, int permille); /* setup work: the green bar */
void log_console_update(void);

/* 1 when the kernel refused the pseudo-handle at start-up (emulator). */
int dcr_is_emulator(void);

/* CPU boost for the single-threaded load path (FastLoad clocks). */

#endif /* DCR_UTIL_H */
