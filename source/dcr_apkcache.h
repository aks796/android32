/* dcr_apkcache.h -- the APK's bytes kept in RAM (dcr_apkcache.c). MIT. */
#ifndef DCR_APKCACHE_H
#define DCR_APKCACHE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* The APK the cache serves (the runtime's boot sets it once the APK is found;
 * it never changes while the game runs). */
void dcr_apkcache_set_path(const char *real);
/* `real` (a translated path) is that APK. */
int dcr_apkcache_is_apk(const char *real);
/* Bytes of the APK at `off` into buf: >= 0, or -1 to read the ordinary way. */
ssize_t dcr_apkcache_read(uint64_t off, void *buf, size_t n);
/* The APK's size, or -1 if the cache cannot open it. */
long long dcr_apkcache_size(void);
/* One line in debug.log: how much came from RAM. */
void dcr_apkcache_report(void);

#endif
