/* bionic_io.h -- helpers shared by the bionic fd / stdio / mmap / socket layers,
 * and what the rest of the runtime asks bionic_io.c. MIT. */
#ifndef DCR_BIONIC_IO_H
#define DCR_BIONIC_IO_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "bionic.h"

int b_is_fake_fd(int fd);
int b_is_socket_fd(int fd);  /* bionic_net.c: one of the offline sockets */
int b_socket_close(int fd);  /* bionic_net.c: frees an offline socket */
/* `len` bytes at `off`, never moving fd's position (mmap of files, pread). */
size_t b_pread_all(int fd, void *buf, size_t len, b_off64_t off);
b_off64_t b_lseek64(int fd, b_off64_t off, int whence);
ssize_t b_read(int fd, void *buf, size_t n);
int b_close(int fd);
int b_open(const char *path, int lflags, ...);
/* Which translated path an open fd refers to (truncate by name, see bionic_io.c). */
void b_track_open(int fd, const char *real, int writable);
void b_untrack_open(int fd);

/* ---- fd activity: the futex word poll/select and the ALooper sleep on ----
 * Bumped (and woken) whenever a pipe gains data or loses its last writer, or
 * a looper is woken (android_ndk.c calls dcr_fd_activity for that). */
void dcr_fd_activity(void);
/* Sleep until activity after `seen` (from dcr_fd_seq), or timeout_ns (-1: a
 * long slice; every wait may also end early, as a spurious wakeup). */
void dcr_fd_wait(uint32_t seen, int64_t timeout_ns);
uint32_t dcr_fd_seq(void);
/* The read end of a pipe: 1 data waiting, 2 no data and no writer left
 * (hang-up), 0 neither; -1 not an open pipe's read end. */
int dcr_fd_readable(int fd);

/* The Android working directory (b_chdir); dcr_path.c resolves relative
 * paths against it. */
const char *dcr_cwd(void);

/* ---- reports ------------------------------------------------------------- */
void dcr_io_selftest(void);  /* boot check: a file open for writing can be sized */
void dcr_io_read_stats(uint64_t *calls, uint64_t *bytes, uint64_t *ticks);
void dcr_io_report_readahead(void); /* RT_IO_READAHEAD; nothing when off */
void dcr_io_report_patterns(void);  /* RT_IO_PATTERN_STATS; nothing when off */
/* A thread the port tags (its GL thread, say) has its file opens and reads
 * counted apart: dcr_io_gl_stats. */
extern __thread int dcr_io_tagged_thread;
void dcr_io_gl_stats(uint64_t *opens, uint64_t *reads, uint64_t *ticks);

#endif
