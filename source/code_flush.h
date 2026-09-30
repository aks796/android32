/* code_flush.h -- cache maintenance for code written at run time (32-bit). MIT. */
#ifndef DCR_CODE_FLUSH_H
#define DCR_CODE_FLUSH_H
#include <stddef.h>

/* libnx32's armICacheInvalidate is a no-op before 4.12.0 (AArch32 EL0 has no
 * cache maintenance instructions). Use these for any code written at run time. */
void dcr_icache_invalidate(void);             /* every core, whole I-cache */
void dcr_code_flush(void *code, size_t size); /* clean D-cache + invalidate I */

/* CALLBACK (weak; default 0), first in dcr_code_flush: return 1 when the port
 * did the whole flush of [code, code+size) itself -- for code it writes
 * through another view (the Crossy Road port's JIT arena cleans its RW alias,
 * or remaps the pages under an emulator); it may call dcr_icache_invalidate. */
int port_code_flush(void *code, size_t size);

#endif
