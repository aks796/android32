/* bionic.h -- the Android bionic ABI as armeabi-v7a code sees it (32-bit ARM).
 *
 * The game's modules were compiled against bionic. We implement their imports
 * on top of newlib + libnx32, and the two C libraries disagree about almost
 * every type that crosses a system call (measured with the toolchain):
 *
 *                     newlib (devkitARM)   bionic (armeabi-v7a)
 *   time_t / off_t    8 / 8                4 / 4
 *   struct timespec   16                   8
 *   struct timeval    16                   8
 *   struct tm         36                   44  (+tm_gmtoff, tm_zone)
 *   struct stat       96                   104 (the ARM stat64 layout)
 *   struct dirent     264                  280
 *   pthread_mutex_t   16                   4
 *   sem_t             12                   4
 *   jmp_buf           160                  256
 *   O_CREAT           0x200                0x40   (and every other open flag)
 *   CLOCK_MONOTONIC   4                    1
 *   errno > 34        newlib numbering     Linux numbering
 *
 * So every shim that passes one of these converts explicitly, using the types
 * below. Nothing here is guessed from the NDK headers alone: the struct layouts
 * are pinned with static assertions, and the constants are the Linux ARM ABI.
 * MIT.
 */
#ifndef DCR_BIONIC_H
#define DCR_BIONIC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define BIONIC_STATIC_ASSERT(c, m) _Static_assert(c, m)

/* ---- scalar types ---------------------------------------------------------- */
typedef int32_t b_time_t;
typedef int32_t b_off_t;
typedef int64_t b_off64_t;
typedef int32_t b_clock_t;
typedef int32_t b_suseconds_t;
typedef int32_t b_pid_t;
typedef uint32_t b_uid_t;
typedef uint32_t b_gid_t;
typedef uint32_t b_mode_t;   /* bionic 32-bit: unsigned short on some, but passed as int */
typedef long b_pthread_t;    /* pointer to our thread record */

/* ---- time ------------------------------------------------------------------ */
struct b_timespec { int32_t tv_sec; int32_t tv_nsec; };
struct b_timeval  { int32_t tv_sec; int32_t tv_usec; };
struct b_itimerval { struct b_timeval it_interval, it_value; };
struct b_tm {
  int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
  long tm_gmtoff;
  const char *tm_zone;
};
struct b_utimbuf { b_time_t actime, modtime; };
BIONIC_STATIC_ASSERT(sizeof(struct b_timespec) == 8, "timespec");
BIONIC_STATIC_ASSERT(sizeof(struct b_tm) == 44, "tm");

/* Linux clock ids */
#define L_CLOCK_REALTIME           0
#define L_CLOCK_MONOTONIC          1
#define L_CLOCK_PROCESS_CPUTIME_ID 2
#define L_CLOCK_THREAD_CPUTIME_ID  3
#define L_CLOCK_MONOTONIC_RAW      4
#define L_CLOCK_REALTIME_COARSE    5
#define L_CLOCK_MONOTONIC_COARSE   6
#define L_CLOCK_BOOTTIME           7

/* ---- files ------------------------------------------------------------------ */
struct b_stat {                   /* ARM __STAT64_BODY */
  unsigned long long st_dev;
  unsigned char __pad0[4];
  unsigned long __st_ino;
  unsigned int st_mode;
  unsigned int st_nlink;
  b_uid_t st_uid;
  b_gid_t st_gid;
  unsigned long long st_rdev;
  unsigned char __pad3[4];
  long long st_size;
  unsigned long st_blksize;
  unsigned long long st_blocks;
  struct b_timespec st_atim;
  struct b_timespec st_mtim;
  struct b_timespec st_ctim;
  unsigned long long st_ino;
};
BIONIC_STATIC_ASSERT(sizeof(struct b_stat) == 104, "struct stat must be the 104-byte ARM layout");
BIONIC_STATIC_ASSERT(offsetof(struct b_stat, st_mode) == 16, "st_mode");
BIONIC_STATIC_ASSERT(offsetof(struct b_stat, st_size) == 48, "st_size");
BIONIC_STATIC_ASSERT(offsetof(struct b_stat, st_mtim) == 80, "st_mtim");

struct b_dirent {
  uint64_t d_ino;
  int64_t d_off;
  unsigned short d_reclen;
  unsigned char d_type;
  char d_name[256];
};
BIONIC_STATIC_ASSERT(sizeof(struct b_dirent) == 280, "dirent");
#define L_DT_UNKNOWN 0
#define L_DT_DIR     4
#define L_DT_REG     8

struct b_statfs {                 /* arm: the statfs64 shape */
  uint32_t f_type, f_bsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint32_t f_namelen, f_frsize, f_flags;
  uint32_t f_spare[4];
};

/* Linux ARM open() flags */
#define L_O_ACCMODE   00000003
#define L_O_RDONLY    00000000
#define L_O_WRONLY    00000001
#define L_O_RDWR      00000002
#define L_O_CREAT     00000100
#define L_O_EXCL      00000200
#define L_O_NOCTTY    00000400
#define L_O_TRUNC     00001000
#define L_O_APPEND    00002000
#define L_O_NONBLOCK  00004000
#define L_O_DIRECTORY 00040000   /* ARM differs from x86 here */
#define L_O_NOFOLLOW  00100000
#define L_O_LARGEFILE 00400000
#define L_O_CLOEXEC   02000000

/* Linux fcntl */
#define L_F_DUPFD 0
#define L_F_GETFD 1
#define L_F_SETFD 2
#define L_F_GETFL 3
#define L_F_SETFL 4
#define L_F_GETLK 5
#define L_F_SETLK 6
#define L_F_SETLKW 7
#define L_F_DUPFD_CLOEXEC 1030

/* Linux mode bits (same values as newlib, listed for clarity) */
#define L_S_IFMT  0170000
#define L_S_IFDIR 0040000
#define L_S_IFREG 0100000
#define L_S_IFCHR 0020000
#define L_S_IFIFO 0010000

/* ---- memory ---------------------------------------------------------------- */
#define L_PROT_READ  1
#define L_PROT_WRITE 2
#define L_PROT_EXEC  4
#define L_MAP_SHARED    0x01
#define L_MAP_PRIVATE   0x02
#define L_MAP_FIXED     0x10
#define L_MAP_ANONYMOUS 0x20
#define L_MAP_NORESERVE 0x4000
#define L_MREMAP_MAYMOVE 1
#define L_MAP_FAILED ((void *)-1)

/* ---- errno (Linux asm-generic numbering) ----------------------------------- */
#define L_EPERM 1
#define L_ENOENT 2
#define L_EINTR 4
#define L_EIO 5
#define L_EBADF 9
#define L_EAGAIN 11
#define L_ENOMEM 12
#define L_EACCES 13
#define L_EFAULT 14
#define L_EBUSY 16
#define L_EEXIST 17
#define L_ENOTDIR 20
#define L_EISDIR 21
#define L_EINVAL 22
#define L_EMFILE 24
#define L_ENOTTY 25
#define L_ENOSPC 28
#define L_ESPIPE 29
#define L_EPIPE 32
#define L_ERANGE 34
#define L_EDEADLK 35
#define L_ENAMETOOLONG 36
#define L_ENOLCK 37
#define L_ENOSYS 38
#define L_ENOTEMPTY 39
#define L_ELOOP 40
#define L_EOVERFLOW 75
#define L_EILSEQ 84
#define L_ENOTSOCK 88
#define L_EOPNOTSUPP 95
#define L_EAFNOSUPPORT 97
#define L_ENETDOWN 100
#define L_ENETUNREACH 101
#define L_ECONNRESET 104
#define L_ENOTCONN 107
#define L_ETIMEDOUT 110
#define L_ECONNREFUSED 111
#define L_EHOSTUNREACH 113
#define L_EALREADY 114
#define L_EINPROGRESS 115
#define L_ESRCH 3
#define L_ECHILD 10

/* newlib errno value -> Linux errno value (identical below 35). */
int b_errno_to_linux(int e);
/* Set the (per-thread, newlib-backed) errno the game reads through __errno(). */
void b_set_errno(int linux_errno);
/* Translate the newlib errno a pass-through call just set. */
void b_fix_errno(void);

/* bionic_stdio.c: stdout/stderr go to the log, a line repeated back to back
 * once plus a count; this writes out a pending count. */
void dcr_stdio_flush_repeats(void);

/* bionic_printf.c: the string printf family, NULL-safe for %s as bionic's is
 * (RT_NULL_SAFE_PRINTF). b_safe_format returns fmt, or a rewrite of it in buf
 * with each NULL %s printed as "(null)". */
int b_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
const char *b_safe_format(const char *fmt, va_list ap, char *buf, size_t cap);

/* ---- profiling callbacks ----------------------------------------------------
 * Weak: the defaults do nothing (bionic_zlib.c). A port that keeps load-time
 * counters defines both; the runtime brackets its costly shims with them:
 *   uint64_t t0 = port_prof_begin(); ...; port_prof_end(RT_PROF_INFLATE, t0, bytes);
 * The counter numbers are the runtime's; a port maps them to its own. */
enum {
  RT_PROF_INFLATE, /* zlib inflate / uncompress (bionic_zlib.c): bytes out */
  RT_PROF_TEX,     /* texture uploads */
  RT_PROF_CTEX,    /* compressed texture uploads */
  RT_PROF_COMPILE, /* shader compiles */
  RT_PROF_LINK,    /* program links */
  RT_PROF_DRAW,    /* draw calls */
  RT_PROF_MIPMAP,  /* mipmap generation */
  RT_PROF_BUFFER,  /* buffer uploads */
  RT_PROF_COUNT
};
uint64_t port_prof_begin(void);
void port_prof_end(int counter, uint64_t t0, uint64_t bytes);

/* ---- signals (Linux ARM) --------------------------------------------------- */
#define L_SIGILL  4
#define L_SIGABRT 6
#define L_SIGBUS  7
#define L_SIGFPE  8
#define L_SIGKILL 9
#define L_SIGSEGV 11
#define L_SIGPIPE 13
#define L_SIGALRM 14
#define L_SIGCHLD 17
#define L_SIGXCPU 24   /* Boehm SIG_THR_RESTART */
#define L_SIGPWR  30   /* Boehm SIG_SUSPEND */
#define L_NSIG    65

struct b_sigaction {
  void *sa_handler_or_action;
  uint32_t sa_mask;           /* sigset_t = unsigned long on 32-bit bionic */
  int sa_flags;
  void (*sa_restorer)(void);
};
#define L_SA_SIGINFO 4

/* ---- threads (32-bit bionic: every object is ONE 32-bit word) -------------- */
typedef struct { int32_t value; } b_pthread_mutex_t;
typedef struct { int32_t value; } b_pthread_cond_t;
typedef long b_pthread_mutexattr_t;
typedef long b_pthread_condattr_t;
typedef int b_pthread_key_t;
typedef int b_pthread_once_t;
typedef struct { uint32_t count; } b_sem_t;
typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} b_pthread_attr_t;
BIONIC_STATIC_ASSERT(sizeof(b_pthread_attr_t) == 24, "pthread_attr_t");
#define B_PTHREAD_ATTR_FLAG_DETACHED 1
#define B_PTHREAD_CREATE_JOINABLE 0
#define B_PTHREAD_CREATE_DETACHED 1
#define B_PTHREAD_MUTEX_NORMAL     0
#define B_PTHREAD_MUTEX_RECURSIVE  1
#define B_PTHREAD_MUTEX_ERRORCHECK 2
/* Static initialisers as the words bionic stores. */
#define B_MUTEX_INIT_NORMAL     0
#define B_MUTEX_INIT_RECURSIVE  0x4000
#define B_MUTEX_INIT_ERRORCHECK 0x8000

struct b_sched_param { int sched_priority; };

/* ---- misc structs ------------------------------------------------------------ */
struct b_passwd {
  char *pw_name, *pw_passwd;
  b_uid_t pw_uid;
  b_gid_t pw_gid;
  char *pw_dir, *pw_shell;
};
struct b_group { char *gr_name, *gr_passwd; b_gid_t gr_gid; char **gr_mem; };
struct b_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };
struct b_rusage {
  struct b_timeval ru_utime, ru_stime;
  long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap,
      ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } b_Dl_info;

/* bionic's 32-bit FILE is 84 bytes; __sF[0..2] are stdin/stdout/stderr. */
#define B_FILE_SIZE 84
extern unsigned char b___sF[3 * B_FILE_SIZE];

#endif /* DCR_BIONIC_H */
