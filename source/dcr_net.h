/* dcr_net.h -- sockets a port serves itself (callbacks of bionic_io.c and
 * bionic_net.c).
 *
 * The runtime's sockets are offline (bionic_net.c): they can be made and
 * closed, and never connect. A port with real sockets, or sockets of its own
 * (a local multiplayer between copies of the engine), defines these; the
 * runtime's weak defaults own nothing. Every fd a port hands out must be one
 * port_net_owns() answers 1 for, and no other file's. MIT.
 */
#ifndef DCR_NET_H
#define DCR_NET_H
#include <stddef.h>
#include <sys/types.h>

/* 1 if fd is one of the port's sockets. Asked first by close, fcntl, ioctl,
 * poll/select, bind, sendto and recvfrom. */
int port_net_owns(int fd);
int port_net_close(int fd);
int port_net_fcntl(int fd, int cmd, long arg);
int port_net_ioctl(int fd, unsigned long req, void *arg);
/* poll: the Linux revents for `events` (POLLIN 1, POLLOUT 4, ...). No futex
 * announces a socket's readiness, so a wait on one goes in 5 ms slices. */
short port_net_ready(int fd, short events);

/* socket(): an fd (>= 0) the port serves, or a negative value to let the
 * runtime make its offline socket. */
int port_net_socket(int domain, int type, int proto);
/* Called only for the port's own fds (port_net_owns). */
int port_net_bind(int fd, const void *addr, unsigned len);
ssize_t port_net_sendto(int fd, const void *b, size_t n, int flags, const void *addr, unsigned alen);
ssize_t port_net_recvfrom(int fd, void *b, size_t n, int flags, void *addr, unsigned *alen);

#endif
