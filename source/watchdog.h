/* watchdog.h -- log flushing and hang reports (watchdog.c). MIT. */
#ifndef RT_WATCHDOG_H
#define RT_WATCHDOG_H

#include <stdint.h>

/* The watchdog thread: from here on the log ring reaches the SD card every
 * 5 s, and a stop in the frames is reported (while dcr_boot_in_focus()). */
void dcr_watchdog_start(void);

/* A count the hang report shows the rise of since the previous report
 * (rising means slow, not stuck): "audio writes", "GC suspends"... Up to 8,
 * besides "presented" (dcr_gl_frames), which is always there. read() is
 * called on the watchdog's thread and must take no lock. */
void rt_watchdog_add_counter(const char *label, uint32_t (*read)(void));

/* Required from the port: the frames the game has completed so far (most
 * ports: dcr_gl_frames(); dcr: its engine's frames). */
uint64_t dcr_boot_frames(void);

/* Weak callback: under an emulator, at each report, before the threads are
 * read (an emulator cannot pause a spinning thread for its registers). secs:
 * how long the frames have stopped. dcr: the main thread's sample and
 * stack_main.bin / jit_arena.bin, on the first report (secs < 20). */
void port_watchdog_emulator_report(unsigned secs);

#endif /* RT_WATCHDOG_H */
