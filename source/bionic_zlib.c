/* bionic_zlib.c -- the libz imports, on miniz (libnx32 ships libminiz).
 *
 * The PvZ engine uses zlib for its zip reader (zziplib: inflateInit2_ with raw
 * deflate, windowBits -15) and for compress/uncompress/deflate of its own data
 * (saves, replays). zlib's z_stream and
 * miniz's mz_stream have the same 56-byte layout on 32-bit ARM (all fields are
 * pointers, unsigned int and unsigned long), and the flush/return codes are
 * zlib's, so the stream the engine owns passes straight through.
 *
 * ONE BEHAVIOUR DIFFERS, and inflate() below makes up for it. zlib decodes
 * only what fits in avail_out, so while decoded bytes are still owed the
 * input is not used up (at the least, the end-of-block code and the 4-byte
 * Adler-32 are left). miniz decodes greedily into its own 32 KB window: it can
 * take ALL the input, trailer included, and still hold rows of output. libpng
 * 1.5.9 (png_read_row, png_read_finish_row) fetches the next IDAT chunk when
 * avail_in is 0: on an image's last chunk there is none, and every PNG ended
 * in "Not enough image data" (hardware run 3, 2026-09-24: images/ESRB_RATING,
 * then the engine's exit). So when inflate() fills the output and has used up
 * the input, it hands the last input byte back (avail_in = 1: "more to
 * come"); the next call takes it again (only if the caller left next_in and
 * avail_in alone) before miniz empties its window. The byte's address is kept
 * in z_stream.reserved, which zlib leaves to the library and miniz only
 * zeroes. MIT.
 */
#include <stdint.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz/miniz.h>

#include "util.h"

_Static_assert(sizeof(mz_stream) == 56, "mz_stream must match zlib's 32-bit z_stream");

/* windowBits 16+n (gzip) and 32+n (zlib or gzip, detected: what
 * Sexy::SexyAppBase::GetTexImage asks for) are zlib's; miniz takes only +-15.
 * Such a stream starts as raw deflate and is listed here until its first
 * inflate() has read the header: a zlib header re-opens it as zlib (miniz
 * parses that itself), a gzip header is skipped. The trailer is left unread. */
#define HDR_MAX 4
static struct { mz_streamp s; int want; } g_hdr[HDR_MAX]; /* want: 16 gzip, 32 either */

static int hdr_slot(mz_streamp s) {
  for (int i = 0; i < HDR_MAX; i++)
    if (g_hdr[i].s == s)
      return i;
  return -1;
}

/* Length of the gzip header at p (n bytes), 0 if incomplete, -1 if not gzip. */
static int gzip_header(const unsigned char *p, unsigned n) {
  if (n < 2)
    return 0;
  if (p[0] != 0x1f || p[1] != 0x8b)
    return -1;
  if (n < 10)
    return 0;
  if (p[2] != 8)
    return -1;
  unsigned flg = p[3], i = 10;
  if (flg & 4) { /* FEXTRA */
    if (n < i + 2)
      return 0;
    i += 2 + (p[i] | p[i + 1] << 8);
  }
  for (unsigned bit = 8; bit <= 16; bit <<= 1) /* FNAME, FCOMMENT: zero-terminated */
    if (flg & bit) {
      while (i < n && p[i])
        i++;
      if (i++ >= n)
        return 0;
    }
  if (flg & 2) /* FHCRC */
    i += 2;
  return i <= n ? (int)i : 0;
}

int b_inflateInit2_(mz_streamp strm, int window_bits, const char *version, int stream_size) {
  if (stream_size != (int)sizeof(mz_stream))
    return MZ_VERSION_ERROR;
  int want = window_bits >= 32 ? 32 : window_bits >= 16 ? 16 : 0;
  if (want)
    window_bits = -MZ_DEFAULT_WINDOW_BITS;
  else if (window_bits == 0)
    window_bits = MZ_DEFAULT_WINDOW_BITS; /* zlib: the window size from the header */
  int r = mz_inflateInit2(strm, window_bits);
  if (r != MZ_OK) {
    debugPrintf("[zlib] inflateInit2(windowBits=%d) failed: %d\n", window_bits, r);
    return r;
  }
  int i = hdr_slot(strm);
  if (i < 0 && want)
    i = hdr_slot(NULL);
  if (i >= 0)
    g_hdr[i].s = want ? strm : NULL, g_hdr[i].want = want;
  else if (want)
    debugPrintf("[zlib] more than %d gzip streams at once: this one read as raw\n", HDR_MAX);
  return r;
}

int b_inflate(mz_streamp strm, int flush) {
  int i = hdr_slot(strm);
  if (i >= 0) {
    int g = gzip_header(strm->next_in, strm->avail_in);
    if (g == 0)
      return MZ_BUF_ERROR; /* the whole header first */
    g_hdr[i].s = NULL;
    if (g < 0 && g_hdr[i].want == 16)
      return MZ_DATA_ERROR;
    mz_inflateEnd(strm);
    int r = mz_inflateInit2(strm, g > 0 ? -MZ_DEFAULT_WINDOW_BITS : MZ_DEFAULT_WINDOW_BITS);
    if (r != MZ_OK)
      return r;
    if (g > 0) {
      strm->next_in += g;
      strm->avail_in -= (unsigned)g;
      strm->total_in = (mz_ulong)g;
    }
  }

  /* the byte handed back last time, if the caller left it where it was */
  const unsigned char *held = (const unsigned char *)(uintptr_t)strm->reserved;
  strm->reserved = 0;
  if (held && !(strm->next_in == held && strm->avail_in == 1))
    held = NULL; /* the caller moved on to new input: nothing to take again */
  if (held) {
    strm->next_in++;
    strm->avail_in = 0;
    strm->total_in++;
  }

  int r = mz_inflate(strm, flush);
  if (held && r == MZ_BUF_ERROR)
    r = MZ_OK; /* no output, but the caller's "last" byte was taken: progress */

  /* Output full, input used up, not at the end: miniz may still hold decoded
   * bytes, which zlib would not have decoded yet -- keep the caller coming
   * back rather than fetching input that may not exist. A byte handed back
   * when nothing was pending costs one call that returns MZ_OK empty. */
  if (r == MZ_OK && strm->avail_in == 0 && strm->avail_out == 0 && strm->total_in > 0 &&
      strm->next_in) {
    strm->next_in--;
    strm->avail_in = 1;
    strm->total_in--;
    strm->reserved = (mz_ulong)(uintptr_t)strm->next_in;
  }
  return r;
}

int b_inflateEnd(mz_streamp strm) {
  int i = hdr_slot(strm);
  if (i >= 0)
    g_hdr[i].s = NULL;
  if (strm)
    strm->reserved = 0;
  return mz_inflateEnd(strm);
}

int b_inflateInit_(mz_streamp strm, const char *version, int stream_size) {
  return b_inflateInit2_(strm, MZ_DEFAULT_WINDOW_BITS, version, stream_size);
}
int b_inflateReset(mz_streamp strm) {
  if (strm)
    strm->reserved = 0; /* no byte held back (see b_inflate) */
  return mz_inflateReset(strm);
}

int b_deflateInit2_(mz_streamp strm, int level, int method, int window_bits, int mem_level,
                    int strategy, const char *version, int stream_size) {
  if (stream_size != (int)sizeof(mz_stream))
    return MZ_VERSION_ERROR;
  return mz_deflateInit2(strm, level, method, window_bits, mem_level, strategy);
}
int b_deflate(mz_streamp strm, int flush) { return mz_deflate(strm, flush); }
int b_deflateEnd(mz_streamp strm) { return mz_deflateEnd(strm); }
int b_deflateReset(mz_streamp strm) { return mz_deflateReset(strm); }

/* uLong is 32 bits here, as mz_ulong is */
int b_compress(unsigned char *dst, mz_ulong *dst_len, const unsigned char *src, mz_ulong src_len) {
  return mz_compress(dst, dst_len, src, src_len);
}
int b_uncompress(unsigned char *dst, mz_ulong *dst_len, const unsigned char *src, mz_ulong src_len) {
  return mz_uncompress(dst, dst_len, src, src_len);
}
mz_ulong b_crc32(mz_ulong crc, const unsigned char *buf, unsigned int len) {
  return buf ? mz_crc32(crc, buf, len) : 0;
}
const char *b_zError(int err) {
  const char *e = mz_error(err);
  return e ? e : "";
}
