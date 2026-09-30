/* bionic_io.h -- helpers shared by the bionic fd / stdio / mmap / socket layers. */
#ifndef DCR_BIONIC_IO_H
#define DCR_BIONIC_IO_H
#include <stddef.h>
#include <sys/types.h>
#include "bionic.h"

int b_is_fake_fd(int fd);
int b_is_socket_fd(int fd);  /* bionic_net.c */
size_t b_pread_all(int fd, void *buf, size_t len, b_off64_t off);
b_off64_t b_lseek64(int fd, b_off64_t off, int whence);
ssize_t b_read(int fd, void *buf, size_t n);
int b_close(int fd);
int b_open(const char *path, int lflags, ...);
/* Which translated path an open fd refers to (truncate by name, see bionic_io.c). */
void b_track_open(int fd, const char *real, int writable);
void b_untrack_open(int fd);

#endif
