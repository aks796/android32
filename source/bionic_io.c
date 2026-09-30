/* bionic_io.c -- POSIX file-descriptor I/O for the bionic ABI.
 *
 * Real files: the game's fd IS the newlib fd (libnx fsdev), after the path goes
 * through dcr_translate_path() and the open flags through the Linux->newlib
 * table. Offsets are converted at the boundary (bionic off_t is 32-bit, newlib
 * off_t is 64-bit) and so is every struct (stat, dirent, statfs).
 *
 * Synthetic files live at fds >= FAKE_FD_BASE and never touch the card:
 *   /proc/cpuinfo, /proc/meminfo, /proc/self/{maps,stat,status,cmdline}  in-memory text
 *   /dev/urandom, /dev/random   ENDLESS: a read loop that retries 0 forever is
 *                               how an earlier port froze once 64 KB of a
 *                               file-backed device ran dry
 *   /dev/null                   discard / EOF
 *   pipe()                      in-memory ring (Mono's thread-pool wake-up pipe)
 *
 * Callbacks: a port's own sockets (dcr_net.h, port_net_*) are asked first by
 * close, fcntl, ioctl and poll/select. Settings: RT_PROC_COMM,
 * RT_IO_READAHEAD, RT_IO_PATTERN_STATS (below). MIT.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "bionic.h"
#include "bionic_io.h"
#include "bionic_pthread.h"
#include "dcr_net.h"
#include "dcr_path.h"
#include "rt_settings.h"
#include "so_util.h"
#include "util.h"

/* The app's files folder, as the game sees it (dcr_path.c maps it to the card). */
#define APP_FILES_DIR "/data/data/" PORT_PACKAGE "/files"

/* ============================== fake fds ================================== */
#define FAKE_FD_BASE 0x4000
#define FAKE_FD_MAX 64

enum { FK_FREE = 0, FK_MEM, FK_URANDOM, FK_NULL, FK_PIPE_R, FK_PIPE_W };

typedef struct {
  Mutex lock;
  CondVar cv;
  uint8_t buf[16384];
  size_t head, tail; /* bytes ever written / read */
  int writers, readers;
} Pipe;

typedef struct {
  int kind;
  char *data; /* FK_MEM */
  size_t size, pos;
  Pipe *pipe;
  int status_flags; /* Linux O_* for F_GETFL */
} FakeFd;

static FakeFd g_fake[FAKE_FD_MAX];
static Mutex g_fake_lock;

/* Bumped (and futex-woken) whenever a pipe gains data, loses a writer, or an
 * ALooper is woken: poll/select and ALooper_pollOnce sleep on it instead of
 * polling (bionic_pthread.c's arbiter wrappers). */
volatile uint32_t g_fd_activity;
void dcr_fd_activity(void) {
  __atomic_add_fetch(&g_fd_activity, 1, __ATOMIC_RELEASE);
  dcr_futex_wake(&g_fd_activity, -1);
}
/* Sleep until fd activity after `seen`, or timeout_ns (-1: a long slice). */
void dcr_fd_wait(uint32_t seen, int64_t timeout_ns) {
  if (__atomic_load_n(&g_fd_activity, __ATOMIC_ACQUIRE) != seen)
    return;
  dcr_futex_wait(&g_fd_activity, seen, timeout_ns);
}
uint32_t dcr_fd_seq(void) { return __atomic_load_n(&g_fd_activity, __ATOMIC_ACQUIRE); }

static int fake_alloc(int kind) {
  mutexLock(&g_fake_lock);
  for (int i = 0; i < FAKE_FD_MAX; i++)
    if (g_fake[i].kind == FK_FREE) {
      memset(&g_fake[i], 0, sizeof g_fake[i]);
      g_fake[i].kind = kind;
      mutexUnlock(&g_fake_lock);
      return FAKE_FD_BASE + i;
    }
  mutexUnlock(&g_fake_lock);
  return -1;
}

static FakeFd *fake_get(int fd) {
  if (fd < FAKE_FD_BASE || fd >= FAKE_FD_BASE + FAKE_FD_MAX)
    return NULL;
  FakeFd *f = &g_fake[fd - FAKE_FD_BASE];
  return f->kind == FK_FREE ? NULL : f;
}

int b_is_fake_fd(int fd) { return fake_get(fd) != NULL; }

static void rng_fill(void *buf, size_t n) {
  randomGet(buf, n); /* libnx: CSPRNG seeded from the kernel */
}

/* ------------------------------------------------------ synthetic content */
static char *synth_cpuinfo(size_t *len) {
  static const char txt[] =
      "processor\t: 0\nBogoMIPS\t: 38.40\nFeatures\t: half thumb fastmult vfp edsp neon vfpv3 tls "
      "vfpv4 idiva idivt lpae evtstrm aes pmull sha1 sha2 crc32\nCPU implementer\t: 0x41\n"
      "CPU architecture: 8\nCPU variant\t: 0x1\nCPU part\t: 0xd07\nCPU revision\t: 1\n\n"
      "processor\t: 1\nBogoMIPS\t: 38.40\nCPU part\t: 0xd07\n\n"
      "processor\t: 2\nBogoMIPS\t: 38.40\nCPU part\t: 0xd07\n\n"
      "Hardware\t: Nintendo Switch (tegra)\nRevision\t: 0000\n";
  *len = sizeof txt - 1;
  char *p = malloc(*len);
  if (p)
    memcpy(p, txt, *len);
  return p;
}

static char *synth_meminfo(size_t *len) {
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  char *p = malloc(256);
  if (!p)
    return NULL;
  *len = (size_t)snprintf(p, 256, "MemTotal:       %u kB\nMemFree:        %u kB\nMemAvailable:   %u kB\n",
                          (unsigned)(total >> 10), (unsigned)((total - used) >> 10),
                          (unsigned)((total - used) >> 10));
  return p;
}

static char *synth_maps(size_t *len) {
  char *p = malloc(4096);
  if (!p)
    return NULL;
  *len = (size_t)so_dump_maps(p, 4096);
  return p;
}

/* The kernel's comm (/proc/self/stat, /proc/self/status): at most 15
 * characters. An Android app process is named after its package, and the
 * zygote keeps the LAST 15 characters of a longer dotted name. A port may
 * give its own (dcr: "disneycrossyro"). */
#ifndef RT_PROC_COMM
#define RT_PROC_COMM \
  (PORT_PACKAGE + (sizeof PORT_PACKAGE - 1 > 15 ? sizeof PORT_PACKAGE - 1 - 15 : 0))
#endif

static char *synth_self_stat(size_t *len) {
  char *p = malloc(256);
  if (!p)
    return NULL;
  *len = (size_t)snprintf(p, 256, "4242 (%.15s) R 1 4242 4242 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 "
                                  "8 0 0 0 0 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                          RT_PROC_COMM);
  return p;
}

static char *synth_self_status(size_t *len) {
  char *p = malloc(256);
  if (!p)
    return NULL;
  *len = (size_t)snprintf(p, 256, "Name:\t%.15s\nState:\tR (running)\nTgid:\t4242\nPid:\t4242\n"
                                  "PPid:\t1\nTracerPid:\t0\nThreads:\t8\n", RT_PROC_COMM);
  return p;
}

/* argv, NUL-separated: an app process's is its package name. One engine
 * freads it from a FILE it does not check (a NULL FILE here was a port's
 * crash on hardware, 2026-09-24). */
static char *synth_cmdline(size_t *len) {
  static const char txt[] = PORT_PACKAGE; /* the array's NUL is the terminator */
  *len = sizeof txt;
  char *p = malloc(*len);
  if (p)
    memcpy(p, txt, *len);
  return p;
}

static char *synth_cpu_range(size_t *len) {
  static const char txt[] = "0-2\n"; /* the three application cores */
  *len = sizeof txt - 1;
  char *p = malloc(*len);
  if (p)
    memcpy(p, txt, *len);
  return p;
}

/* libmain's cpu-features reads the hardware capabilities from the aux vector
 * when getauxval() is unavailable: AT_HWCAP / AT_HWCAP2 for a Cortex-A57 in
 * AArch32 state (half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt
 * vfpd32 lpae evtstrm; aes pmull sha1 sha2 crc32). */
static char *synth_auxv(size_t *len) {
  static const uint32_t v[] = {16 /* AT_HWCAP */, 0x003FB0D6u, 26 /* AT_HWCAP2 */, 0x1Fu,
                               6 /* AT_PAGESZ */, 0x1000u, 0 /* AT_NULL */, 0};
  *len = sizeof v;
  char *p = malloc(*len);
  if (p)
    memcpy(p, v, *len);
  return p;
}

/* Returns a fake fd, -1 with errno set, or -2 when `path` is not synthetic. */
static int open_synthetic(const char *path) {
  char *(*gen)(size_t *) = NULL;
  int kind = FK_MEM;
  if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random"))
    kind = FK_URANDOM;
  else if (!strcmp(path, "/dev/null"))
    kind = FK_NULL;
  else if (!strcmp(path, "/proc/cpuinfo"))
    gen = synth_cpuinfo;
  else if (!strcmp(path, "/proc/meminfo"))
    gen = synth_meminfo;
  else if (!strcmp(path, "/proc/self/maps"))
    gen = synth_maps;
  else if (!strcmp(path, "/proc/self/stat"))
    gen = synth_self_stat;
  else if (!strcmp(path, "/proc/self/status"))
    gen = synth_self_status;
  else if (!strcmp(path, "/proc/self/cmdline"))
    gen = synth_cmdline;
  else if (!strcmp(path, "/proc/self/auxv"))
    gen = synth_auxv;
  else if (!strcmp(path, "/sys/devices/system/cpu/present") ||
           !strcmp(path, "/sys/devices/system/cpu/possible") ||
           !strcmp(path, "/sys/devices/system/cpu/online"))
    gen = synth_cpu_range;
  else if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5) || !strncmp(path, "/dev/", 5)) {
    b_set_errno(L_ENOENT);
    return -1;
  } else
    return -2;

  int fd = fake_alloc(kind);
  if (fd < 0) {
    b_set_errno(L_EMFILE);
    return -1;
  }
  if (gen) {
    FakeFd *f = fake_get(fd);
    f->data = gen(&f->size);
    if (!f->data) {
      f->kind = FK_FREE;
      b_set_errno(L_ENOMEM);
      return -1;
    }
  }
  return fd;
}

/* ========================== paths of open files ===========================
 * Horizon's filesystem lets one handle at a time write a file: opening it
 * again (for writing, or at all once it is open for writing) fails -- and
 * libnx reports that result as EIO. POSIX code that reaches a file it already
 * has open BY NAME must therefore go through the handle it has. Unity sizes
 * its cache files that way: LocalFileSystemPosix::SetLength is
 * truncate(path) on the file ArchiveStorageCreator holds open, and every
 * cached AssetBundle failed with "Unable to reserve header in the archive
 * file" (hardware 2026-09-24: character models and world pieces). */
#define OPEN_PATHS 256
static struct {
  int fd, writable;
  char *path; /* translated (sdmc:) */
} g_open[OPEN_PATHS];
static Mutex g_open_lock;

void b_track_open(int fd, const char *real, int writable) {
  if (fd < 0 || !real)
    return;
  char *copy = strdup(real);
  mutexLock(&g_open_lock);
  /* The number is fresh from open(): an entry still holding it is stale (its
   * file was closed some way that bypassed b_close, e.g. fclose(fdopen())). */
  for (int i = 0; i < OPEN_PATHS; i++)
    if (g_open[i].path && g_open[i].fd == fd) {
      free(g_open[i].path);
      g_open[i].path = NULL;
    }
  for (int i = 0; i < OPEN_PATHS; i++)
    if (!g_open[i].path) {
      g_open[i].fd = fd;
      g_open[i].writable = writable;
      g_open[i].path = copy;
      copy = NULL;
      break;
    }
  mutexUnlock(&g_open_lock);
  free(copy); /* table full: this file just is not findable by name */
}

void b_untrack_open(int fd) {
  mutexLock(&g_open_lock);
  for (int i = 0; i < OPEN_PATHS; i++)
    if (g_open[i].path && g_open[i].fd == fd) {
      free(g_open[i].path);
      g_open[i].path = NULL;
    }
  mutexUnlock(&g_open_lock);
}

/* An fd we hold open for writing on `real`, or -1. FAT names are case-blind. */
static int open_writer(const char *real) {
  int fd = -1;
  mutexLock(&g_open_lock);
  for (int i = 0; i < OPEN_PATHS && fd < 0; i++)
    if (g_open[i].path && g_open[i].writable && !strcasecmp(g_open[i].path, real))
      fd = g_open[i].fd;
  mutexUnlock(&g_open_lock);
  return fd;
}

/* Boot check of the rule above on this console's own filesystem: a file open
 * for writing, sized by name -- first the way newlib alone does it (a second
 * open), then through b_truncate. */
int b_truncate(const char *path, b_off_t len); /* below */
int b_stat(const char *path, struct b_stat *out);

void dcr_io_selftest(void) {
  const char *path = APP_FILES_DIR "/.dcr_truncate_test";
  char buf[DCR_PATH_MAX], dir[DCR_PATH_MAX];
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  snprintf(dir, sizeof dir, "%s", real);
  for (char *c = strchr(dir + 1, '/'); c; c = strchr(c + 1, '/')) {
    *c = 0;
    if (c[-1] != ':')
      mkdir(dir, 0777);
    *c = '/';
  }
  int fd = b_open(path, L_O_WRONLY | L_O_CREAT | L_O_TRUNC, 0644);
  if (fd < 0) {
    debugPrintf("[io] self-test skipped: cannot create %s\n", real);
    return;
  }
  int by_name = truncate(real, 1024);
  Result by_name_rc = by_name < 0 ? fsdevGetLastResult() : 0;
  int ours = b_truncate(path, 4096);
  struct stat st;
  long size = fstat(fd, &st) == 0 ? (long)st.st_size : -1;
  struct b_stat bst; /* by name while open: how Unity checks a cached bundle's size */
  long named = b_stat(path, &bst) == 0 ? (long)bst.st_size : -1;
  b_close(fd);
  unlink(real);
  char a[48];
  if (by_name == 0)
    snprintf(a, sizeof a, "worked");
  else
    snprintf(a, sizeof a, "fails (fs result 0x%x)", by_name_rc);
  int good = ours == 0 && size == 4096 && named == 4096;
  debugPrintf("[io] self-test: a file open for writing -- sized by name alone %s; through its open "
              "handle %s (size %ld); stat by name %ld: %s\n",
              a, ours == 0 ? "worked" : "FAILED", size, named,
              good ? "OK (a file being written can be sized and checked)"
                   : "FAILED -- a file being written cannot be sized");
}

/* ================================ open ==================================== */
static int flags_linux_to_newlib(int lf) {
  int f = 0;
  switch (lf & L_O_ACCMODE) {
  case L_O_WRONLY: f = O_WRONLY; break;
  case L_O_RDWR: f = O_RDWR; break;
  default: f = O_RDONLY; break;
  }
  if (lf & L_O_CREAT) f |= O_CREAT;
  if (lf & L_O_EXCL) f |= O_EXCL;
  if (lf & L_O_TRUNC) f |= O_TRUNC;
  if (lf & L_O_APPEND) f |= O_APPEND;
  if (lf & L_O_NONBLOCK) f |= O_NONBLOCK;
  return f;
}

/* File reads, for the long-frame report (dcr_boost.c): calls, bytes, time. */
static uint64_t g_rd_calls, g_rd_bytes, g_rd_ticks;
/* The same on the thread the port tags (its GL thread), and its opens: a
 * frame's file work (dcr_io_gl_stats). */
__thread int dcr_io_tagged_thread;
static uint64_t g_gl_opens, g_gl_reads, g_gl_ticks;

/* dcr_dircache.c: missing files in the app dirs, answered without the card */
int dcr_dircache_missing(const char *real);
void dcr_dircache_forget(void);

/* fds open on the APK: their reads go through the RAM cache (dcr_apkcache.c) */
int dcr_apkcache_is_apk(const char *real);
ssize_t dcr_apkcache_read(uint64_t off, void *buf, size_t n);
#define APK_FDS 1024
static uint8_t g_apk_fd[APK_FDS];

/* READ-AHEAD for files open read-only (RT_IO_READAHEAD). One engine parses
 * its data archive's zip directory (8,218 entries, ~1.4 MB) with reads of 4
 * bytes: ~296,000 of them at start-up, each a round trip to the filesystem
 * service -- ~0.1 ms on the console, most of the 30 s to its first picture
 * (hardware 2026-09-25). So a small read is served from a buffer of the
 * file's bytes from that point on; while the reads keep following on, each
 * refill is twice the last (16 KB up to 256 KB), and a read at least as large
 * as the next refill goes straight to the caller's memory. The file position
 * is kept as if every read had gone to the card. Only for files opened
 * read-only (not the APK, which has its own cache), and forgotten at close.
 * Not for two threads reading one fd at once (neither is read()'s position).
 * a8r: 1; every other port 0. */
#ifndef RT_IO_READAHEAD
#define RT_IO_READAHEAD 0
#endif
/* Per-fd read statistics for the start-up report (dcr_io_report_patterns):
 * one lseek more per read. a8r: 1; every other port 0. */
#ifndef RT_IO_PATTERN_STATS
#define RT_IO_PATTERN_STATS 0
#endif

#if RT_IO_READAHEAD
#define RA_FDS 256
#define RA_MIN (16u << 10)
#define RA_MAX (256u << 10)
typedef struct {
  uint8_t *buf;
  uint64_t off;
  uint32_t len, next;
} ReadAhead;
static ReadAhead g_ra[RA_FDS];
static uint8_t g_ra_on[RA_FDS];
static uint64_t g_ra_hits, g_ra_fills;

static ssize_t ra_read(int fd, void *dst, size_t n) {
  ReadAhead *r = &g_ra[fd];
  off_t pos = lseek(fd, 0, SEEK_CUR); /* fsdev keeps the offset: no IPC */
  if (pos < 0)
    return -1;
  size_t done = 0;
  if (r->len && (uint64_t)pos >= r->off && (uint64_t)pos < r->off + r->len) {
    size_t k = (size_t)(r->off + r->len - (uint64_t)pos);
    if (k > n)
      k = n;
    memcpy(dst, r->buf + ((uint64_t)pos - r->off), k);
    done = k;
    g_ra_hits++;
  }
  if (done < n) {
    uint64_t p = (uint64_t)pos + done;
    size_t left = n - done;
    int follows = r->len && p == r->off + r->len;
    r->next = !follows || !r->next ? RA_MIN : r->next < RA_MAX ? r->next * 2 : RA_MAX;
    if (!r->buf && !(r->buf = malloc(RA_MAX)))
      return -1; /* no memory: the ordinary way */
    lseek(fd, (off_t)p, SEEK_SET);
    ssize_t got;
    if (left >= r->next) {
      got = read(fd, (uint8_t *)dst + done, left);
      if (got > 0)
        done += (size_t)got;
    } else {
      got = read(fd, r->buf, r->next);
      g_ra_fills++;
      if (got > 0) {
        r->off = p;
        r->len = (uint32_t)got;
        size_t k = (size_t)got < left ? (size_t)got : left;
        memcpy((uint8_t *)dst + done, r->buf, k);
        done += k;
      } else {
        r->len = 0;
      }
    }
    if (got < 0 && !done) {
      lseek(fd, pos, SEEK_SET);
      return -1;
    }
  }
  lseek(fd, pos + (off_t)done, SEEK_SET);
  return (ssize_t)done;
}
#endif

static void ra_forget(int fd) {
#if RT_IO_READAHEAD
  if (fd < 0 || fd >= RA_FDS)
    return;
  free(g_ra[fd].buf);
  memset(&g_ra[fd], 0, sizeof g_ra[fd]);
  g_ra_on[fd] = 0;
#endif
}

void dcr_io_report_readahead(void) {
#if RT_IO_READAHEAD
  if (g_ra_fills)
    debugPrintf("[io] read-ahead: %llu small reads from RAM, %llu refills from the card\n",
                (unsigned long long)g_ra_hits, (unsigned long long)g_ra_fills);
#endif
}

static ssize_t apk_read(int fd, void *buf, size_t n) {
  off_t pos = lseek(fd, 0, SEEK_CUR); /* fsdev keeps the offset: no IPC */
  if (pos < 0)
    return -1;
  ssize_t r = dcr_apkcache_read((uint64_t)pos, buf, n);
  if (r < 0)
    return -1;
  /* A regular file's read() is short only at its end, and engines read that
   * way (a short read is the end of an asset bundle): if the cache fell
   * short of that (an SD read failed), the rest comes the ordinary way. */
  size_t done = (size_t)r;
  lseek(fd, pos + (off_t)done, SEEK_SET);
  while (done < n) {
    ssize_t k = read(fd, (char *)buf + done, n - done);
    if (k <= 0)
      break;
    done += (size_t)k;
    static int logged;
    if (logged++ < 8)
      debugPrintf("[apk] cache read short at 0x%llx (%d of %u): %d more read directly\n", (unsigned long long)pos,
                  (int)r, (unsigned)n, (int)k);
  }
  return (ssize_t)done;
}

/* How each file is read (RT_IO_PATTERN_STATS; the start-up report,
 * dcr_boost.c): how many reads, how many continue where the last one ended,
 * their sizes, and how many start in a different 128 KB block than the last
 * one ended in. */
#if RT_IO_PATTERN_STATS
#define PAT_FDS 64
static struct {
  uint32_t calls, seq, newblk, size[7];
  uint64_t bytes, end;
} g_pat[PAT_FDS];

static void read_pattern(int fd, off_t pos, ssize_t r) {
  if (fd < 0 || fd >= PAT_FDS || pos < 0)
    return;
  typeof(g_pat[0]) *p = &g_pat[fd];
  p->calls++;
  p->seq += (uint64_t)pos == p->end;
  p->newblk += ((uint64_t)pos >> 17) != (p->end >> 17);
  size_t n = r > 0 ? (size_t)r : 0;
  p->size[n <= 4 ? 0 : n <= 16 ? 1 : n <= 64 ? 2 : n <= 512 ? 3 : n <= 4096 ? 4 : n <= 65536 ? 5 : 6]++;
  p->bytes += n;
  p->end = (uint64_t)pos + n;
}

static void report_pattern(int fd) {
  typeof(g_pat[0]) *p = &g_pat[fd];
  if (p->calls < 1000)
    return;
  const char *path = "?";
  mutexLock(&g_open_lock);
  for (int i = 0; i < OPEN_PATHS; i++)
    if (g_open[i].path && g_open[i].fd == fd)
      path = g_open[i].path;
  debugPrintf("[io] fd %d %s: %lu reads, %llu KB; %lu%% continue the last, %lu start in another 128 KB "
              "block; sizes <=4 %lu, <=16 %lu, <=64 %lu, <=512 %lu, <=4K %lu, <=64K %lu, more %lu\n",
              fd, path, (unsigned long)p->calls, (unsigned long long)(p->bytes >> 10),
              (unsigned long)((uint64_t)p->seq * 100 / p->calls), (unsigned long)p->newblk,
              (unsigned long)p->size[0], (unsigned long)p->size[1], (unsigned long)p->size[2],
              (unsigned long)p->size[3], (unsigned long)p->size[4], (unsigned long)p->size[5],
              (unsigned long)p->size[6]);
  mutexUnlock(&g_open_lock);
}
#endif

/* The files still open with many reads (the start-up report, dcr_boost.c);
 * closed ones are reported as they close. */
void dcr_io_report_patterns(void) {
#if RT_IO_PATTERN_STATS
  for (int fd = 0; fd < PAT_FDS; fd++)
    report_pattern(fd);
#endif
}

void dcr_io_gl_stats(uint64_t *opens, uint64_t *reads, uint64_t *ticks) {
  *opens = __atomic_load_n(&g_gl_opens, __ATOMIC_RELAXED);
  *reads = __atomic_load_n(&g_gl_reads, __ATOMIC_RELAXED);
  *ticks = __atomic_load_n(&g_gl_ticks, __ATOMIC_RELAXED);
}

void dcr_io_read_stats(uint64_t *calls, uint64_t *bytes, uint64_t *ticks) {
  *calls = __atomic_load_n(&g_rd_calls, __ATOMIC_RELAXED);
  *bytes = __atomic_load_n(&g_rd_bytes, __ATOMIC_RELAXED);
  *ticks = __atomic_load_n(&g_rd_ticks, __ATOMIC_RELAXED);
}

int b_open(const char *path, int lflags, ...) {
  int mode = 0;
  if (lflags & L_O_CREAT) {
    va_list ap;
    va_start(ap, lflags);
    mode = va_arg(ap, int);
    va_end(ap);
  }
  if (!path) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  int s = open_synthetic(path);
  if (s != -2)
    return s;

  char buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  int may_create = (lflags & L_O_CREAT) != 0;
  if (!may_create && dcr_dircache_missing(real)) {
    b_set_errno(L_ENOENT);
    return -1;
  }

  if (lflags & L_O_DIRECTORY) {
    struct stat st;
    if (stat(real, &st) != 0) {
      b_fix_errno();
      return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
      b_set_errno(L_ENOTDIR);
      return -1;
    }
    /* Directory fds are only ever fstat'ed / closed by the engine. */
    int fd = fake_alloc(FK_NULL);
    return fd < 0 ? (b_set_errno(L_EMFILE), -1) : fd;
  }

  u64 t_open = armGetSystemTick();
  int fd = open(real, flags_linux_to_newlib(lflags), mode ? mode : 0666);
  if (dcr_io_tagged_thread) {
    __atomic_fetch_add(&g_gl_opens, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_gl_ticks, armGetSystemTick() - t_open, __ATOMIC_RELAXED);
  }
  if (fd >= 0 && may_create)
    dcr_dircache_forget();
  if (fd >= 0) {
    b_track_open(fd, real, (lflags & L_O_ACCMODE) != L_O_RDONLY);
    if (fd < APK_FDS)
      g_apk_fd[fd] = (lflags & L_O_ACCMODE) == L_O_RDONLY && dcr_apkcache_is_apk(real);
    ra_forget(fd);
#if RT_IO_READAHEAD
    if (fd < RA_FDS)
      g_ra_on[fd] = (lflags & L_O_ACCMODE) == L_O_RDONLY && !(fd < APK_FDS && g_apk_fd[fd]);
#endif
#if RT_IO_PATTERN_STATS
    if (fd < PAT_FDS)
      memset(&g_pat[fd], 0, sizeof g_pat[fd]);
#endif
    /* an engine may open its data archive again and again while it plays
     * (about 5 times a second in one): the first ones are enough */
    static int traced;
    if (dcr_path_traced(path) && traced < 48) {
      traced++;
      debugPrintf("[io] open(%s) -> fd %d%s\n", path, fd, traced == 48 ? " (the last one logged)" : "");
    }
  }
  if (fd < 0) {
    int e = errno;
    b_fix_errno();
    static int logged;
    if (logged < 48) {
      logged++;
      /* EIO is libnx's word for any filesystem result it does not map, and
       * the libnx32 fork says EBUSY for 2-0xE02 ("in use"): show the result
       * itself. */
      if (e == EIO || e == EBUSY)
        debugPrintf("[io] open(%s, 0x%x) -> %s: errno %d (fs result 0x%x)\n", path, lflags, real,
                    e, fsdevGetLastResult());
      else
        debugPrintf("[io] open(%s) -> %s: errno %d\n", path, real, e);
    }
  }
  return fd;
}

/* ============================ read / write ================================ */

ssize_t b_read(int fd, void *buf, size_t n) {
  FakeFd *f = fake_get(fd);
  if (!f) {
    uint64_t t0 = armGetSystemTick();
    ssize_t r = -1;
#if RT_IO_PATTERN_STATS
    off_t pos = fd >= 0 && fd < PAT_FDS ? lseek(fd, 0, SEEK_CUR) : -1;
#endif
    if (fd >= 0 && fd < APK_FDS && g_apk_fd[fd])
      r = apk_read(fd, buf, n);
#if RT_IO_READAHEAD
    else if (fd >= 0 && fd < RA_FDS && g_ra_on[fd])
      r = ra_read(fd, buf, n);
#endif
    if (r < 0)
      r = read(fd, buf, n);
#if RT_IO_PATTERN_STATS
    read_pattern(fd, pos, r);
#endif
    u64 took = armGetSystemTick() - t0;
    __atomic_fetch_add(&g_rd_ticks, took, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_rd_calls, 1, __ATOMIC_RELAXED);
    if (dcr_io_tagged_thread) {
      __atomic_fetch_add(&g_gl_reads, 1, __ATOMIC_RELAXED);
      __atomic_fetch_add(&g_gl_ticks, took, __ATOMIC_RELAXED);
    }
    if (r > 0)
      __atomic_fetch_add(&g_rd_bytes, (uint64_t)r, __ATOMIC_RELAXED);
    if (r < 0)
      b_fix_errno();
    return r;
  }
  switch (f->kind) {
  case FK_URANDOM:
    rng_fill(buf, n);
    return (ssize_t)n;
  case FK_NULL:
    return 0;
  case FK_MEM: {
    size_t left = f->pos < f->size ? f->size - f->pos : 0;
    if (n > left)
      n = left;
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    return (ssize_t)n;
  }
  case FK_PIPE_R: {
    Pipe *p = f->pipe;
    mutexLock(&p->lock);
    while (p->head == p->tail) {
      if (!p->writers || (f->status_flags & L_O_NONBLOCK)) {
        mutexUnlock(&p->lock);
        if (!p->writers)
          return 0;
        b_set_errno(L_EAGAIN);
        return -1;
      }
      condvarWait(&p->cv, &p->lock);
    }
    size_t avail = p->head - p->tail, k = n < avail ? n : avail;
    for (size_t i = 0; i < k; i++)
      ((uint8_t *)buf)[i] = p->buf[(p->tail + i) % sizeof p->buf];
    p->tail += k;
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
    return (ssize_t)k;
  }
  default:
    b_set_errno(L_EBADF);
    return -1;
  }
}

ssize_t b_write(int fd, const void *buf, size_t n) {
  if (fd == 1 || fd == 2) {
    char line[512];
    size_t k = n < sizeof line - 1 ? n : sizeof line - 1;
    memcpy(line, buf, k);
    line[k] = 0;
    debugPrintf("[%s] %s%s", fd == 1 ? "stdout" : "stderr", line,
                (k && line[k - 1] == '\n') ? "" : "\n");
    return (ssize_t)n;
  }
  FakeFd *f = fake_get(fd);
  if (!f) {
    ssize_t r = write(fd, buf, n);
    if (r < 0)
      b_fix_errno();
    return r;
  }
  switch (f->kind) {
  case FK_NULL:
  case FK_URANDOM:
    return (ssize_t)n;
  case FK_PIPE_W: {
    Pipe *p = f->pipe;
    mutexLock(&p->lock);
    if (!p->readers) {
      mutexUnlock(&p->lock);
      b_set_errno(L_EPIPE);
      return -1;
    }
    size_t space = sizeof p->buf - (p->head - p->tail), k = n < space ? n : space;
    if (!k && (f->status_flags & L_O_NONBLOCK)) {
      mutexUnlock(&p->lock);
      b_set_errno(L_EAGAIN);
      return -1;
    }
    for (size_t i = 0; i < k; i++)
      p->buf[(p->head + i) % sizeof p->buf] = ((const uint8_t *)buf)[i];
    p->head += k;
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
    if (k)
      dcr_fd_activity();
    return (ssize_t)k;
  }
  default:
    b_set_errno(L_EBADF);
    return -1;
  }
}

struct b_iovec { void *base; size_t len; };

ssize_t b_writev(int fd, const struct b_iovec *iov, int cnt) {
  ssize_t total = 0;
  for (int i = 0; i < cnt; i++) {
    ssize_t r = b_write(fd, iov[i].base, iov[i].len);
    if (r < 0)
      return total ? total : r;
    total += r;
    if ((size_t)r < iov[i].len)
      break;
  }
  return total;
}

int b_close(int fd) {
  if (port_net_owns(fd))
    return port_net_close(fd);
  if (b_is_socket_fd(fd)) /* an offline socket: its slot comes free */
    return b_socket_close(fd);
  FakeFd *f = fake_get(fd);
  if (!f) {
    if (fd >= 0 && fd <= 2)
      return 0;
#if RT_IO_PATTERN_STATS
    if (fd < PAT_FDS)
      report_pattern(fd);
#endif
    b_untrack_open(fd);
    if (fd < APK_FDS)
      g_apk_fd[fd] = 0;
    ra_forget(fd);
    int r = close(fd);
    if (r < 0)
      b_fix_errno();
    return r;
  }
  mutexLock(&g_fake_lock);
  if (f->kind == FK_PIPE_R || f->kind == FK_PIPE_W) {
    Pipe *p = f->pipe;
    mutexLock(&p->lock);
    if (f->kind == FK_PIPE_R)
      p->readers--;
    else
      p->writers--;
    int gone = !p->readers && !p->writers;
    condvarWakeAll(&p->cv);
    mutexUnlock(&p->lock);
    if (gone)
      free(p);
    dcr_fd_activity();
  }
  free(f->data);
  f->kind = FK_FREE;
  mutexUnlock(&g_fake_lock);
  return 0;
}

/* ================================ seeking ================================= */
b_off64_t b_lseek64(int fd, b_off64_t off, int whence) {
  FakeFd *f = fake_get(fd);
  if (!f) {
    off_t r = lseek(fd, (off_t)off, whence);
    if (r < 0)
      b_fix_errno();
    return (b_off64_t)r;
  }
  if (f->kind != FK_MEM) {
    b_set_errno(L_ESPIPE);
    return -1;
  }
  b_off64_t np = whence == SEEK_SET ? off : whence == SEEK_CUR ? (b_off64_t)f->pos + off
                                                               : (b_off64_t)f->size + off;
  if (np < 0) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  f->pos = (size_t)np;
  return np;
}

b_off_t b_lseek(int fd, b_off_t off, int whence) {
  b_off64_t r = b_lseek64(fd, off, whence);
  if (r > 0x7fffffffll) {
    b_set_errno(L_EOVERFLOW);
    return -1;
  }
  return (b_off_t)r;
}

/* ================================== stat ================================== */
static void conv_stat(struct b_stat *o, const struct stat *s) {
  memset(o, 0, sizeof *o);
  o->st_dev = s->st_dev;
  o->__st_ino = (unsigned long)s->st_ino;
  o->st_ino = s->st_ino;
  o->st_mode = s->st_mode;
  o->st_nlink = s->st_nlink ? s->st_nlink : 1;
  o->st_uid = 10123;
  o->st_gid = 10123;
  o->st_size = s->st_size;
  o->st_blksize = 4096;
  o->st_blocks = (s->st_size + 511) / 512;
  o->st_atim.tv_sec = (int32_t)s->st_atime;
  o->st_mtim.tv_sec = (int32_t)s->st_mtime;
  o->st_ctim.tv_sec = (int32_t)s->st_ctime;
}

static void fake_stat(struct b_stat *o, unsigned mode, long long size) {
  memset(o, 0, sizeof *o);
  o->st_mode = mode;
  o->st_nlink = 1;
  o->st_size = size;
  o->st_blksize = 4096;
}

int b_stat(const char *path, struct b_stat *out) {
  if (!path || !out) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/dev/", 5)) {
    int fd = open_synthetic(path);
    if (fd < 0)
      return -1;
    FakeFd *f = fake_get(fd);
    fake_stat(out, (f->kind == FK_MEM ? L_S_IFREG : L_S_IFCHR) | 0444, (long long)f->size);
    b_close(fd);
    return 0;
  }
  char buf[DCR_PATH_MAX];
  struct stat st;
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  /* libnx's stat() opens the file to read its size; on a file we hold open
   * for writing that open fails (2-0xE02, see "paths of open files"), so ask
   * our handle instead. Unity checks each cached AssetBundle's size this way
   * before closing it ("Mismatching archive size ... got 0", 2026-09-24). */
  int wfd = open_writer(real);
  if (wfd < 0 && dcr_dircache_missing(real)) {
    b_set_errno(L_ENOENT);
    return -1;
  }
  if (wfd >= 0 && fstat(wfd, &st) == 0) {
    st.st_mode = (st.st_mode & ~S_IFMT) | S_IFREG;
    conv_stat(out, &st);
    return 0;
  }
  if (stat(real, &st) != 0) {
    b_fix_errno();
    if (dcr_path_traced(path))
      debugPrintf("[io] stat(%s): errno %d\n", path, errno);
    return -1;
  }
  if (dcr_path_traced(path))
    debugPrintf("[io] stat(%s): %lld bytes\n", path, (long long)st.st_size);
  conv_stat(out, &st);
  return 0;
}

int b_lstat(const char *path, struct b_stat *out) { return b_stat(path, out); }

int b_fstat(int fd, struct b_stat *out) {
  if (!out) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  FakeFd *f = fake_get(fd);
  if (f) {
    unsigned mode = f->kind == FK_MEM ? L_S_IFREG | 0444
                  : (f->kind == FK_PIPE_R || f->kind == FK_PIPE_W) ? L_S_IFIFO | 0600
                  : L_S_IFCHR | 0666;
    fake_stat(out, mode, (long long)f->size);
    return 0;
  }
  if (fd >= 0 && fd <= 2) {
    fake_stat(out, L_S_IFCHR | 0620, 0);
    return 0;
  }
  struct stat st;
  if (fstat(fd, &st) != 0) {
    b_fix_errno();
    return -1;
  }
  conv_stat(out, &st);
  return 0;
}

int b_statfs(const char *path, struct b_statfs *out) {
  if (!out) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  memset(out, 0, sizeof *out);
  out->f_type = 0xEF53;
  out->f_bsize = out->f_frsize = 4096;
  out->f_blocks = 8ull << 20; /* 32 GB, 1 GB free: plenty, and never "full" */
  out->f_bfree = out->f_bavail = 256ull << 10;
  out->f_files = 1 << 20;
  out->f_ffree = 1 << 19;
  out->f_namelen = 255;
  return 0;
}

/* ============================ path operations ============================= */
/* Unexpected filesystem failures, with the Horizon result behind libnx's EIO
 * (2-0xE02: the file, or one inside the directory, is open). */
static void log_fs_failure(const char *op, const char *path, int e) {
  static int logged;
  if (logged++ < 32)
    debugPrintf("[io] %s(%s): errno %d (fs result 0x%x)\n", op, path, e, fsdevGetLastResult());
}

#define PATH_OP(expr_newlib)                                                    \
  do {                                                                          \
    if (!path) {                                                                \
      b_set_errno(L_EFAULT);                                                    \
      return -1;                                                                \
    }                                                                           \
    char buf[DCR_PATH_MAX];                                                     \
    const char *real = dcr_translate_path(path, buf, sizeof buf);               \
    int r = (expr_newlib);                                                      \
    if (r < 0) {                                                                \
      int e = errno;                                                            \
      b_fix_errno();                                                            \
      if (e != ENOENT && e != EEXIST)                                           \
        log_fs_failure(__func__, path, e);                                      \
    }                                                                           \
    return r;                                                                   \
  } while (0)

int b_access(const char *path, int mode) {
  if (path && (!strcmp(path, "/dev/urandom") || !strcmp(path, "/proc/cpuinfo") ||
               !strcmp(path, "/proc/self/maps") || !strcmp(path, "/dev/null")))
    return 0;
  struct b_stat st;
  return b_stat(path, &st); /* newlib's access() is unreliable over fsdev */
}
int b_mkdir(const char *path, b_mode_t mode) { PATH_OP((dcr_dircache_forget(), mkdir(real, 0777))); }
int b_rmdir(const char *path) { PATH_OP((dcr_dircache_forget(), rmdir(real))); }
int b_unlink(const char *path) { PATH_OP((dcr_dircache_forget(), unlink(real))); }
int b_remove(const char *path) { PATH_OP((dcr_dircache_forget(), remove(real))); }
int b_chmod(const char *path, b_mode_t mode) { PATH_OP(((void)real, 0)); }
/* On a file we hold open for writing, through that handle (see "paths of
 * open files"); otherwise newlib opens, sizes and closes it. */
int b_truncate(const char *path, b_off_t len) {
  if (!path) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  char buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  int fd = open_writer(real);
  int r = fd >= 0 ? ftruncate(fd, (off_t)len) : truncate(real, (off_t)len);
  if (r < 0) {
    int e = errno;
    b_fix_errno();
    static int logged;
    if (logged++ < 16)
      debugPrintf("[io] truncate(%s, %ld)%s -> errno %d (fs result 0x%x)\n", path, (long)len,
                  fd >= 0 ? " via its open fd" : "", e, fsdevGetLastResult());
  }
  return r;
}

int b_rename(const char *from, const char *to) {
  char b1[DCR_PATH_MAX], b2[DCR_PATH_MAX];
  if (!from || !to) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  const char *f = dcr_translate_path(from, b1, sizeof b1);
  const char *t = dcr_translate_path(to, b2, sizeof b2);
  /* fsdev will not replace an existing target; POSIX rename does -- a file,
   * or an EMPTY directory (rmdir refuses anything else, as rename would). */
  dcr_dircache_forget();
  struct stat fst;
  if (stat(f, &fst) == 0 && S_ISDIR(fst.st_mode))
    rmdir(t);
  else
    unlink(t);
  int r = rename(f, t);
  if (r < 0) {
    int e = errno;
    b_fix_errno();
    log_fs_failure("b_rename", from, e);
  }
  return r;
}

int b_ftruncate(int fd, b_off_t len) {
  if (fake_get(fd)) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  int r = ftruncate(fd, (off_t)len);
  if (r < 0)
    b_fix_errno();
  return r;
}

int b_fsync(int fd) {
  if (fake_get(fd) || fd <= 2)
    return 0;
  int r = fsync(fd);
  if (r < 0)
    b_fix_errno();
  return r;
}

int b_flock(int fd, int op) { return 0; }

ssize_t b_readlink(const char *path, char *buf, size_t sz) {
  /* No symlinks; /proc/self/exe has no meaningful answer either. */
  b_set_errno(path && !strncmp(path, "/proc/", 6) ? L_ENOENT : L_EINVAL);
  return -1;
}

char *b_realpath(const char *path, char *resolved) {
  if (!path) {
    b_set_errno(L_EINVAL);
    return NULL;
  }
  char *out = resolved ? resolved : malloc(4096);
  if (!out)
    return NULL;
  /* Normalise "." and ".." lexically; keep the game-visible (Android) form. */
  char tmp[4096];
  snprintf(tmp, sizeof tmp, "%s", path);
  char *parts[256];
  int n = 0;
  for (char *tok = strtok(tmp, "/"); tok && n < 256; tok = strtok(NULL, "/")) {
    if (!strcmp(tok, "."))
      continue;
    if (!strcmp(tok, "..")) {
      if (n)
        n--;
      continue;
    }
    parts[n++] = tok;
  }
  size_t o = 0;
  out[0] = 0;
  for (int i = 0; i < n && o < 4000; i++)
    o += (size_t)snprintf(out + o, 4096 - o, "/%s", parts[i]);
  if (!o)
    strcpy(out, "/");
  return out;
}

static char g_cwd[DCR_PATH_MAX] = APP_FILES_DIR;
const char *dcr_cwd(void) { return g_cwd; } /* dcr_path.c resolves relative paths here */

char *b_getcwd(char *buf, size_t size) {
  if (!buf) {
    buf = malloc(size ? size : sizeof g_cwd);
    if (!buf)
      return NULL;
    if (!size)
      size = sizeof g_cwd;
  }
  if (strlen(g_cwd) + 1 > size) {
    b_set_errno(L_ERANGE);
    return NULL;
  }
  strcpy(buf, g_cwd);
  return buf;
}

int b_chdir(const char *path) {
  if (!path) {
    b_set_errno(L_EFAULT);
    return -1;
  }
  snprintf(g_cwd, sizeof g_cwd, "%s", path);
  return 0;
}

int b_mkstemp(char *tmpl) {
  if (!tmpl) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  size_t n = strlen(tmpl);
  if (n < 6 || strcmp(tmpl + n - 6, "XXXXXX")) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  static const char al[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int tries = 0; tries < 64; tries++) {
    uint8_t r[6];
    rng_fill(r, sizeof r);
    for (int i = 0; i < 6; i++)
      tmpl[n - 6 + i] = al[r[i] % (sizeof al - 1)];
    int fd = b_open(tmpl, L_O_RDWR | L_O_CREAT | L_O_EXCL, 0600);
    if (fd >= 0 || errno != L_EEXIST)
      return fd;
  }
  return -1;
}

/* ================================= fcntl ================================== */
int b_fcntl(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  long arg = va_arg(ap, long);
  va_end(ap);
  if (port_net_owns(fd))
    return port_net_fcntl(fd, cmd, arg);
  FakeFd *f = fake_get(fd);
  switch (cmd) {
  case L_F_GETFD:
    return 0;
  case L_F_SETFD:
    return 0;
  case L_F_GETFL:
    return f ? f->status_flags : L_O_RDWR;
  case L_F_SETFL:
    if (f)
      f->status_flags = (f->status_flags & L_O_ACCMODE) | ((int)arg & ~L_O_ACCMODE);
    return 0;
  case L_F_GETLK:
  case L_F_SETLK:
  case L_F_SETLKW:
    return 0;
  case L_F_DUPFD:
  case L_F_DUPFD_CLOEXEC:
    if (!f) {
      int r = dup(fd);
      if (r < 0)
        b_fix_errno();
      return r;
    }
    b_set_errno(L_EINVAL);
    return -1;
  default:
    b_set_errno(L_EINVAL);
    return -1;
  }
}

int b_dup2(int oldfd, int newfd) {
  if (newfd >= 0 && newfd <= 2)
    return newfd; /* the engine redirecting stdout/stderr: keep ours */
  if (fake_get(oldfd) || fake_get(newfd)) {
    b_set_errno(L_EBADF);
    return -1;
  }
  b_untrack_open(newfd); /* dup2 closes it */
  ra_forget(newfd);
  int r = dup2(oldfd, newfd);
  if (r < 0)
    b_fix_errno();
  else if (newfd < APK_FDS) /* a duplicate shares the offset: cache it the same way */
    g_apk_fd[newfd] = oldfd >= 0 && oldfd < APK_FDS && g_apk_fd[oldfd];
  return r;
}

int b_ioctl(int fd, unsigned long req, ...) {
  if (port_net_owns(fd)) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    return port_net_ioctl(fd, req, arg);
  }
  if (req == 0x541B) { /* FIONREAD */
    va_list ap;
    va_start(ap, req);
    int *out = va_arg(ap, int *);
    va_end(ap);
    FakeFd *f = fake_get(fd);
    if (out)
      *out = (f && f->kind == FK_PIPE_R) ? (int)(f->pipe->head - f->pipe->tail) : 0;
    return 0;
  }
  b_set_errno(L_ENOTTY);
  return -1;
}

int b_isatty(int fd) {
  b_set_errno(L_ENOTTY);
  return 0;
}

/* ================================= pipes ================================== */
int b_pipe(int fds[2]) {
  Pipe *p = calloc(1, sizeof *p);
  if (!p) {
    b_set_errno(L_ENOMEM);
    return -1;
  }
  int r = fake_alloc(FK_PIPE_R), w = fake_alloc(FK_PIPE_W);
  if (r < 0 || w < 0) {
    if (r >= 0) g_fake[r - FAKE_FD_BASE].kind = FK_FREE;
    if (w >= 0) g_fake[w - FAKE_FD_BASE].kind = FK_FREE;
    free(p);
    b_set_errno(L_EMFILE);
    return -1;
  }
  p->readers = p->writers = 1;
  fake_get(r)->pipe = p;
  fake_get(r)->status_flags = L_O_RDONLY;
  fake_get(w)->pipe = p;
  fake_get(w)->status_flags = L_O_WRONLY;
  fds[0] = r;
  fds[1] = w;
  return 0;
}

/* =================================== poll ================================= */
struct b_pollfd { int fd; short events, revents; };
#define L_POLLIN 0x001
#define L_POLLOUT 0x004
#define L_POLLHUP 0x010
#define L_POLLNVAL 0x020

/* *sockets: set if any is a port's socket (port_net_owns), whose readiness
 * no futex announces: a wait on them goes in short slices. */
#define SOCKET_SLICE_NS 5000000ll
static int poll_once(struct b_pollfd *fds, unsigned n, int *sockets) {
  int ready = 0;
  for (unsigned i = 0; i < n; i++) {
    struct b_pollfd *p = &fds[i];
    p->revents = 0;
    if (p->fd < 0)
      continue;
    if (port_net_owns(p->fd)) {
      p->revents = port_net_ready(p->fd, p->events);
      if (sockets)
        *sockets = 1;
      if (p->revents)
        ready++;
      continue;
    }
    FakeFd *f = fake_get(p->fd);
    if (!f) {
      if (b_is_socket_fd(p->fd))
        p->revents = 0; /* offline sockets never become ready */
      else
        p->revents = p->events & (L_POLLIN | L_POLLOUT); /* files are always ready */
    } else if (f->kind == FK_PIPE_R) {
      if (f->pipe->head != f->pipe->tail)
        p->revents |= p->events & L_POLLIN;
      if (!f->pipe->writers)
        p->revents |= L_POLLHUP;
    } else if (f->kind == FK_PIPE_W) {
      p->revents |= p->events & L_POLLOUT;
    } else {
      p->revents = p->events & (L_POLLIN | L_POLLOUT);
    }
    if (p->revents)
      ready++;
  }
  return ready;
}

/* For the ALooper: 1 data waiting, 2 no data and no writer left (hang-up),
 * 0 neither, -1 not the read end of an open pipe (closed, or not ours). */
int dcr_fd_readable(int fd) {
  FakeFd *f = fake_get(fd);
  if (!f || f->kind != FK_PIPE_R)
    return -1;
  if (f->pipe->head != f->pipe->tail)
    return 1;
  return f->pipe->writers ? 0 : 2;
}

int b_poll(struct b_pollfd *fds, unsigned n, int timeout_ms) {
  u64 deadline = timeout_ms < 0 ? ~0ull : armGetSystemTick() + armNsToTicks((u64)timeout_ms * 1000000ull);
  for (;;) {
    uint32_t seen = dcr_fd_seq();
    int sockets = 0;
    int r = poll_once(fds, n, &sockets);
    if (r || timeout_ms == 0)
      return r;
    u64 now = armGetSystemTick();
    if (timeout_ms > 0 && now >= deadline)
      return 0;
    s64 wait = timeout_ms < 0 ? -1 : (s64)armTicksToNs(deadline - now);
    if (sockets && (wait < 0 || wait > SOCKET_SLICE_NS))
      wait = SOCKET_SLICE_NS;
    dcr_fd_wait(seen, wait);
  }
}

typedef struct { uint32_t bits[32]; } b_fd_set; /* 1024 fds */

int b_select(int nfds, b_fd_set *rd, b_fd_set *wr, b_fd_set *ex, struct b_timeval *tv) {
  u64 limit = tv ? (u64)tv->tv_sec * 1000000000ull + (u64)tv->tv_usec * 1000ull : ~0ull;
  u64 start = armTicksToNs(armGetSystemTick());
  b_fd_set r0 = rd ? *rd : (b_fd_set){{0}}, w0 = wr ? *wr : (b_fd_set){{0}};
  for (;;) {
    uint32_t seen = dcr_fd_seq();
    int ready = 0, sockets = 0;
    if (rd) memset(rd, 0, sizeof *rd);
    if (wr) memset(wr, 0, sizeof *wr);
    if (ex) memset(ex, 0, sizeof *ex);
    for (int fd = 0; fd < nfds && fd < 1024; fd++) {
      int want_r = (r0.bits[fd / 32] >> (fd % 32)) & 1, want_w = (w0.bits[fd / 32] >> (fd % 32)) & 1;
      if (!want_r && !want_w)
        continue;
      struct b_pollfd p = {fd, (short)((want_r ? L_POLLIN : 0) | (want_w ? L_POLLOUT : 0)), 0};
      poll_once(&p, 1, &sockets);
      if ((p.revents & L_POLLIN) && rd) { rd->bits[fd / 32] |= 1u << (fd % 32); ready++; }
      if ((p.revents & L_POLLOUT) && wr) { wr->bits[fd / 32] |= 1u << (fd % 32); ready++; }
    }
    u64 spent = armTicksToNs(armGetSystemTick()) - start;
    if (ready || spent >= limit)
      return ready;
    s64 wait = limit == ~0ull ? -1 : (s64)(limit - spent);
    if (sockets && (wait < 0 || wait > SOCKET_SLICE_NS))
      wait = SOCKET_SLICE_NS;
    dcr_fd_wait(seen, wait);
  }
}

/* =============================== directories =============================== */
typedef struct {
  DIR *dir;
  struct b_dirent ent;
} BDir;

void *b_opendir(const char *path) {
  if (!path) {
    b_set_errno(L_EFAULT);
    return NULL;
  }
  char buf[DCR_PATH_MAX];
  const char *real = dcr_translate_path(path, buf, sizeof buf);
  if (dcr_dircache_missing(real)) {
    b_set_errno(L_ENOENT);
    return NULL;
  }
  DIR *d = opendir(real);
  if (!d) {
    b_fix_errno();
    return NULL;
  }
  BDir *bd = calloc(1, sizeof *bd);
  if (!bd) {
    closedir(d);
    b_set_errno(L_ENOMEM);
    return NULL;
  }
  bd->dir = d;
  return bd;
}

struct b_dirent *b_readdir(void *h) {
  BDir *bd = h;
  if (!bd)
    return NULL;
  struct dirent *e = readdir(bd->dir);
  if (!e)
    return NULL;
  memset(&bd->ent, 0, sizeof bd->ent);
  bd->ent.d_ino = 1;
  bd->ent.d_reclen = sizeof bd->ent;
  bd->ent.d_type = e->d_type == DT_DIR ? L_DT_DIR : e->d_type == DT_REG ? L_DT_REG : L_DT_UNKNOWN;
  snprintf(bd->ent.d_name, sizeof bd->ent.d_name, "%s", e->d_name);
  return &bd->ent;
}

int b_closedir(void *h) {
  BDir *bd = h;
  if (!bd)
    return -1;
  closedir(bd->dir);
  free(bd);
  return 0;
}

/* ================================ sendfile ================================ */
ssize_t b_sendfile(int out_fd, int in_fd, b_off_t *off, size_t count) {
  char buf[16384];
  size_t done = 0;
  if (off && b_lseek(in_fd, *off, SEEK_SET) < 0)
    return -1;
  while (done < count) {
    size_t k = count - done < sizeof buf ? count - done : sizeof buf;
    ssize_t r = b_read(in_fd, buf, k);
    if (r <= 0)
      break;
    ssize_t w = b_write(out_fd, buf, (size_t)r);
    if (w <= 0)
      break;
    done += (size_t)w;
  }
  if (off)
    *off += (b_off_t)done;
  return (ssize_t)done;
}

/* Read `len` bytes at absolute `off` without disturbing the fd position; used
 * by mmap() of files (bionic_mem.c) and pread. Returns bytes read.
 *
 * mmap() never moves a file's position on Linux, so an engine may map part of
 * a file while another of its threads seeks and reads the same fd (its own
 * lock covers only its seek+read pairs). Borrowing the fd's position here --
 * seek, read, seek back -- could then hand either side bytes from the other's
 * offset: an asset bundle read in place from the APK came out "corrupted"
 * (emulator, 2026-09-29). So the fd's position is never touched: the APK is
 * read through its block cache (its own handle), any other file through a
 * handle opened for this read. Only when that open fails (a file open for
 * writing: Horizon allows one handle then) is the position borrowed, and
 * logged. */
size_t b_pread_all(int fd, void *buf, size_t len, b_off64_t off) {
  FakeFd *f = fake_get(fd);
  if (f) {
    if (f->kind != FK_MEM || off >= (b_off64_t)f->size)
      return 0;
    size_t k = len < f->size - (size_t)off ? len : f->size - (size_t)off;
    memcpy(buf, f->data + off, k);
    return k;
  }
  if (fd >= 0 && fd < APK_FDS && g_apk_fd[fd]) {
    size_t done = 0;
    while (done < len) {
      ssize_t r = dcr_apkcache_read((uint64_t)off + done, (char *)buf + done, len - done);
      if (r <= 0)
        break;
      done += (size_t)r;
    }
    if (done)
      return done;
  }
  char path[DCR_PATH_MAX];
  path[0] = 0;
  mutexLock(&g_open_lock);
  for (int i = 0; i < OPEN_PATHS; i++)
    if (g_open[i].path && g_open[i].fd == fd) {
      snprintf(path, sizeof path, "%s", g_open[i].path);
      break;
    }
  mutexUnlock(&g_open_lock);
  if (path[0]) {
    int own = open(path, O_RDONLY);
    if (own >= 0) {
      size_t done = 0;
      if (lseek(own, (off_t)off, SEEK_SET) >= 0)
        while (done < len) {
          ssize_t r = read(own, (char *)buf + done, len - done);
          if (r <= 0)
            break;
          done += (size_t)r;
        }
      close(own);
      return done;
    }
  }
  static int logged;
  if (logged++ < 8)
    debugPrintf("[io] pread on fd %d (%s) through its own position\n", fd, path[0] ? path : "path unknown");
  off_t save = lseek(fd, 0, SEEK_CUR);
  if (lseek(fd, (off_t)off, SEEK_SET) < 0)
    return 0;
  size_t done = 0;
  while (done < len) {
    ssize_t r = read(fd, (char *)buf + done, len - done);
    if (r <= 0)
      break;
    done += (size_t)r;
  }
  lseek(fd, save, SEEK_SET);
  return done;
}

/* pread(): bionic's 32-bit off_t. Two threads reading one descriptor at
 * different places this way do not disturb each other (b_pread_all). */
ssize_t b_pread(int fd, void *buf, size_t n, b_off_t off) {
  if (off < 0) {
    b_set_errno(L_EINVAL);
    return -1;
  }
  return (ssize_t)b_pread_all(fd, buf, n, (b_off64_t)off);
}

int b_dup(int fd) { return b_fcntl(fd, L_F_DUPFD, 0); }

int b_readdir_r(void *dir, struct b_dirent *entry, struct b_dirent **result) {
  struct b_dirent *e = b_readdir(dir);
  if (!e) {
    if (result)
      *result = NULL;
    return 0;
  }
  if (entry)
    memcpy(entry, e, sizeof *entry);
  if (result)
    *result = entry;
  return 0;
}
