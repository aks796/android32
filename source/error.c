/* error.c -- fatal error: log it, show it, exit. MIT. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>

#include "error.h"
#include "util.h"

void NORETURN fatal_error(const char *fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  debugPrintf("[fatal] %s\n", msg);
  log_flush_ring();

  /* The error applet needs the applet service; if it is not up (very early
   * failure) the log line above is all there is. */
  ErrorApplicationConfig c;
  if (R_SUCCEEDED(errorApplicationCreate(&c, "Labyrinth 2: fatal error", msg)))
    errorApplicationShow(&c);

  extern u32 __nx_applet_exit_mode;
  __nx_applet_exit_mode = 1;
  exit(1);
}
