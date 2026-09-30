/* launcher_bar.c -- the launcher's progress screen: the game's name, why,
 * a green bar and the step, on the console, as the game program's own setup
 * screen draws it (source/util.c). Only redrawn when the percentage or the
 * step changes. (From the A8R launcher, where installing its zips takes
 * minutes.) MIT.
 */
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "launcher.h"
#include "rt_settings.h"

static int g_bar_on;

void launcher_header(void) {
  static const char title[] = PORT_TITLE " for Nintendo Switch -- launcher";
  char rule[sizeof title];
  memset(rule, '=', sizeof rule - 1);
  rule[sizeof rule - 1] = 0;
  printf("%s\n%s\n", title, rule);
#ifdef PORT_LAUNCHER_BYLINE
  printf("%s\n", PORT_LAUNCHER_BYLINE);
#endif
  printf("\n");
}

void launcher_bar(const char *what, int permille) {
  static char last[96];
  static int last_pct = -1;
  permille = permille < 0 ? 0 : permille > 1000 ? 1000 : permille;
  int pct = permille / 10;
  if (g_bar_on && pct == last_pct && !strncmp(what, last, sizeof last - 1))
    return;
  g_bar_on = 1;
  last_pct = pct;
  snprintf(last, sizeof last, "%s", what);
  enum { COLS = 80, BAR = 56 };
  int fill = permille * BAR / 1000;
  char bar[BAR + 1];
  for (int i = 0; i < BAR; i++)
    bar[i] = i < fill ? '#' : '-';
  bar[BAR] = 0;
  static const char title[] = PORT_TITLE;
  static const char note[] = PORT_SETUP_NOTE;
  printf("\x1b[2J");
  printf("\x1b[18;%dH\x1b[32;1m%s\x1b[0m", (COLS - (int)sizeof title + 1) / 2 + 1, title);
  printf("\x1b[20;%dH%s", (COLS - (int)sizeof note + 1) / 2 + 1, note);
  printf("\x1b[23;%dH[\x1b[32m%s\x1b[0m] %3d%%", (COLS - BAR - 7) / 2 + 1, bar, pct);
  int wl = (int)strlen(last);
  printf("\x1b[25;%dH%s", wl < COLS ? (COLS - wl) / 2 + 1 : 1, last);
  fflush(stdout);
  consoleUpdate(NULL);
}

void launcher_bar_off(void) {
  if (!g_bar_on)
    return;
  g_bar_on = 0;
  printf("\x1b[2J\x1b[1;1H");
  launcher_header();
  consoleUpdate(NULL);
}

int launcher_bar_on(void) { return g_bar_on; }
