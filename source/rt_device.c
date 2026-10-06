/* rt_device.c -- this console's id (rt_device.h), and SHA-256.
 *
 * Moved here from Labyrinth 2 (lab_online.c), Labyrinth 1 (l1_online.c) and
 * Angry Birds Seasons (abse_device.c), which each had a copy: the steps, the
 * seeds and the formatting are theirs, so an id kept or made by any of them
 * comes out the same.
 *
 * RT_DEVICE_ID_SALT starts each seed: PORT_NAME by default, which is what
 * those ports used ("labyrinth2_nx", "abseasons_nx"). MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "dcr_path.h"
#include "rt_device.h"
#include "rt_settings.h"
#include "util.h"

#ifndef RT_DEVICE_ID_SALT
#define RT_DEVICE_ID_SALT PORT_NAME
#endif

__attribute__((weak)) const char *port_device_id_override(void) { return NULL; }

/* ------------------------------------------------------------------ SHA-256 */
static uint32_t ror32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(uint32_t h[8], const uint8_t *p) {
  static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t w[64];
  /* The big-endian words through memcpy + bswap: GCC turns the four-byte
   * shift-or into NEON VSHLL.I8, which Ryujinx's A32 decoder rejects
   * (Angry Birds Seasons hit it). */
  memcpy(w, p, 64);
  for (int i = 0; i < 16; i++)
    w[i] = __builtin_bswap32(w[i]);
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
    uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void rt_sha256(const void *data, size_t n, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const uint8_t *p = data;
  size_t left = n;
  for (; left >= 64; left -= 64, p += 64)
    sha256_block(h, p);
  uint8_t tail[128] = {0};
  memcpy(tail, p, left);
  tail[left] = 0x80;
  size_t tl = left < 56 ? 64 : 128;
  uint64_t bits = (uint64_t)n * 8;
  for (int i = 0; i < 8; i++)
    tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
  sha256_block(h, tail);
  if (tl == 128)
    sha256_block(h, tail + 64);
  for (int i = 0; i < 8; i++)
    out[i * 4] = (uint8_t)(h[i] >> 24), out[i * 4 + 1] = (uint8_t)(h[i] >> 16), out[i * 4 + 2] = (uint8_t)(h[i] >> 8),
                  out[i * 4 + 3] = (uint8_t)h[i];
}

/* ------------------------------------------------------------ the device id */
static char g_id[33];
static Mutex g_lock;

static void hash16(const char *seed, char *out) {
  uint8_t d[32];
  rt_sha256(seed, strlen(seed), d);
  for (int i = 0; i < 8; i++)
    snprintf(out + i * 2, 3, "%02x", d[i]);
}

/* a hidden serial reads as nothing, or a three-letter prefix and zeros */
static int serial_is_real(const char *s) {
  size_t n = strlen(s);
  if (n < 10)
    return 0;
  for (size_t i = 3; i < n; i++)
    if (s[i] != '0')
      return 1;
  return 0;
}

/* the user the game was started with, else the last one to open anything,
 * else the first on the console */
static int console_user(AccountUid *uid) {
  if (R_FAILED(accountInitialize(AccountServiceType_Application)))
    return 0;
  int ok = R_SUCCEEDED(accountGetPreselectedUser(uid)) && accountUidIsValid(uid);
  if (!ok)
    ok = R_SUCCEEDED(accountGetLastOpenedUser(uid)) && accountUidIsValid(uid);
  if (!ok) {
    AccountUid users[ACC_USER_LIST_SIZE];
    s32 count = 0;
    if (R_SUCCEEDED(accountListAllUsers(users, ACC_USER_LIST_SIZE, &count)) && count > 0) {
      *uid = users[0];
      ok = accountUidIsValid(uid);
    }
  }
  accountExit();
  return ok;
}

static const char *make_id(void) {
  SetSysSerialNumber serial;
  memset(&serial, 0, sizeof serial);
  int have = 0;
  /* Ryujinx has no GetSerialNumber (set:sys 68): it stops the emulator */
  if (!dcr_is_emulator()) {
    have = R_SUCCEEDED(setsysInitialize()) && R_SUCCEEDED(setsysGetSerialNumber(&serial));
    setsysExit();
  }
  serial.number[sizeof serial.number - 1] = 0;
  char seed[96];
  if (have && serial_is_real(serial.number)) {
    snprintf(seed, sizeof seed, RT_DEVICE_ID_SALT ":%s", serial.number);
    hash16(seed, g_id);
    return "the console's serial number";
  }
  AccountUid uid;
  if (console_user(&uid)) {
    snprintf(seed, sizeof seed, RT_DEVICE_ID_SALT ":user:%016llx%016llx", (unsigned long long)uid.uid[0],
             (unsigned long long)uid.uid[1]);
    hash16(seed, g_id);
    return "the console's user (the serial number is hidden)";
  }
  uint64_t r[2];
  randomGet(r, sizeof r);
  snprintf(g_id, sizeof g_id, "%016llx", (unsigned long long)(r[0] ^ (r[1] << 1)));
  return "a random number (no serial number or user to go by)";
}

static int is_hex_id(const char *s, size_t lo, size_t hi) {
  size_t n = strlen(s);
  if (n < lo || n > hi)
    return 0;
  for (size_t i = 0; i < n; i++)
    if (!strchr("0123456789abcdefABCDEF", s[i]))
      return 0;
  return 1;
}

const char *rt_device_id(void) {
  mutexLock(&g_lock);
  if (g_id[0]) {
    mutexUnlock(&g_lock);
    return g_id;
  }
  const char *set = port_device_id_override();
  if (set && set[0] && is_hex_id(set, 8, 16)) {
    snprintf(g_id, sizeof g_id, "%s", set);
    for (char *p = g_id; *p; p++)
      if (*p >= 'A' && *p <= 'F')
        *p = (char)(*p - 'A' + 'a');
    mutexUnlock(&g_lock);
    debugPrintf("[device] id: set by the player (config.ini)\n");
    return g_id;
  }
  char path[320];
  snprintf(path, sizeof path, "%s/data/device_id", dcr_game_root());
  FILE *f = fopen(path, "r");
  if (f) {
    char line[64] = {0};
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    line[strcspn(line, "\r\n ")] = 0;
    if (ok && is_hex_id(line, 8, 16)) {
      snprintf(g_id, sizeof g_id, "%s", line);
      mutexUnlock(&g_lock);
      debugPrintf("[device] id: kept in data/device_id\n");
      return g_id;
    }
  }
  const char *from = make_id();
  f = fopen(path, "w");
  int kept = f != NULL;
  if (f) {
    fprintf(f, "%s\n", g_id);
    fclose(f);
  }
  mutexUnlock(&g_lock);
  debugPrintf("[device] id: made from %s, kept in data/device_id%s\n", from, kept ? "" : " (could NOT be written)");
  return g_id;
}
