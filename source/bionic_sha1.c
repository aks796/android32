/* bionic_sha1.c -- SHA1Init / SHA1Update / SHA1Final, as old bionic exported them.
 *
 * Android's libc before 4.4 exported the NetBSD SHA-1 (<sha1.h>), and Unity
 * 5.6's libunity imports it (2017.4's does not). The context is the caller's
 * memory, so its layout is bionic's:
 *   typedef struct { uint32_t state[5]; uint32_t count[2]; uint8_t buffer[64]; } SHA1_CTX;
 * (92 bytes; count[0] = low word of the bit count), and SHA1Final(digest[20],
 * ctx). The transform is the classic public-domain one (Steve Reid). MIT.
 */
#include <stdint.h>
#include <string.h>

typedef struct {
  uint32_t state[5];
  uint32_t count[2];
  uint8_t buffer[64];
} B_SHA1_CTX;
_Static_assert(sizeof(B_SHA1_CTX) == 92, "bionic SHA1_CTX");

#define ROL(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

/* Scalar on purpose: GCC vectorises the byte loads into VSHLL.I8 #8, which
 * Ryujinx 1.1.1098's A32 decoder lacks (and SHA-1 here hashes a few short
 * strings: nothing to gain). */
__attribute__((optimize("no-tree-vectorize")))
static void transform(uint32_t st[5], const uint8_t blk[64]) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)blk[4 * i] << 24 | (uint32_t)blk[4 * i + 1] << 16 | (uint32_t)blk[4 * i + 2] << 8 | blk[4 * i + 3];
  for (int i = 16; i < 80; i++)
    w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
    else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
    else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
    else f = b ^ c ^ d, k = 0xCA62C1D6u;
    uint32_t t = ROL(a, 5) + f + e + k + w[i];
    e = d, d = c, c = ROL(b, 30), b = a, a = t;
  }
  st[0] += a, st[1] += b, st[2] += c, st[3] += d, st[4] += e;
}

void b_SHA1Init(B_SHA1_CTX *c) {
  static const uint32_t iv[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  memcpy(c->state, iv, sizeof iv);
  c->count[0] = c->count[1] = 0;
}

void b_SHA1Update(B_SHA1_CTX *c, const uint8_t *data, uint32_t len) {
  uint32_t j = (c->count[0] >> 3) & 63;
  if ((c->count[0] += len << 3) < (len << 3))
    c->count[1]++;
  c->count[1] += len >> 29;
  uint32_t i = 0;
  if (j + len > 63) {
    i = 64 - j;
    memcpy(&c->buffer[j], data, i);
    transform(c->state, c->buffer);
    for (; i + 63 < len; i += 64)
      transform(c->state, data + i);
    j = 0;
  }
  memcpy(&c->buffer[j], data + i, len - i);
}

void b_SHA1Final(uint8_t digest[20], B_SHA1_CTX *c) {
  uint8_t cnt[8];
  for (int i = 0; i < 8; i++) /* big-endian bit count, high word first */
    cnt[i] = (uint8_t)(c->count[i >= 4 ? 0 : 1] >> ((3 - (i & 3)) * 8));
  uint8_t pad = 0x80;
  b_SHA1Update(c, &pad, 1);
  pad = 0;
  while ((c->count[0] & 504) != 448)
    b_SHA1Update(c, &pad, 1);
  b_SHA1Update(c, cnt, 8);
  for (int i = 0; i < 20; i++)
    digest[i] = (uint8_t)(c->state[i >> 2] >> ((3 - (i & 3)) * 8));
  memset(c, 0, sizeof *c);
}
