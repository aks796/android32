/* code_flush.h -- cache maintenance for code written at run time (32-bit). */
#ifndef DCR_CODE_FLUSH_H
#define DCR_CODE_FLUSH_H
#include <stddef.h>

/* libnx32's armICacheInvalidate is a no-op (AArch32 EL0 has no cache
 * maintenance instructions). Use these for any code written at run time. */
void dcr_icache_invalidate(void);             /* every core, whole I-cache */
void dcr_code_flush(void *code, size_t size); /* clean D-cache + invalidate I */

#endif
