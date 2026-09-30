/* fwd_mine.h -- is the title this NRO runs in a forwarder made for it?
 *
 * A sphaira forwarder is a title (id 05xx...) whose romfs names the NRO it
 * starts: /nextNroPath (and /nextArgv). An NRO started from a homebrew menu
 * runs inside whatever title that menu runs in, so the title id alone is not
 * enough: sphaira or hbmenu started from their own forwarder icon are 05xx
 * titles too, and so is the program sphaira makes for "launch with other CPU
 * cores" (its romfs also has /redirectProgramId). Installing the game there
 * would take over that icon. So the launcher only installs when the title's
 * /nextNroPath is this NRO. MIT.
 */
#ifndef FWD_MINE_H
#define FWD_MINE_H

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <switch.h>

/* Path without quotes, spaces or "sdmc:" around it. */
static inline void fwd_norm_path(const char *in, char *out, size_t cap) {
  while (*in == ' ' || *in == '"')
    in++;
  if (!strncasecmp(in, "sdmc:", 5))
    in += 5;
  snprintf(out, cap, "%s", in);
  size_t n = strlen(out);
  while (n && (out[n - 1] == '"' || out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\r'))
    out[--n] = 0;
}

/* 1: the running title is a forwarder whose /nextNroPath is `self` (argv[0]).
 * 0: anything else, or it cannot be told. */
static inline int fwd_is_mine(const char *self) {
  if (!self || !*self || R_FAILED(romfsMountFromCurrentProcess("fwd")))
    return 0;
  int mine = 0;
  FILE *f = fopen("fwd:/redirectProgramId", "rb");
  if (f) {
    fclose(f); /* sphaira's core-launch program, not a forwarder */
  } else if ((f = fopen("fwd:/nextNroPath", "rb"))) {
    char buf[FS_MAX_PATH + 8] = {0}, a[FS_MAX_PATH + 8], b[FS_MAX_PATH + 8];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    fwd_norm_path(buf, a, sizeof a);
    fwd_norm_path(self, b, sizeof b);
    mine = a[0] && !strcasecmp(a, b);
  }
  romfsUnmount("fwd");
  return mine;
}

#endif
