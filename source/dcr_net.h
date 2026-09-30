/* dcr_net.h -- real sockets for a game that needs them (PvZ's multiplayer had
 * them). Labyrinth 2 plays offline: every descriptor is the offline
 * socket model's (bionic_net.c), so none of these own anything. */
#ifndef DCR_NET_H
#define DCR_NET_H

int dcr_net_owns(int fd);
int dcr_net_close(int fd);
int dcr_net_fcntl(int fd, int cmd, long arg);
int dcr_net_ioctl(int fd, unsigned long req, void *arg);
short dcr_net_ready(int fd, short events);

#endif
