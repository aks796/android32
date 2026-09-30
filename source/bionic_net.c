/* bionic_net.c -- sockets and name resolution, offline.
 *
 * PvZ TV Touch plays offline (as did Crossy Road, where this comes from); what
 * wants the network is ads, analytics, cloud save and store SDKs, most of
 * which are Java that does not run here. The port presents a device with no connectivity: sockets can be
 * created and closed (some code treats socket() failure as fatal) but never
 * connect; name lookups fail cleanly. The address-conversion helpers are pure
 * functions and are implemented for real. MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "util.h"

#define SOCK_FD_BASE 0x5000
#define SOCK_FD_MAX 128
static uint8_t g_sock_used[SOCK_FD_MAX];
static Mutex g_sock_lock;

int b_is_socket_fd(int fd) {
  return fd >= SOCK_FD_BASE && fd < SOCK_FD_BASE + SOCK_FD_MAX && g_sock_used[fd - SOCK_FD_BASE];
}

int b_socket(int domain, int type, int proto) {
  mutexLock(&g_sock_lock);
  for (int i = 0; i < SOCK_FD_MAX; i++)
    if (!g_sock_used[i]) {
      g_sock_used[i] = 1;
      mutexUnlock(&g_sock_lock);
      return SOCK_FD_BASE + i;
    }
  mutexUnlock(&g_sock_lock);
  b_set_errno(L_EMFILE);
  return -1;
}

/* close() of a socket fd comes through here from b_close's caller table. */
int b_socket_close(int fd) {
  if (!b_is_socket_fd(fd))
    return -1;
  g_sock_used[fd - SOCK_FD_BASE] = 0;
  return 0;
}

#define OFFLINE(ret)             \
  do {                           \
    b_set_errno(L_ENETUNREACH);  \
    return ret;                  \
  } while (0)

int b_connect(int fd, const void *addr, unsigned len) { OFFLINE(-1); }
int b_bind(int fd, const void *addr, unsigned len) { return 0; }
int b_listen(int fd, int backlog) { return 0; }
int b_accept(int fd, void *addr, unsigned *len) { b_set_errno(L_EAGAIN); return -1; }
ssize_t b_send(int fd, const void *b, size_t n, int f) { OFFLINE(-1); }
ssize_t b_sendto(int fd, const void *b, size_t n, int f, const void *a, unsigned l) { OFFLINE(-1); }
ssize_t b_sendmsg(int fd, const void *m, int f) { OFFLINE(-1); }
ssize_t b_recv(int fd, void *b, size_t n, int f) { b_set_errno(L_ENOTCONN); return -1; }
ssize_t b_recvfrom(int fd, void *b, size_t n, int f, void *a, unsigned *l) { b_set_errno(L_EAGAIN); return -1; }
ssize_t b_recvmsg(int fd, void *m, int f) { b_set_errno(L_EAGAIN); return -1; }
int b_shutdown(int fd, int how) { return 0; }
int b_setsockopt(int fd, int lvl, int opt, const void *v, unsigned l) { return 0; }
int b_getsockopt(int fd, int lvl, int opt, void *v, unsigned *l) {
  if (v && l && *l >= 4)
    *(int *)v = 0;
  return 0;
}
int b_getsockname(int fd, void *a, unsigned *l) {
  if (a && l)
    memset(a, 0, *l);
  return 0;
}
int b_getpeername(int fd, void *a, unsigned *l) { b_set_errno(L_ENOTCONN); return -1; }

/* ---------------------------------------------------------- name lookup */
#define B_EAI_FAIL 4
int b_getaddrinfo(const char *node, const char *svc, const void *hints, void **res) {
  if (res)
    *res = NULL;
  return B_EAI_FAIL;
}
void b_freeaddrinfo(void *res) {}
int b_getnameinfo(const void *sa, unsigned salen, char *host, unsigned hl, char *serv, unsigned sl, int f) {
  return B_EAI_FAIL;
}
int *b___get_h_errno(void);
void *b_gethostbyname(const char *name) {
  *b___get_h_errno() = 1; /* HOST_NOT_FOUND */
  return NULL;
}

/* ------------------------------------------------------- address helpers */
uint32_t b_inet_addr(const char *cp) {
  unsigned a, b, c, d;
  if (!cp || sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return 0xffffffffu;
  return (a) | (b << 8) | (c << 16) | (d << 24); /* network byte order */
}

int b_inet_aton(const char *cp, uint32_t *out) {
  uint32_t v = b_inet_addr(cp);
  if (v == 0xffffffffu && (!cp || strcmp(cp, "255.255.255.255")))
    return 0;
  if (out)
    *out = v;
  return 1;
}

char *b_inet_ntoa(uint32_t in) {
  static char buf[16];
  snprintf(buf, sizeof buf, "%u.%u.%u.%u", in & 255, (in >> 8) & 255, (in >> 16) & 255, in >> 24);
  return buf;
}

#define L_AF_INET 2
#define L_AF_INET6 10

const char *b_inet_ntop(int af, const void *src, char *dst, unsigned size) {
  if (af == L_AF_INET) {
    const uint8_t *p = src;
    if (snprintf(dst, size, "%u.%u.%u.%u", p[0], p[1], p[2], p[3]) >= (int)size) {
      b_set_errno(L_ENOSPC);
      return NULL;
    }
    return dst;
  }
  if (af == L_AF_INET6) {
    const uint8_t *p = src;
    char tmp[48];
    int o = 0;
    for (int i = 0; i < 16; i += 2)
      o += snprintf(tmp + o, sizeof tmp - o, i ? ":%x" : "%x", (p[i] << 8) | p[i + 1]);
    if ((unsigned)o >= size) {
      b_set_errno(L_ENOSPC);
      return NULL;
    }
    strcpy(dst, tmp);
    return dst;
  }
  b_set_errno(L_EAFNOSUPPORT);
  return NULL;
}

int b_inet_pton(int af, const char *src, void *dst) {
  if (af == L_AF_INET) {
    uint32_t v;
    if (!b_inet_aton(src, &v))
      return 0;
    memcpy(dst, &v, 4);
    return 1;
  }
  if (af == L_AF_INET6)
    return 0; /* no IPv6 literals are parsed offline */
  b_set_errno(L_EAFNOSUPPORT);
  return -1;
}

/* --------------------------------------------------------------- epoll */
/* Mono's socket I/O thread uses epoll; with no live sockets nothing arrives. */
int b_epoll_create(int size) { return b_socket(0, 0, 0); }
int b_epoll_ctl(int epfd, int op, int fd, void *ev) { return 0; }
int b_epoll_wait(int epfd, void *events, int maxevents, int timeout_ms) {
  if (timeout_ms > 0)
    svcSleepThread((s64)timeout_ms * 1000000);
  else if (timeout_ms < 0)
    svcSleepThread(1000000000ll);
  return 0;
}
