/* dcr_apkcache.c -- the APK's bytes kept in RAM across scene loads.
 *
 * Engines read their assets straight out of the player's APK, often the same
 * ones again and again: Unity re-reads ~55 MB of the APK for every reload of
 * a scene (a theme change in Disney Crossy Road: its loading thread spent
 * about half of a 2.5 s load blocked in SD-card reads, hardware 2026-09-24).
 *
 * So reads of the APK go through a block cache: 128 KB blocks, filled from
 * the SD card on first use by one FsFile of its own (so no fd's position is
 * disturbed), kept least-recently-used. The first load fills it; later ones
 * read RAM. The APK never changes while the game runs (it is found, adopted
 * or rewritten before anything reads it), so there is nothing to invalidate.
 *
 * The cache only GROWS while the heap has room to spare: never past
 * RT_APKCACHE_MAX_BLOCKS blocks (128 MB), and never when fewer than
 * RT_APKCACHE_KEEP_FREE bytes (256 MB) of the heap would be left (mallinfo:
 * the engine's and the GPU's allocations all come from this heap). Past that
 * it recycles its own least-recently-used blocks. MIT.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/types.h>

#include "dcr_apkcache.h"
#include "nx_init.h"
#include "rt_settings.h"
#include "util.h"

#define BLK_SHIFT 17 /* 128 KB */
#define BLK (1u << BLK_SHIFT)
/* The most blocks the cache holds (all ports: 1024 = 128 MB). */
#ifndef RT_APKCACHE_MAX_BLOCKS
#define RT_APKCACHE_MAX_BLOCKS 1024
#endif
/* The heap the cache leaves free (all ports: 256 MB). */
#ifndef RT_APKCACHE_KEEP_FREE
#define RT_APKCACHE_KEEP_FREE (256u << 20)
#endif
#define MAX_BLOCKS RT_APKCACHE_MAX_BLOCKS
#define KEEP_FREE RT_APKCACHE_KEEP_FREE
#define HASH 2048
_Static_assert(MAX_BLOCKS < HASH, "RT_APKCACHE_MAX_BLOCKS must stay below the hash table's 2048 slots");

typedef struct {
  uint32_t idx, len, stamp;
  uint8_t *data;
} Block;

static Block g_blk[MAX_BLOCKS];
static int g_nblk;
static int16_t g_hash[HASH]; /* block index -> slot + 1, 0 = empty (linear probing) */
static Mutex g_mx;
static FsFile g_file;
static int g_state; /* 0 not tried, 1 ready, -1 unavailable */
static uint64_t g_size;
static uint32_t g_clock;
static uint64_t g_hit, g_miss, g_direct;
static char g_path[512]; /* 256 cut long any-name APK paths: no match, no cache */

void dcr_apkcache_set_path(const char *real) { snprintf(g_path, sizeof g_path, "%s", real ? real : ""); }

int dcr_apkcache_is_apk(const char *real) { return g_path[0] && real && !strcmp(real, g_path); }

static int open_file(void) {
  if (g_state)
    return g_state > 0;
  g_state = -1;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  const char *p = strncmp(g_path, "sdmc:", 5) ? g_path : g_path + 5;
  s64 size = 0;
  Result rc = fs ? fsFsOpenFile(fs, p, FsOpenMode_Read, &g_file) : MAKERESULT(Module_Libnx, LibnxError_NotFound);
  if (R_FAILED(rc)) {
    debugPrintf("[apk] the APK is not cached (open 0x%x): read from the card as it is\n", (unsigned)rc);
    return 0;
  }
  if (R_FAILED(fsFileGetSize(&g_file, &size)) || size <= 0) {
    fsFileClose(&g_file);
    return 0;
  }
  g_size = (uint64_t)size;
  g_state = 1;
  debugPrintf("[apk] reads of the APK (%llu MB) are cached in RAM, up to %u MB\n",
              (unsigned long long)(g_size >> 20), (unsigned)((MAX_BLOCKS * (uint64_t)BLK) >> 20));
  return 1;
}

static int find(uint32_t idx) {
  for (uint32_t h = (idx * 2654435761u) % HASH, k = 0; k < HASH; k++, h = (h + 1) % HASH) {
    if (!g_hash[h])
      return -1;
    if (g_blk[g_hash[h] - 1].idx == idx)
      return g_hash[h] - 1;
  }
  return -1;
}

static void hash_rebuild(void) {
  memset(g_hash, 0, sizeof g_hash);
  for (int i = 0; i < g_nblk; i++)
    for (uint32_t h = (g_blk[i].idx * 2654435761u) % HASH, k = 0; k < HASH; k++, h = (h + 1) % HASH)
      if (!g_hash[h]) {
        g_hash[h] = (int16_t)(i + 1);
        break;
      }
}

/* A slot for a new block: a fresh one while the heap can spare it, else the
 * least recently used. -1 if neither. */
static int slot_for_new(void) {
  if (g_nblk < MAX_BLOCKS) {
    struct mallinfo mi = mallinfo();
    uint64_t heap = g_nxinit.heap ? g_nxinit.heap : (992ull << 20);
    if ((uint64_t)mi.uordblks + BLK + KEEP_FREE <= heap) {
      uint8_t *d = malloc(BLK);
      if (d) {
        g_blk[g_nblk].data = d;
        g_blk[g_nblk].idx = ~0u;
        return g_nblk++;
      }
    }
  }
  if (!g_nblk)
    return -1;
  int lru = 0;
  for (int i = 1; i < g_nblk; i++)
    if (g_blk[i].stamp < g_blk[lru].stamp)
      lru = i;
  return lru;
}

static Block *get(uint32_t idx) {
  int i = find(idx);
  if (i >= 0) {
    g_blk[i].stamp = ++g_clock;
    return &g_blk[i];
  }
  i = slot_for_new();
  if (i < 0)
    return NULL;
  Block *b = &g_blk[i];
  u64 got = 0;
  uint64_t off = (uint64_t)idx << BLK_SHIFT;
  size_t want = off + BLK <= g_size ? BLK : (size_t)(g_size - off);
  int was = b->idx != ~0u;
  b->idx = ~0u;
  if (R_FAILED(fsFileRead(&g_file, (s64)off, b->data, want, FsReadOption_None, &got)) || got != want) {
    if (was)
      hash_rebuild(); /* the slot's old block is gone */
    return NULL;
  }
  b->idx = idx;
  b->len = (uint32_t)want;
  b->stamp = ++g_clock;
  hash_rebuild(); /* cheap next to a 128 KB read, and keeps probing simple */
  return b;
}

/* The APK's size, or -1 if the cache cannot open it. */
long long dcr_apkcache_size(void) {
  mutexLock(&g_mx);
  long long r = open_file() ? (long long)g_size : -1;
  mutexUnlock(&g_mx);
  return r;
}

/* Bytes of the APK at `off` into buf: >= 0, or -1 to read the ordinary way. */
ssize_t dcr_apkcache_read(uint64_t off, void *buf, size_t n) {
  mutexLock(&g_mx);
  if (!open_file()) {
    mutexUnlock(&g_mx);
    return -1;
  }
  if (off >= g_size) {
    mutexUnlock(&g_mx);
    return 0;
  }
  if (n > g_size - off)
    n = (size_t)(g_size - off);
  size_t done = 0;
  while (done < n) {
    uint64_t pos = off + done;
    uint32_t idx = (uint32_t)(pos >> BLK_SHIFT), in = (uint32_t)(pos & (BLK - 1));
    int hit = find(idx) >= 0;
    Block *b = get(idx);
    if (!b)
      break;
    size_t k = b->len > in ? b->len - in : 0;
    if (k > n - done)
      k = n - done;
    if (!k)
      break;
    memcpy((uint8_t *)buf + done, b->data + in, k);
    done += k;
    if (hit)
      g_hit += k;
    else
      g_miss += k;
  }
  if (done < n) {
    /* no block to be had: the rest straight from the card */
    u64 got = 0;
    if (R_SUCCEEDED(fsFileRead(&g_file, (s64)(off + done), (uint8_t *)buf + done, n - done,
                               FsReadOption_None, &got))) {
      done += (size_t)got;
      g_direct += got;
    }
  }
  mutexUnlock(&g_mx);
  return (ssize_t)done;
}

void dcr_apkcache_report(void) {
  if (g_state <= 0)
    return;
  struct mallinfo mi = mallinfo();
  debugPrintf("[apk] cache %d MB; read from RAM %llu MB, from the card %llu MB (+%llu MB uncached); heap "
              "in use %u MB\n",
              (int)(((uint64_t)g_nblk * BLK) >> 20), (unsigned long long)(g_hit >> 20),
              (unsigned long long)(g_miss >> 20), (unsigned long long)(g_direct >> 20),
              (unsigned)(mi.uordblks >> 20));
}
