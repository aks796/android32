/* error.h -- fatal error path (shows a message, then exits). MIT. */
#ifndef DCR_ERROR_H
#define DCR_ERROR_H

#include "util.h"

void NORETURN fatal_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* DCR_ERROR_H */
