/* dcr_exefs.h -- the ExeFS override, built on the console.
 *
 * Atmosphere replaces an installed title's whole ExeFS with
 * /atmosphere/contents/<title id>/exefs.nsp. The wrapper (pvz_nx.nsp: a PFS0
 * holding the 32-bit `main` NSO and `main.npdm`) becomes that file for a
 * forwarder title once main.npdm's program id is the forwarder's: the loader
 * checks ACI0's program id and the ACID's allowed range. This is
 * tools/make_exefs_override.py in C, header-only so that both the 64-bit
 * launcher NRO (launcher/) and the 32-bit wrapper (dcr_setup.c) use it.
 * No libnx, no allocation except the one result buffer. MIT.
 */
#ifndef DCR_EXEFS_H
#define DCR_EXEFS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t exefs_rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t exefs_rd64(const uint8_t *p) {
  return (uint64_t)exefs_rd32(p) | (uint64_t)exefs_rd32(p + 4) << 32;
}
static inline void exefs_wr32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}
static inline void exefs_wr64(uint8_t *p, uint64_t v) {
  exefs_wr32(p, (uint32_t)v);
  exefs_wr32(p + 4, (uint32_t)(v >> 32));
}

/* Number of files in a PFS0 image, or -1 if it is not a well-formed one. */
static inline int exefs_count(const uint8_t *pfs, size_t len) {
  if (len < 16 || memcmp(pfs, "PFS0", 4))
    return -1;
  uint32_t n = exefs_rd32(pfs + 4), strsz = exefs_rd32(pfs + 8);
  if (n > 64 || 16 + 24ull * n + strsz > len)
    return -1;
  return (int)n;
}

/* File i of a PFS0: name, data and size; 0 on success. */
static inline int exefs_file(const uint8_t *pfs, size_t len, int i, const char **name,
                             const uint8_t **data, size_t *size) {
  int n = exefs_count(pfs, len);
  if (n < 0 || i < 0 || i >= n)
    return -1;
  uint32_t strsz = exefs_rd32(pfs + 8);
  size_t table = 16 + 24 * (size_t)n, start = table + strsz;
  const uint8_t *e = pfs + 16 + 24 * (size_t)i;
  uint64_t off = exefs_rd64(e), sz = exefs_rd64(e + 8);
  uint32_t no = exefs_rd32(e + 16);
  if (no >= strsz || start + off + sz > len || !memchr(pfs + table + no, 0, strsz - no))
    return -1;
  *name = (const char *)pfs + table + no;
  *data = pfs + start + off;
  *size = (size_t)sz;
  return 0;
}

static inline int exefs_find(const uint8_t *pfs, size_t len, const char *want, const uint8_t **data,
                             size_t *size) {
  int n = exefs_count(pfs, len);
  for (int i = 0; i < n; i++) {
    const char *nm;
    if (exefs_file(pfs, len, i, &nm, data, size) == 0 && !strcmp(nm, want))
      return 0;
  }
  return -1;
}

/* main.npdm's program id (ACI0) and whether it is a 64-bit process; the
 * layout checks of make_exefs_override.py. 0 on success. */
static inline int npdm_info(const uint8_t *npdm, size_t len, uint64_t *pid, int *is64) {
  if (len < 0x80 || memcmp(npdm, "META", 4))
    return -1;
  uint32_t aci0 = exefs_rd32(npdm + 0x70), acid = exefs_rd32(npdm + 0x78);
  if ((size_t)aci0 + 0x18 > len || (size_t)acid + 0x220 > len || memcmp(npdm + aci0, "ACI0", 4) ||
      memcmp(npdm + acid + 0x200, "ACID", 4))
    return -1;
  *pid = exefs_rd64(npdm + aci0 + 0x10);
  *is64 = npdm[0xC] & 1;
  return 0;
}

/* The override for title `tid`: every file of `nsp` in its order, main.npdm
 * retargeted (ACI0 program id, ACID program id range). A malloc'd PFS0 in
 * *out; 0 on success, -1 if `nsp` is not the 32-bit wrapper. */
static inline int exefs_build_override(const uint8_t *nsp, size_t len, uint64_t tid, uint8_t **out,
                                       size_t *out_len) {
  int n = exefs_count(nsp, len);
  const uint8_t *npdm, *d;
  size_t npdm_len, sz;
  uint64_t old;
  int is64;
  if (n < 2 || exefs_find(nsp, len, "main", &d, &sz) || exefs_find(nsp, len, "main.npdm", &npdm, &npdm_len) ||
      npdm_info(npdm, npdm_len, &old, &is64) || is64)
    return -1;
  /* the string table and data are rebuilt exactly as write_pfs0() does */
  size_t strsz = 0, data = 0;
  for (int i = 0; i < n; i++) {
    const char *nm;
    if (exefs_file(nsp, len, i, &nm, &d, &sz))
      return -1;
    strsz += strlen(nm) + 1;
    data += sz;
  }
  size_t header = 16 + 24 * (size_t)n + strsz;
  strsz += (0x20 - header % 0x20) % 0x20; /* data 0x20-aligned */
  header = 16 + 24 * (size_t)n + strsz;
  uint8_t *o = calloc(1, header + data);
  if (!o)
    return -1;
  memcpy(o, "PFS0", 4);
  exefs_wr32(o + 4, (uint32_t)n);
  exefs_wr32(o + 8, (uint32_t)strsz);
  size_t name_off = 0, data_off = 0;
  for (int i = 0; i < n; i++) {
    const char *nm;
    exefs_file(nsp, len, i, &nm, &d, &sz);
    uint8_t *e = o + 16 + 24 * (size_t)i;
    exefs_wr64(e, data_off);
    exefs_wr64(e + 8, sz);
    exefs_wr32(e + 16, (uint32_t)name_off);
    memcpy(o + 16 + 24 * (size_t)n + name_off, nm, strlen(nm) + 1);
    name_off += strlen(nm) + 1;
    uint8_t *dst = o + header + data_off;
    memcpy(dst, d, sz);
    if (!strcmp(nm, "main.npdm")) {
      uint32_t aci0 = exefs_rd32(dst + 0x70), acid = exefs_rd32(dst + 0x78);
      exefs_wr64(dst + aci0 + 0x10, tid);
      exefs_wr64(dst + acid + 0x210, tid);
      exefs_wr64(dst + acid + 0x218, tid);
    }
    data_off += sz;
  }
  *out = o;
  *out_len = header + data;
  return 0;
}

/* A sphaira forwarder's title id (0x05 in the top byte), the only kind the
 * override is ever written for: never a real game's or a system title's. */
static inline int exefs_is_forwarder_tid(uint64_t tid) { return (tid >> 56) == 0x05; }

#endif
